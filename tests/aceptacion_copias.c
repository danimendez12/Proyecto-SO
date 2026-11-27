#include "dsm.h"
#include "coherence.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define EXIGIR(c) do { if (!(c)) { \
    fprintf(stderr, "aceptacion_copias:%d: %s\n", __LINE__, #c); \
    goto terminar; } } while (0)

/* Solo este ejecutable envuelve la copia controlada. Los permisos, el
 * directorio y los datos de todas las llamadas exitosas son los reales. */
enum inyeccion {
    NINGUNA, FALLAR_LECTURA, FALLAR_ESCRITURA, PAUSAR_LECTURA,
    CAMBIAR_DATOS, CAMBIAR_VERSION
};
static pthread_mutex_t inyeccion_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t inyeccion_cv = PTHREAD_COND_INITIALIZER;
static enum inyeccion modo;
static unsigned lecturas, escrituras;
static int dentro, continuar;
static int alterado;
static _Thread_local dsm_directorio_t *directorio_capturado;

int __real_dsm_coherencia_copiar_controlado(dsm_coherencia_t *, uint32_t, uint64_t,
                                            size_t, void *, size_t, int,
                                            const struct timespec *);

static int antes_de_copiar(int escribir)
{
    pthread_mutex_lock(&inyeccion_mutex);
    unsigned numero = escribir ? ++escrituras : ++lecturas;
    if (modo == PAUSAR_LECTURA && !escribir && !dentro) {
        dentro = 1;
        pthread_cond_broadcast(&inyeccion_cv);
        while (!continuar) {
            pthread_cond_wait(&inyeccion_cv, &inyeccion_mutex);
        }
    }
    int error = modo == FALLAR_LECTURA && !escribir && numero >= 2 ? ETIMEDOUT :
                (modo == FALLAR_ESCRITURA || modo == CAMBIAR_DATOS || modo == CAMBIAR_VERSION) &&
                escribir && numero >= 2 ? ENOTCONN : 0;
    pthread_mutex_unlock(&inyeccion_mutex);
    return error;
}

int __wrap_dsm_coherencia_copiar_controlado(dsm_coherencia_t *c, uint32_t id,
                                            uint64_t gpn, size_t offset, void *buf,
                                            size_t bytes, int escribir,
                                            const struct timespec *plazo)
{
    int error = antes_de_copiar(escribir);
    if (error != 0) {
        errno = error;
        return -1;
    }
    return __real_dsm_coherencia_copiar_controlado(c, id, gpn, offset, buf,
                                                   bytes, escribir, plazo);
}

int __real_dsm_directorio_escribir_respaldo(dsm_directorio_t *, uint32_t, uint64_t,
                                            size_t, const void *, void *, size_t, uint32_t *);

int __wrap_dsm_directorio_escribir_respaldo(dsm_directorio_t *dir, uint32_t id, uint64_t gpn,
                                            size_t offset, const void *nuevo, void *previo,
                                            size_t bytes, uint32_t *version)
{
    directorio_capturado = dir;
    return __real_dsm_directorio_escribir_respaldo(dir, id, gpn, offset, nuevo, previo,
                                                   bytes, version);
}

int __real_dsm_coherencia_escribir_respaldo(dsm_coherencia_t *, uint32_t, uint64_t,
                                            size_t, void *, void *, size_t, uint32_t *,
                                            const struct timespec *);

