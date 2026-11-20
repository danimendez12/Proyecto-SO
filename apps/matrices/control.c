#include "control.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>

enum {
    CONTROL_MAGIC = 0x4d415431,
    CONTROL_VERSION = 1,
    CONTROL_MAX_TRABAJADORES = 15,
    FASE_ARRANQUE = 1,
    FASE_DATOS = 2,
    FASE_ABORTADA = 3,
    MARCA_NUEVA = 0,
    MARCA_LISTA = 1,
    MARCA_FIN = 2
};

struct fase {
    uint32_t estado;
    int32_t resultado;
};

struct marca {
    uint32_t estado;
    int32_t resultado;
};

struct cabecera {
    uint32_t magic, version;
    matrices_control_datos_t datos;
    struct fase fase;
};

/* Una sola pagina DSM; nunca hay mutexes, atomicos ni punteros compartidos.
 * Las marcas tienen un escritor por nodo; MSI serializa las copias por pagina.
 * Ambos lados deben usar el ABI Linux x86-64 de la aplicacion. */
struct control {
    struct cabecera cabecera;
    struct marca marcas[CONTROL_MAX_TRABAJADORES];
};

_Static_assert(sizeof(struct control) <= 4096, "El control debe caber en una pagina");

static int datos_validos(const matrices_control_datos_t *d)
{
    return d != NULL && d->filas != 0 && d->interior != 0 && d->columnas != 0 &&
           d->trabajadores != 0 && d->trabajadores <= CONTROL_MAX_TRABAJADORES &&
           d->page_size >= sizeof(struct control);
}

static int ids_validos(const matrices_control_datos_t *d)
{
    return d->a != 0 && d->b != 0 && d->c != 0 &&
           (d->a >> 16) == 0 && (d->b >> 16) == 0 && (d->c >> 16) == 0 &&
           d->a != d->b && d->a != d->c && d->b != d->c;
}

static int leer_cabecera(dsm_context_t *ctx, dsm_reserva_id_t id, struct cabecera *c)
{
    if (id == 0 || (id >> 16) != 0) {
        return DSM_ERR_INVAL;
    }
    int rc = dsm_leer(ctx, id, 0, c, sizeof *c);
    if (rc != DSM_OK) {
        return rc;
    }
    if (c->magic != CONTROL_MAGIC || c->version != CONTROL_VERSION ||
        !datos_validos(&c->datos) ||
        (c->fase.estado != FASE_ARRANQUE && c->fase.estado != FASE_DATOS &&
         c->fase.estado != FASE_ABORTADA) ||
        (c->fase.estado == FASE_ABORTADA ? c->fase.resultado >= 0 : c->fase.resultado != 0)) {
        return DSM_ERR_PROTO;
    }
    return DSM_OK;
}

static int vigente(const struct timespec *plazo)
{
    if (plazo == NULL || plazo->tv_nsec < 0 || plazo->tv_nsec >= 1000000000L) {
        return DSM_ERR_INVAL;
    }
    struct timespec ahora;
    if (clock_gettime(CLOCK_MONOTONIC, &ahora) != 0) {
        return DSM_ERR_ESTADO;
    }
    return ahora.tv_sec < plazo->tv_sec ||
           (ahora.tv_sec == plazo->tv_sec && ahora.tv_nsec < plazo->tv_nsec) ?
           DSM_OK : DSM_ERR_TIEMPO;
}

/* La pausa reduce trafico de sondeo. El exito siempre exige una marca DSM. */
static int pausar(const struct timespec *plazo)
{
    int rc = vigente(plazo);
    if (rc != DSM_OK) {
        return rc;
    }
    struct timespec proximo;
    if (clock_gettime(CLOCK_MONOTONIC, &proximo) != 0) {
        return DSM_ERR_ESTADO;
    }
    proximo.tv_nsec += 20000000L;
    if (proximo.tv_nsec >= 1000000000L) {
        proximo.tv_sec++;
        proximo.tv_nsec -= 1000000000L;
    }
    if (proximo.tv_sec > plazo->tv_sec ||
        (proximo.tv_sec == plazo->tv_sec && proximo.tv_nsec > plazo->tv_nsec)) {
        proximo = *plazo;
    }
    int error;
    do {
        error = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &proximo, NULL);
    } while (error == EINTR);
    return error == 0 ? DSM_OK : DSM_ERR_ESTADO;
}

int matrices_control_plazo(uint64_t segundos, struct timespec *plazo)
{
    if (plazo == NULL || segundos == 0 || segundos > INT_MAX) {
        return DSM_ERR_INVAL;
    }
    if (clock_gettime(CLOCK_MONOTONIC, plazo) != 0) {
        return DSM_ERR_ESTADO;
    }
    plazo->tv_sec += (time_t)segundos;
    return DSM_OK;
}

