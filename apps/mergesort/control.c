#include "control.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <unistd.h>

enum {
    CONTROL_MAGIC = 0x4d534731,
    CONTROL_VERSION = 1,
    CONTROL_MAX_TRABAJADORES = 15,
    MARCA_VACIA = 0,
    MARCA_COMPLETA = 1,
    MARCA_CERRADA = 2,
    MARCA_ERROR = 3
};

struct cabecera {
    uint32_t magic, version;
    mergesort_control_datos_t datos;
    mergesort_control_fase_t fase;
    int32_t error;
};

struct marca {
    uint64_t generacion;
    uint32_t estado;
    int32_t resultado;
};

_Static_assert(sizeof(struct cabecera) <= 4096, "Cabecera de control excesiva");
_Static_assert(sizeof(struct marca) <= 4096, "Marca de control excesiva");

static int datos_validos(const mergesort_control_datos_t *d)
{
    long pagina = sysconf(_SC_PAGESIZE);
    return d != NULL && pagina > 0 && d->elementos != 0 && d->bloque != 0 &&
           d->trabajadores != 0 && d->trabajadores <= CONTROL_MAX_TRABAJADORES &&
           d->hilos != 0 && d->page_size >= sizeof(struct cabecera) &&
           d->page_size % (size_t)pagina == 0 &&
           d->page_size % sizeof(int32_t) == 0 &&
           d->bloque % (d->page_size / sizeof(int32_t)) == 0 &&
           d->elementos <= SIZE_MAX / sizeof(int32_t) &&
           d->trabajadores + 1u <= SIZE_MAX / d->page_size;
}

static int ids_validos(const mergesort_control_datos_t *d, dsm_reserva_id_t id)
{
    return d->a != 0 && d->b != 0 && d->a != d->b && d->a != id && d->b != id &&
           (d->a >> 16) == 0 && (d->b >> 16) == 0;
}