int __wrap_dsm_coherencia_escribir_respaldo(dsm_coherencia_t *c, uint32_t id, uint64_t gpn,
                                            size_t offset, void *buf, void *previo, size_t bytes,
                                            uint32_t *version, const struct timespec *plazo)
{
    int error = antes_de_copiar(1);
    if (error != 0) {
        errno = error;
        return -1;
    }
    int rc = __real_dsm_coherencia_escribir_respaldo(c, id, gpn, offset, buf, previo,
                                                     bytes, version, plazo);
    pthread_mutex_lock(&inyeccion_mutex);
    enum inyeccion accion = rc == 0 && escrituras == 1 ? modo : NINGUNA;
    pthread_mutex_unlock(&inyeccion_mutex);
    if (accion == CAMBIAR_DATOS) {
        unsigned char ajeno[65536];
        if (bytes > sizeof ajeno) {
            errno = EINVAL;
            return -1;
        }
        memset(ajeno, 0x93, bytes);
        rc = __real_dsm_coherencia_copiar_controlado(c, id, gpn, offset, ajeno, bytes, 1, plazo);
        alterado = rc == 0;
    } else if (accion == CAMBIAR_VERSION) {
        dsm_directorio_t *dir = directorio_capturado;
        dsm_page_request_t pedido = {.gpn = gpn, .acceso = DSM_ACCESO_LECTURA, .msi = DSM_MSI_I};
        dsm_page_data_t datos;
        dsm_pagina_desc_t pagina;
        int cambio = 0;
        /* Transiciones reales M->S->M con el mismo nodo y los mismos bytes.
         * El rollback debe rechazar la version nueva aunque siga en M. */
        if (dir == NULL || dsm_directorio_servir_pagina(dir, dir->nodo_id, &pedido,
                                                        &datos, NULL, 0, &cambio) != 0 ||
            !cambio || datos.version <= *version ||
            __real_dsm_coherencia_copiar_controlado(c, id, gpn, offset, buf, 0, 1, plazo) != 0 ||
            dsm_directorio_describir(dir, gpn, &pagina) != 0 ||
            pagina.msi != DSM_MSI_M || pagina.version <= *version) {
            errno = EPROTO;
            return -1;
        }
        alterado = 1;
    }
    return rc;
}

static void inyectar(enum inyeccion nueva)
{
    pthread_mutex_lock(&inyeccion_mutex);
    modo = nueva;
    lecturas = escrituras = 0;
    dentro = continuar = 0;
    alterado = 0;
    pthread_mutex_unlock(&inyeccion_mutex);
}

static void reanudar(void)
{
    pthread_mutex_lock(&inyeccion_mutex);
    continuar = 1;
    pthread_cond_broadcast(&inyeccion_cv);
    pthread_mutex_unlock(&inyeccion_mutex);
}

struct tarea {
    dsm_context_t *ctx;
    dsm_reserva_id_t id;
    void *buffer;
    size_t bytes;
    int escribir, resultado;
    pthread_barrier_t *barrera;
};

static void *copiar_hilo(void *arg)
{
    struct tarea *t = arg;
    if (t->barrera != NULL) {
        pthread_barrier_wait(t->barrera);
    }
    t->resultado = t->escribir ? dsm_escribir(t->ctx, t->id, 0, t->buffer, t->bytes)
                              : dsm_leer(t->ctx, t->id, 0, t->buffer, t->bytes);
    if (dsm_hilo_desregistrar(t->ctx) != DSM_ERR_ESTADO) {
        t->resultado = DSM_ERR_ESTADO;
    }
    return NULL;
}

static int escritores(dsm_context_t *ctx, dsm_reserva_id_t id, void *a, void *b, size_t n)
{
    pthread_barrier_t barrera;
    if (pthread_barrier_init(&barrera, NULL, 2) != 0) {
        return -1;
    }
    struct tarea tareas[2] = {
        {.ctx = ctx, .id = id, .buffer = a, .bytes = n, .escribir = 1, .barrera = &barrera},
        {.ctx = ctx, .id = id, .buffer = b, .bytes = n, .escribir = 1, .barrera = &barrera}
    };
    pthread_t hilo;
    if (pthread_create(&hilo, NULL, copiar_hilo, &tareas[0]) != 0) {
        pthread_barrier_destroy(&barrera);
        return -1;
    }
    copiar_hilo(&tareas[1]);
    int rc = pthread_join(hilo, NULL);
    pthread_barrier_destroy(&barrera);
    return rc == 0 && tareas[0].resultado == DSM_OK && tareas[1].resultado == DSM_OK ? 0 : -1;
}