int matrices_control_crear(dsm_context_t *ctx, dsm_proc_id_t proceso,
                           const matrices_control_datos_t *datos, dsm_reserva_id_t *id)
{
    if (id == NULL || !datos_validos(datos) || datos->a != 0 || datos->b != 0 || datos->c != 0) {
        return DSM_ERR_INVAL;
    }
    struct control c = {
        .cabecera = {.magic = CONTROL_MAGIC, .version = CONTROL_VERSION,
                     .datos = *datos, .fase = {.estado = FASE_ARRANQUE}},
    };
    void *base;
    int rc = dsm_reservar(ctx, proceso, datos->page_size, id, &base);
    if (rc != DSM_OK) {
        return rc;
    }
    rc = (*id >> 16) == 0 ? dsm_escribir(ctx, *id, 0, &c, sizeof c) : DSM_ERR_INVAL;
    if (rc != DSM_OK) {
        (void)dsm_liberar(ctx, *id);
    }
    return rc;
}

int matrices_control_abrir(dsm_context_t *ctx, dsm_reserva_id_t id,
                           dsm_node_id_t nodo, matrices_control_datos_t *datos)
{
    if (datos == NULL || nodo == 0 || nodo > CONTROL_MAX_TRABAJADORES) {
        return DSM_ERR_INVAL;
    }
    void *base;
    size_t bytes;
    int rc = dsm_abrir_reserva(ctx, id, &base, &bytes);
    if (rc != DSM_OK) {
        return rc;
    }
    struct cabecera c;
    rc = leer_cabecera(ctx, id, &c);
    if (rc == DSM_OK && (nodo > c.datos.trabajadores || bytes != c.datos.page_size)) {
        rc = DSM_ERR_PROTO;
    }
    if (rc == DSM_OK && c.fase.estado == FASE_ABORTADA) {
        rc = c.fase.resultado;
    }
    if (rc != DSM_OK) {
        (void)dsm_desvincular_reserva(ctx, id);
    } else {
        *datos = c.datos;
    }
    return rc;
}

static int marcar(dsm_context_t *ctx, dsm_reserva_id_t id, dsm_node_id_t nodo,
                    uint32_t estado, int resultado)
{
    struct cabecera c;
    int rc = leer_cabecera(ctx, id, &c);
    if (rc != DSM_OK) {
        return rc;
    }
    if (nodo == 0 || nodo > c.datos.trabajadores || resultado > 0) {
        return DSM_ERR_INVAL;
    }
    if (estado == MARCA_LISTA && c.fase.estado == FASE_ABORTADA) {
        return c.fase.resultado;
    }
    size_t offset = offsetof(struct control, marcas) + (size_t)(nodo - 1u) * sizeof(struct marca);
    struct marca anterior;
    rc = dsm_leer(ctx, id, offset, &anterior, sizeof anterior);
    if (rc != DSM_OK) {
        return rc;
    }
    if (anterior.estado > MARCA_FIN ||
        (anterior.estado != MARCA_FIN && anterior.resultado != 0)) {
        return DSM_ERR_PROTO;
    }
    if (anterior.estado == MARCA_FIN) {
        return estado == MARCA_FIN && anterior.resultado == resultado ? DSM_OK : DSM_ERR_ESTADO;
    }
    struct marca marca = {.estado = estado, .resultado = resultado};
    return dsm_escribir(ctx, id, offset, &marca, sizeof marca);
}

int matrices_control_listo(dsm_context_t *ctx, dsm_reserva_id_t id, dsm_node_id_t nodo)
{
    int rc = marcar(ctx, id, nodo, MARCA_LISTA, DSM_OK);
    if (rc != DSM_OK) {
        (void)marcar(ctx, id, nodo, MARCA_FIN, rc);
    }
    return rc;
}

int matrices_control_fin(dsm_context_t *ctx, dsm_reserva_id_t id,
                         dsm_node_id_t nodo, int resultado)
{
    return marcar(ctx, id, nodo, MARCA_FIN, resultado);
}

int matrices_control_publicar(dsm_context_t *ctx, dsm_reserva_id_t id,
                              const matrices_control_datos_t *datos)
{
    if (!datos_validos(datos) || !ids_validos(datos)) {
        return DSM_ERR_INVAL;
    }
    struct cabecera c;
    int rc = leer_cabecera(ctx, id, &c);
    if (rc != DSM_OK) {
        return rc;
    }
    if (c.fase.estado == FASE_ABORTADA) {
        return c.fase.resultado;
    }
    if (c.datos.filas != datos->filas || c.datos.interior != datos->interior ||
        c.datos.columnas != datos->columnas || c.datos.trabajadores != datos->trabajadores ||
        c.datos.page_size != datos->page_size || datos->a == id || datos->b == id || datos->c == id) {
        return DSM_ERR_INVAL;
    }
    if (c.fase.estado == FASE_DATOS) {
        return c.datos.a == datos->a && c.datos.b == datos->b && c.datos.c == datos->c ?
               DSM_OK : DSM_ERR_ESTADO;
    }
    rc = dsm_escribir(ctx, id, offsetof(struct cabecera, datos), datos, sizeof *datos);
    if (rc == DSM_OK) {
        const struct fase fase = {.estado = FASE_DATOS};
        rc = dsm_escribir(ctx, id, offsetof(struct cabecera, fase), &fase, sizeof fase);
    }
    if (rc != DSM_OK) {
        (void)matrices_control_abortar(ctx, id, rc);
    }
    return rc;
}