static int fase_valida(const struct cabecera *c, dsm_reserva_id_t id)
{
    const mergesort_control_fase_t *f = &c->fase;
    if (f->origen > 1) {
        return 0;
    }
    if (f->tipo == MERGESORT_CONTROL_ARRANQUE) {
        return f->generacion == 0 && f->ancho == 0 && f->origen == 0 &&
               ((c->datos.a == 0 && c->datos.b == 0) || ids_validos(&c->datos, id));
    }
    if (!ids_validos(&c->datos, id) || f->generacion == 0) {
        return 0;
    }
    if (f->tipo == MERGESORT_CONTROL_ORDENAR) {
        return f->generacion == 1 && f->ancho == c->datos.bloque && f->origen == 0;
    }
    if (f->tipo == MERGESORT_CONTROL_FUSIONAR) {
        return f->generacion >= 2 && f->ancho != 0 &&
               f->ancho % (c->datos.page_size / sizeof(int32_t)) == 0;
    }
    return f->tipo == MERGESORT_CONTROL_CERRAR && f->generacion >= 2 && f->ancho == 0;
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
    return c->magic == CONTROL_MAGIC && c->version == CONTROL_VERSION &&
           datos_validos(&c->datos) && c->error <= 0 && fase_valida(c, id) ?
           DSM_OK : DSM_ERR_PROTO;
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

int mergesort_control_plazo(uint64_t segundos, struct timespec *plazo)
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

int mergesort_control_crear(dsm_context_t *ctx, dsm_proc_id_t proceso,
                            const mergesort_control_datos_t *datos, dsm_reserva_id_t *id)
{
    if (id != NULL) {
        *id = 0;
    }
    if (id == NULL || !datos_validos(datos) || datos->a != 0 || datos->b != 0) {
        return DSM_ERR_INVAL;
    }
    void *base;
    int rc = dsm_reservar(ctx, proceso, ((size_t)datos->trabajadores + 1u) * datos->page_size,
                          id, &base);
    if (rc != DSM_OK) {
        return rc;
    }
    if ((*id >> 16) != 0) {
        rc = DSM_ERR_INVAL;
    }
    for (uint32_t p = 0; p <= datos->trabajadores && rc == DSM_OK; p++) {
        dsm_node_id_t hogar;
        uint64_t gpn;
        rc = dsm_ubicacion_pagina(ctx, *id, p, &hogar, &gpn);
        if (rc == DSM_OK && hogar != 0) {
            rc = DSM_ERR_NOMEM;
        }
    }
    if (rc == DSM_OK) {
        const struct cabecera c = {.magic = CONTROL_MAGIC, .version = CONTROL_VERSION,
                                  .datos = *datos};
        rc = dsm_escribir(ctx, *id, 0, &c, sizeof c);
    }
    for (uint32_t i = 0; i < datos->trabajadores && rc == DSM_OK; i++) {
        const struct marca m = {0};
        rc = dsm_escribir(ctx, *id, ((size_t)i + 1u) * datos->page_size, &m, sizeof m);
    }
    if (rc != DSM_OK) {
        if (dsm_liberar(ctx, *id) == DSM_OK) {
            *id = 0;
        }
    }
    return rc;
}

int mergesort_control_abrir(dsm_context_t *ctx, dsm_reserva_id_t id, uint32_t indice,
                            mergesort_control_datos_t *datos)
{
    if (datos == NULL || indice >= CONTROL_MAX_TRABAJADORES) {
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
    if (rc == DSM_OK && (indice >= c.datos.trabajadores ||
        bytes != ((size_t)c.datos.trabajadores + 1u) * c.datos.page_size)) {
        rc = DSM_ERR_PROTO;
    }
    if (rc == DSM_OK) {
        rc = c.error;
    }
    if (rc != DSM_OK) {
        (void)dsm_desvincular_reserva(ctx, id);
    } else {
        *datos = c.datos;
    }
    return rc;
}

int mergesort_control_datos(dsm_context_t *ctx, dsm_reserva_id_t id,
                            mergesort_control_datos_t *datos)
{
    if (datos == NULL) {
        return DSM_ERR_INVAL;
    }
    struct cabecera c;
    int rc = leer_cabecera(ctx, id, &c);
    if (rc == DSM_OK && c.error != DSM_OK) {
        rc = c.error;
    }
    if (rc == DSM_OK && !ids_validos(&c.datos, id)) {
        rc = DSM_ERR_ESTADO;
    }
    if (rc == DSM_OK) {
        *datos = c.datos;
    }
    return rc;
}

int mergesort_control_abortar(dsm_context_t *ctx, dsm_reserva_id_t id, int resultado)
{
    if (resultado >= 0) {
        return DSM_ERR_INVAL;
    }
    struct cabecera c;
    int rc = leer_cabecera(ctx, id, &c);
    if (rc != DSM_OK || c.error != DSM_OK) {
        return rc;
    }
    int32_t error = resultado;
    return dsm_escribir(ctx, id, offsetof(struct cabecera, error), &error, sizeof error);
}

static int leer_marca(dsm_context_t *ctx, dsm_reserva_id_t id, uint32_t page_size,
                       uint32_t indice, struct marca *m)
{
    int rc = dsm_leer(ctx, id, ((size_t)indice + 1u) * page_size, m, sizeof *m);
    if (rc != DSM_OK) {
        return rc;
    }
    return m->estado <= MARCA_ERROR &&
           (m->estado == MARCA_ERROR ? m->resultado < 0 : m->resultado == 0) &&
           (m->estado != MARCA_VACIA || m->generacion == 0) ? DSM_OK : DSM_ERR_PROTO;
}

int mergesort_control_marcar(dsm_context_t *ctx, dsm_reserva_id_t id, uint32_t indice,
                             uint64_t generacion, int resultado)
{
    struct cabecera c;
    int rc = leer_cabecera(ctx, id, &c);
    if (rc != DSM_OK) {
        return rc;
    }
    if (indice >= c.datos.trabajadores || resultado > 0) {
        return DSM_ERR_INVAL;
    }
    if (generacion != c.fase.generacion) {
        return DSM_ERR_PROTO;
    }
    if (c.error != DSM_OK && resultado == DSM_OK) {
        return c.error;
    }
    struct marca anterior;
    rc = leer_marca(ctx, id, c.datos.page_size, indice, &anterior);
    if (rc != DSM_OK) {
        return rc;
    }
    uint32_t estado = resultado != DSM_OK ? MARCA_ERROR :
                      c.fase.tipo == MERGESORT_CONTROL_CERRAR ? MARCA_CERRADA : MARCA_COMPLETA;
    if (anterior.estado != MARCA_VACIA && anterior.generacion == generacion) {
        return anterior.estado == estado && anterior.resultado == resultado ?
               DSM_OK : DSM_ERR_ESTADO;
    }
    if (anterior.estado == MARCA_ERROR || anterior.estado == MARCA_CERRADA ||
        (generacion == 0 ? anterior.estado != MARCA_VACIA :
         anterior.estado != MARCA_COMPLETA || anterior.generacion != generacion - 1u)) {
        return DSM_ERR_PROTO;
    }
    const struct marca m = {.generacion = generacion, .estado = estado, .resultado = resultado};
    return dsm_escribir(ctx, id, ((size_t)indice + 1u) * c.datos.page_size, &m, sizeof m);
}

static int completas(dsm_context_t *ctx, dsm_reserva_id_t id, const struct cabecera *c)
{
    int pendientes = 0;
    for (uint32_t i = 0; i < c->datos.trabajadores; i++) {
        struct marca m;
        int rc = leer_marca(ctx, id, c->datos.page_size, i, &m);
        if (rc != DSM_OK) {
            return rc;
        }
        if (m.estado == MARCA_ERROR) {
            (void)mergesort_control_abortar(ctx, id, m.resultado);
            return m.resultado;
        }
        if (m.generacion > c->fase.generacion ||
            (m.estado == MARCA_CERRADA && c->fase.tipo != MERGESORT_CONTROL_CERRAR)) {
            return DSM_ERR_PROTO;
        }
        uint32_t esperada = c->fase.tipo == MERGESORT_CONTROL_CERRAR ? MARCA_CERRADA : MARCA_COMPLETA;
        if (m.generacion != c->fase.generacion || m.estado != esperada) {
            pendientes = 1;
        }
    }
    return pendientes ? DSM_ERR_OCUPADA : DSM_OK;
}

int mergesort_control_esperar(dsm_context_t *ctx, dsm_reserva_id_t id, uint64_t generacion,
                              const struct timespec *plazo)
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
        if (c.error != DSM_OK) {
            return c.error;
        }
        if (c.fase.generacion != generacion) {
            return DSM_ERR_PROTO;
        }
        rc = completas(ctx, id, &c);
        if (rc != DSM_ERR_OCUPADA) {
            return rc;
        }
        rc = pausar(plazo);
        if (rc != DSM_OK) {
            return rc;
        }
    }
}