int main(void)
{
    alarm(45);
    FILE *config = tmpfile();
    dsm_context_t *ctx = NULL;
    dsm_proc_id_t proceso;
    dsm_reserva_id_t a = 0, b = 0;
    void *ar_a, *ar_b;
    unsigned char *patron = NULL, *leido = NULL, *esperado = NULL;
    pthread_t hilo;
    int hilo_activo = 0, resultado = 1;
    long pagina_so = sysconf(_SC_PAGESIZE);
    EXIGIR(config != NULL && pagina_so > 0);
    size_t p = (size_t)pagina_so, n = 3 * p - 17;
    EXIGIR(fprintf(config, "page_size = %zu\nmemoria_inicial = %zu\nmax_nodos = 1\n"
                   "max_hilos = 8\nrpc_timeout_ms = 1000\n"
                   "puerto_rango_inicio = 27200\npuerto_rango_fin = 27299\n"
                   "nodo = 0 copias 127.0.0.1 27295\n", p, 8 * p) > 0 &&
           fflush(config) == 0);
    char ruta[64];
    snprintf(ruta, sizeof ruta, "/proc/self/fd/%d", fileno(config));
    EXIGIR(dsm_iniciar(ruta, 0, &ctx) == DSM_OK && dsm_proc_abrir(ctx, &proceso) == DSM_OK);
    EXIGIR(dsm_reservar(ctx, proceso, n, &a, &ar_a) == DSM_OK &&
           dsm_reservar(ctx, proceso, n, &b, &ar_b) == DSM_OK);
    patron = malloc(n);
    leido = malloc(n);
    esperado = malloc(n);
    EXIGIR(patron != NULL && leido != NULL && esperado != NULL);

    inyectar(NINGUNA);
    EXIGIR(dsm_leer(NULL, a, 0, leido, SIZE_MAX) == DSM_ERR_INVAL);
    EXIGIR(dsm_leer(ctx, UINT32_MAX, 0, leido, SIZE_MAX) == DSM_ERR_INVAL);
    EXIGIR(dsm_escribir(ctx, a, SIZE_MAX, patron, 1) == DSM_ERR_INVAL);
    EXIGIR(dsm_leer(ctx, a, 0, NULL, 1) == DSM_ERR_INVAL);
    EXIGIR(dsm_escribir(ctx, a, 0, NULL, 1) == DSM_ERR_INVAL);
    EXIGIR(dsm_leer(ctx, a, n, NULL, 0) == DSM_OK &&
           dsm_escribir(ctx, a, n, NULL, 0) == DSM_OK);
    EXIGIR(dsm_leer(ctx, a, n + 1, NULL, 0) == DSM_ERR_INVAL &&
           dsm_leer(ctx, UINT32_MAX, 0, NULL, 0) == DSM_ERR_INVAL);
    EXIGIR(lecturas == 0 && escrituras == 0);
    puts("copias: NULL, cero bytes y rango se validan antes de asignar/copiar");

    for (size_t i = 0; i < n; i++) {
        esperado[i] = patron[i] = (unsigned char)(17 * i + 5);
    }
    EXIGIR(dsm_escribir(ctx, a, 0, patron, n) == DSM_OK);
    memset(leido, 0xa5, n);
    inyectar(FALLAR_LECTURA);
    EXIGIR(dsm_leer(ctx, a, 0, leido, n) == DSM_ERR_TIEMPO && lecturas == 2);
    for (size_t i = 0; i < n; i++) {
        EXIGIR(leido[i] == 0xa5);
    }
    EXIGIR(dsm_hilo_desregistrar(ctx) == DSM_ERR_ESTADO);
    inyectar(FALLAR_ESCRITURA);
    memset(patron, 0x6c, n);
    EXIGIR(dsm_escribir(ctx, a, 0, patron, n) == DSM_ERR_NODO &&
           escrituras == 2 && lecturas == 0);
    /* El permiso/version siguen vigentes: restauracion local sin otro RPC. */
    inyectar(NINGUNA);
    EXIGIR(dsm_leer(ctx, a, 0, leido, n) == DSM_OK && memcmp(leido, esperado, n) == 0);
    inyectar(CAMBIAR_DATOS);
    EXIGIR(dsm_escribir(ctx, a, 0, patron, n) == DSM_ERR_NODO &&
           escrituras == 2 && alterado);
    inyectar(NINGUNA);
    memset(esperado, 0x93, p);
    EXIGIR(dsm_leer(ctx, a, 0, leido, n) == DSM_OK && memcmp(leido, esperado, n) == 0);
    inyectar(CAMBIAR_VERSION);
    EXIGIR(dsm_escribir(ctx, a, 0, patron, n) == DSM_ERR_NODO &&
           escrituras == 2 && alterado);
    inyectar(NINGUNA);
    memset(esperado, 0x6c, p);
    EXIGIR(dsm_leer(ctx, a, 0, leido, n) == DSM_OK && memcmp(leido, esperado, n) == 0);
    puts("copias: restauracion local posible; cambios reales de datos o version M impiden sobrescribirlos");

    /* Inyeccion de permisos: una Ar inaccesible no debe recibir memcpy, aun
     * si el hilo llamador no estaba registrado antes de entrar al API. */
    EXIGIR(mprotect(ar_a, 3 * p, PROT_NONE) == 0 && mprotect(ar_b, 3 * p, PROT_NONE) == 0);
    inyectar(FALLAR_ESCRITURA);
    EXIGIR(dsm_escribir(ctx, a, 7, ar_a, n - 7) == DSM_ERR_NODO);
    inyectar(NINGUNA);
    EXIGIR(dsm_leer(ctx, a, 0, leido, n) == DSM_OK && memcmp(leido, esperado, n) == 0);
    EXIGIR(dsm_leer(ctx, a, 0, ar_b, n) == DSM_OK);
    EXIGIR(dsm_leer(ctx, b, 0, leido, n) == DSM_OK && memcmp(leido, esperado, n) == 0);
    EXIGIR(dsm_escribir(ctx, a, 0, ar_b, n) == DSM_OK);
    memmove(esperado + 7, esperado, n - 7);
    EXIGIR(dsm_escribir(ctx, a, 7, ar_a, n - 7) == DSM_OK);
    memmove(esperado, esperado + 9, n - 9);
    EXIGIR(dsm_leer(ctx, a, 9, ar_a, n - 9) == DSM_OK);
    EXIGIR(dsm_leer(ctx, a, 0, leido, n) == DSM_OK && memcmp(leido, esperado, n) == 0);
    EXIGIR(dsm_leer(ctx, a, 0, (unsigned char *)ar_b + n - 1, 2) == DSM_ERR_INVAL);
    EXIGIR(dsm_escribir(ctx, a, 0, (unsigned char *)ar_b + n - 1, 2) == DSM_ERR_INVAL);
    EXIGIR(dsm_leer(ctx, a, 0, (void *)((uintptr_t)ar_b - 1), 2) == DSM_ERR_INVAL);
    EXIGIR(dsm_hilo_desregistrar(ctx) == DSM_ERR_ESTADO);
    memset(patron, 0x22, n);
    EXIGIR(dsm_escribir(ctx, b, 0, patron, n) == DSM_OK);
    inyectar(FALLAR_ESCRITURA);
    EXIGIR(dsm_leer(ctx, a, 0, ar_b, n) == DSM_ERR_NODO && escrituras == 2);
    inyectar(NINGUNA);
    memcpy(patron, esperado, p);
    EXIGIR(dsm_leer(ctx, b, 0, leido, n) == DSM_OK && memcmp(leido, patron, n) == 0);
    puts("copias: alias Ar protegido, solapamiento y destino DSM parcial sin accesos directos");

    inyectar(PAUSAR_LECTURA);
    struct tarea tarea = {.ctx = ctx, .id = a, .buffer = ar_b, .bytes = n};
    EXIGIR(pthread_create(&hilo, NULL, copiar_hilo, &tarea) == 0);
    hilo_activo = 1;
    pthread_mutex_lock(&inyeccion_mutex);
    while (!dentro) {
        pthread_cond_wait(&inyeccion_cv, &inyeccion_mutex);
    }
    pthread_mutex_unlock(&inyeccion_mutex);
    EXIGIR(dsm_liberar(ctx, a) == DSM_ERR_OCUPADA &&
           dsm_liberar(ctx, b) == DSM_ERR_OCUPADA && dsm_cerrar(ctx) == DSM_ERR_OCUPADA);
    reanudar();
    int unido = pthread_join(hilo, NULL);
    hilo_activo = 0;
    EXIGIR(unido == 0 && tarea.resultado == DSM_OK);
    inyectar(NINGUNA);
    memset(patron, 0x31, n);
    memset(esperado, 0x72, n);
    for (unsigned i = 0; i < 8; i++) {
        EXIGIR(escritores(ctx, b, patron, esperado, n) == 0);
        EXIGIR(dsm_leer(ctx, b, 0, leido, n) == DSM_OK &&
               (memcmp(leido, patron, n) == 0 || memcmp(leido, esperado, n) == 0));
    }
    puts("copias: registro temporal protege ambas vistas y serializa escritores locales solapados");
    resultado = 0;
terminar:
    reanudar();
    if (hilo_activo) {
        pthread_join(hilo, NULL);
    }
    inyectar(NINGUNA);
    if (ctx != NULL) {
        if (a != 0 && dsm_liberar(ctx, a) != DSM_OK) {
            resultado = 1;
        }
        if (b != 0 && dsm_liberar(ctx, b) != DSM_OK) {
            resultado = 1;
        }
        if (dsm_cerrar(ctx) != DSM_OK) {
            resultado = 1;
        }
    }
    free(patron);
    free(leido);
    free(esperado);
    if (config != NULL) {
        fclose(config);
    }
    return resultado;
}