int matrices_control_abortar(dsm_context_t *ctx, dsm_reserva_id_t id, int resultado)
{
    if (resultado >= 0) {
        return DSM_ERR_INVAL;
    }
    struct cabecera c;
    int rc = leer_cabecera(ctx, id, &c);
    if (rc != DSM_OK || c.fase.estado == FASE_ABORTADA) {
        return rc;
    }
    const struct fase fase = {.estado = FASE_ABORTADA, .resultado = resultado};
    return dsm_escribir(ctx, id, offsetof(struct cabecera, fase), &fase, sizeof fase);
}

static int esperar_marcas(dsm_context_t *ctx, dsm_reserva_id_t id,
                           uint32_t esperada, const struct timespec *plazo)
{
    for (;;) {
        int rc = vigente(plazo);
        if (rc != DSM_OK) {
            return rc;
        }
        struct cabecera c;
        rc = leer_cabecera(ctx, id, &c);
        if (rc != DSM_OK) {
            return rc;
        }
        if (c.fase.estado == FASE_ABORTADA) {
            return c.fase.resultado;
        }
        struct marca marcas[CONTROL_MAX_TRABAJADORES];
        rc = dsm_leer(ctx, id, offsetof(struct control, marcas), marcas,
                      c.datos.trabajadores * sizeof marcas[0]);
        if (rc != DSM_OK) {
            return rc;
        }
        int completas = 1;
        for (uint32_t i = 0; i < c.datos.trabajadores; i++) {
            if (marcas[i].estado > MARCA_FIN ||
                (marcas[i].estado != MARCA_FIN && marcas[i].resultado != 0)) {
                return DSM_ERR_PROTO;
            }
            if (marcas[i].estado == MARCA_FIN && marcas[i].resultado != DSM_OK) {
                int error = marcas[i].resultado < 0 ? marcas[i].resultado : DSM_ERR_PROTO;
                (void)matrices_control_abortar(ctx, id, error);
                return error;
            }
            if (marcas[i].estado != esperada) {
                completas = 0;
            }
        }
        if (completas) {
            return DSM_OK;
        }
        rc = pausar(plazo);
        if (rc != DSM_OK) {
            return rc;
        }
    }
}

int matrices_control_esperar_listos(dsm_context_t *ctx, dsm_reserva_id_t id,
                                    const struct timespec *plazo)
{
    return esperar_marcas(ctx, id, MARCA_LISTA, plazo);
}

int matrices_control_esperar_fin(dsm_context_t *ctx, dsm_reserva_id_t id,
                                 const struct timespec *plazo)
{
    return esperar_marcas(ctx, id, MARCA_FIN, plazo);
}

int matrices_control_esperar_datos(dsm_context_t *ctx, dsm_reserva_id_t id,
                                   matrices_control_datos_t *datos,
                                   const struct timespec *plazo)
{
    if (datos == NULL) {
        return DSM_ERR_INVAL;
    }
    for (;;) {
        int rc = vigente(plazo);
        if (rc != DSM_OK) {
            return rc;
        }
        struct cabecera c;
        rc = leer_cabecera(ctx, id, &c);
        if (rc != DSM_OK) {
            return rc;
        }
        if (c.fase.estado == FASE_ABORTADA) {
            return c.fase.resultado;
        }
        if (c.fase.estado == FASE_DATOS) {
            if (!ids_validos(&c.datos)) {
                return DSM_ERR_PROTO;
            }
            *datos = c.datos;
            return DSM_OK;
        }
        rc = pausar(plazo);
        if (rc != DSM_OK) {
            return rc;
        }
    }
}

static int cerrar(dsm_context_t *ctx, dsm_reserva_id_t id,
                    const struct timespec *plazo, int operacion)
{
    if (plazo == NULL || plazo->tv_nsec < 0 || plazo->tv_nsec >= 1000000000L) {
        return DSM_ERR_INVAL;
    }
    for (;;) {
        int rc = operacion == 0 ? dsm_desvincular_reserva(ctx, id) :
                 operacion == 1 ? dsm_liberar(ctx, id) : dsm_cerrar(ctx);
        if (rc != DSM_ERR_OCUPADA) {
            return rc;
        }
        rc = pausar(plazo);
        if (rc != DSM_OK) {
            return rc;
        }
    }
}

int matrices_control_desvincular(dsm_context_t *ctx, dsm_reserva_id_t id,
                                 const struct timespec *plazo)
{
    return cerrar(ctx, id, plazo, 0);
}

int matrices_control_liberar(dsm_context_t *ctx, dsm_reserva_id_t id,
                             const struct timespec *plazo)
{
    return cerrar(ctx, id, plazo, 1);
}

int matrices_control_cerrar(dsm_context_t *ctx, const struct timespec *plazo)
{
    return cerrar(ctx, 0, plazo, 2);
}