int mergesort_control_publicar_datos(dsm_context_t *ctx, dsm_reserva_id_t id,
                                     const mergesort_control_datos_t *datos)
{
    if (!datos_validos(datos) || !ids_validos(datos, id)) {
        return DSM_ERR_INVAL;
    }
    struct cabecera c;
    int rc = leer_cabecera(ctx, id, &c);
    if (rc != DSM_OK) {
        return rc;
    }
    if (c.error != DSM_OK) {
        return c.error;
    }
    if (c.fase.tipo != MERGESORT_CONTROL_ARRANQUE ||
        c.datos.elementos != datos->elementos || c.datos.bloque != datos->bloque ||
        c.datos.trabajadores != datos->trabajadores || c.datos.hilos != datos->hilos ||
        c.datos.page_size != datos->page_size) {
        return DSM_ERR_ESTADO;
    }
    if (ids_validos(&c.datos, id)) {
        return c.datos.a == datos->a && c.datos.b == datos->b ? DSM_OK : DSM_ERR_ESTADO;
    }
    rc = completas(ctx, id, &c);
    if (rc == DSM_OK) {
        rc = dsm_escribir(ctx, id, offsetof(struct cabecera, datos), datos, sizeof *datos);
        if (rc != DSM_OK) {
            (void)mergesort_control_abortar(ctx, id, rc);
        }
    }
    return rc;
}

static int misma_fase(const mergesort_control_fase_t *a, const mergesort_control_fase_t *b)
{
    return a->generacion == b->generacion && a->ancho == b->ancho &&
           a->tipo == b->tipo && a->origen == b->origen;
}

static int siguiente_valida(const struct cabecera *c, const mergesort_control_fase_t *f)
{
    if (f->origen > 1) {
        return 0;
    }
    if (c->fase.tipo == MERGESORT_CONTROL_ARRANQUE) {
        return f->tipo == MERGESORT_CONTROL_ORDENAR && f->ancho == c->datos.bloque && f->origen == 0;
    }
    if (c->fase.tipo != MERGESORT_CONTROL_ORDENAR && c->fase.tipo != MERGESORT_CONTROL_FUSIONAR) {
        return 0;
    }
    uint32_t salida = c->fase.tipo == MERGESORT_CONTROL_ORDENAR ? 0 : 1u - c->fase.origen;
    /* ORDENAR deja runs de bloque elementos; una fusion deja runs de 2*ancho.
     * Comparar con ceil(N/2) evita multiplicar y desbordar ese ultimo ancho. */
    int completo = c->fase.tipo == MERGESORT_CONTROL_ORDENAR ?
                   c->fase.ancho >= c->datos.elementos :
                   c->fase.ancho >= c->datos.elementos / 2u + c->datos.elementos % 2u;
    if (f->tipo == MERGESORT_CONTROL_CERRAR) {
        return completo && f->ancho == 0 && f->origen == salida;
    }
    if (completo) {
        return 0;
    }
    uint64_t ancho = c->fase.ancho;
    if (c->fase.tipo == MERGESORT_CONTROL_FUSIONAR) {
        if (ancho > UINT64_MAX / 2u) {
            return 0;
        }
        ancho *= 2u;
    }
    return f->tipo == MERGESORT_CONTROL_FUSIONAR && f->ancho == ancho && f->origen == salida;
}

int mergesort_control_publicar_fase(dsm_context_t *ctx, dsm_reserva_id_t id,
                                    const mergesort_control_fase_t *fase)
{
    if (fase == NULL) {
        return DSM_ERR_INVAL;
    }
    struct cabecera c;
    int rc = leer_cabecera(ctx, id, &c);
    if (rc != DSM_OK) {
        return rc;
    }
    if (c.error != DSM_OK) {
        return c.error;
    }
    if (misma_fase(&c.fase, fase)) {
        return DSM_OK;
    }
    if (c.fase.generacion == UINT64_MAX || fase->generacion != c.fase.generacion + 1u ||
        !ids_validos(&c.datos, id) || !siguiente_valida(&c, fase)) {
        return DSM_ERR_PROTO;
    }
    rc = completas(ctx, id, &c);
    if (rc == DSM_OK) {
        rc = dsm_escribir(ctx, id, offsetof(struct cabecera, fase), fase, sizeof *fase);
        if (rc != DSM_OK) {
            (void)mergesort_control_abortar(ctx, id, rc);
        }
    }
    return rc;
}

int mergesort_control_esperar_fase(dsm_context_t *ctx, dsm_reserva_id_t id,
                                   uint64_t anterior, mergesort_control_fase_t *fase,
                                   const struct timespec *plazo)
{
    if (fase == NULL || anterior == UINT64_MAX) {
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
        if (c.error != DSM_OK) {
            return c.error;
        }
        if (c.fase.generacion == anterior + 1u) {
            *fase = c.fase;
            return DSM_OK;
        }
        if (c.fase.generacion != anterior) {
            return DSM_ERR_PROTO;
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

int mergesort_control_desvincular(dsm_context_t *ctx, dsm_reserva_id_t id,
                                  const struct timespec *plazo)
{
    return cerrar(ctx, id, plazo, 0);
}

int mergesort_control_liberar(dsm_context_t *ctx, dsm_reserva_id_t id,
                              const struct timespec *plazo)
{
    return cerrar(ctx, id, plazo, 1);
}

int mergesort_control_cerrar(dsm_context_t *ctx, const struct timespec *plazo)
{
    return cerrar(ctx, 0, plazo, 2);
}
