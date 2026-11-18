#include "pager.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

enum { DSM_PAGER_MAX_TRABAJADORES = 64 };

struct trabajador {
    dsm_solicitud_t solicitud;
    pthread_t hilo;
    int registrado;
};

struct dsm_pager {
    dsm_vistas_t *vistas;
    size_t page_size;
    size_t max_trabajadores;
    dsm_pager_pagina_t *paginas;
    unsigned char *imagen;
    dsm_pager_coherencia_t resolver;
    dsm_pager_fallo_t resolver_directorio;
    uint16_t solicitante;
    uint32_t timeout_ms;
    struct trabajador *trabajadores;
    dsm_pager_preparar_t preparar;
    void *datos;
    dsm_acciones_t acciones;
    sem_t aviso;
    pthread_t hilo;
    pthread_mutex_t mutex;
    int cerrando;
};

static int error_con(int error)
{
    errno = error;
    return -1;
}

static int permisos(uint8_t msi)
{
    switch (msi) {
    case DSM_MSI_I:
        return PROT_NONE;
    case DSM_MSI_S:
        return PROT_READ;
    case DSM_MSI_M:
        return PROT_READ | PROT_WRITE;
    default:
        return -1;
    }
}

static int vence_en(uint32_t ms, struct timespec *plazo)
{
    if (clock_gettime(CLOCK_MONOTONIC, plazo) != 0) {
        return -1;
    }
    plazo->tv_sec += ms / 1000;
    plazo->tv_nsec += (long)(ms % 1000) * 1000000;
    if (plazo->tv_nsec >= 1000000000L) {
        plazo->tv_sec++;
        plazo->tv_nsec -= 1000000000L;
    }
    return 0;
}

static int antes_del_plazo(const struct timespec *plazo)
{
    struct timespec ahora;
    return clock_gettime(CLOCK_MONOTONIC, &ahora) == 0 &&
           (ahora.tv_sec < plazo->tv_sec ||
            (ahora.tv_sec == plazo->tv_sec && ahora.tv_nsec < plazo->tv_nsec));
}

static int resolver_msi(dsm_pager_t *pager, size_t indice, dsm_acceso_t acceso,
                         unsigned char *pagina_cr, dsm_pager_pagina_t *pagina)
{
    int escritura = acceso == DSM_FALLO_ESCRITURA;
    dsm_pager_peticion_t peticion = {
        .pagina = *pagina,
        .solicitante = pager->solicitante,
        .acceso = acceso,
    };
    dsm_pager_concesion_t concesion = {0};
    if (vence_en(pager->timeout_ms, &peticion.plazo) != 0 ||
        pager->resolver(&peticion, pager->imagen, pager->page_size, &concesion, pager->datos) != 0 ||
        !antes_del_plazo(&peticion.plazo)) {
        return DSM_SOLICITUD_ERROR;
    }

    if (concesion.exclusiva != escritura || concesion.version < pagina->version ||
        (escritura && concesion.version == pagina->version) ||
        (concesion.bytes_validos != 0 && concesion.bytes_validos != pager->page_size) ||
        (pagina->msi == DSM_MSI_I && concesion.bytes_validos != pager->page_size)) {
        return DSM_SOLICITUD_ERROR;
    }
    if (concesion.bytes_validos != 0) {
        if (pagina->msi == DSM_MSI_I) {
            memcpy(pagina_cr, pager->imagen, pager->page_size);
        } else if (memcmp(pagina_cr, pager->imagen, pager->page_size) != 0) {
            /* En S hay lectores activos y la copia ya debe estar vigente. */
            return DSM_SOLICITUD_ERROR;
        }
    }

    uint8_t nuevo = escritura ? DSM_MSI_M : DSM_MSI_S;
    pthread_mutex_lock(&pager->mutex);
    int rc = dsm_vistas_proteger_pagina(pager->vistas, indice, pager->page_size, permisos(nuevo));
    if (rc == 0) {
        pagina->version = concesion.version;
        pagina->msi = nuevo;
    }
    pthread_mutex_unlock(&pager->mutex);
    return rc == 0 ? DSM_SOLICITUD_LISTA : DSM_SOLICITUD_ERROR;
}

static int cargar_pagina(dsm_pager_t *pager, size_t indice, dsm_acceso_t acceso)
{
    if (indice >= pager->vistas->bytes / pager->page_size) {
        return DSM_SOLICITUD_ERROR;
    }

    dsm_pager_pagina_t *pagina = &pager->paginas[indice];
    if (pager->resolver_directorio != NULL) {
        dsm_pager_peticion_t peticion = {
            .pagina = *pagina,
            .solicitante = pager->solicitante,
            .acceso = acceso,
        };
        if (vence_en(pager->timeout_ms, &peticion.plazo) != 0 ||
            pager->resolver_directorio(&peticion, pager->datos) != 0) {
            return DSM_SOLICITUD_ERROR;
        }
        return DSM_SOLICITUD_LISTA;
    }
    if (pagina->msi == DSM_MSI_M ||
        (pagina->msi == DSM_MSI_S && acceso == DSM_FALLO_LECTURA)) {
        return DSM_SOLICITUD_LISTA;
    }

    unsigned char *pagina_cr = (unsigned char *)pager->vistas->c_r + indice * pager->page_size;
    if (pager->resolver != NULL) {
        return resolver_msi(pager, indice, acceso, pagina_cr, pagina);
    }
    if (acceso != DSM_FALLO_LECTURA ||
        pager->preparar(pagina_cr, indice, pager->page_size, pager->datos) != 0) {
        return DSM_SOLICITUD_ERROR;
    }
    pthread_mutex_lock(&pager->mutex);
    int rc = dsm_vistas_proteger_pagina(pager->vistas, indice, pager->page_size, PROT_READ);
    if (rc == 0) {
        pagina->msi = DSM_MSI_S;
    }
    pthread_mutex_unlock(&pager->mutex);
    return rc == 0 ? DSM_SOLICITUD_LISTA : DSM_SOLICITUD_ERROR;
}

static struct trabajador *buscar_pendiente(dsm_pager_t *pager)
{
    for (size_t i = 0; i < pager->max_trabajadores; i++) {
        struct trabajador *trabajador = &pager->trabajadores[i];
        if (trabajador->registrado &&
            atomic_load_explicit(&trabajador->solicitud.estado, memory_order_acquire) ==
                DSM_SOLICITUD_PENDIENTE) {
            return trabajador;
        }
    }
    return NULL;
}

static void *paginar(void *arg)
{
    dsm_pager_t *pager = arg;
    for (;;) {
        while (sem_wait(&pager->aviso) != 0) {
            if (errno != EINTR) {
                abort();
            }
        }

        pthread_mutex_lock(&pager->mutex);
        int cerrando = pager->cerrando;
        struct trabajador *trabajador = buscar_pendiente(pager);
        pthread_mutex_unlock(&pager->mutex);
        if (cerrando) {
            return NULL;
        }
        if (trabajador == NULL) {
            continue;
        }

        dsm_solicitud_t *solicitud = &trabajador->solicitud;
        int estado = cargar_pagina(pager, solicitud->pagina, solicitud->acceso);

        /* El trabajador puede observar el resultado antes de que llegue la
         * senal. El mutex conserva su registro hasta terminar pthread_kill.
         */
        pthread_mutex_lock(&pager->mutex);
        atomic_store_explicit(&solicitud->estado, estado, memory_order_release);
        if (pthread_kill(trabajador->hilo, SIGUSR1) != 0) {
            abort();
        }
        pthread_mutex_unlock(&pager->mutex);
    }
}

/* Llamar con el mutex tomado y sin trabajadores registrados. */
static void solicitar_cierre(dsm_pager_t *pager)
{
    pager->cerrando = 1;
    if (sem_post(&pager->aviso) != 0) {
        abort();
    }
}

static void liberar(dsm_pager_t *pager)
{
    free(pager->trabajadores);
    free(pager->imagen);
    free(pager->paginas);
    free(pager);
}

static int iniciar(dsm_pager_t **salida, dsm_vistas_t *vistas, size_t page_size,
                   size_t max_trabajadores, dsm_pager_preparar_t preparar, void *datos,
                   const dsm_pager_pagina_t *paginas, uint16_t solicitante, uint32_t timeout_ms,
                   dsm_pager_coherencia_t resolver, dsm_pager_fallo_t resolver_directorio)
{
    if (salida == NULL) {
        return error_con(EINVAL);
    }
    *salida = NULL;
    long pagina_so = sysconf(_SC_PAGESIZE);
    if (vistas == NULL || vistas->a_r == NULL || vistas->c_r == NULL || vistas->bytes == 0 ||
        pagina_so <= 0 || page_size == 0 || page_size % (size_t)pagina_so != 0 ||
        vistas->bytes % page_size != 0 || (uintptr_t)vistas->a_r % (size_t)pagina_so != 0 ||
        max_trabajadores == 0 || max_trabajadores > DSM_PAGER_MAX_TRABAJADORES ||
        (preparar == NULL && ((resolver == NULL && resolver_directorio == NULL) ||
                             paginas == NULL || timeout_ms == 0))) {
        return error_con(EINVAL);
    }

    size_t cantidad = vistas->bytes / page_size;
    if (paginas != NULL) {
        for (size_t i = 0; i < cantidad; i++) {
            if (permisos(paginas[i].msi) < 0) {
                return error_con(EINVAL);
            }
        }
    }

    dsm_pager_t *pager = calloc(1, sizeof *pager);
    if (pager == NULL) {
        return -1;
    }
    int error = ENOMEM;
    pager->paginas = calloc(cantidad, sizeof *pager->paginas);
    pager->trabajadores = calloc(max_trabajadores, sizeof *pager->trabajadores);
    if (pager->paginas == NULL || pager->trabajadores == NULL) {
        goto fallo;
    }
    if (paginas != NULL) {
        memcpy(pager->paginas, paginas, cantidad * sizeof *paginas);
    }
    if (resolver != NULL) {
        pager->imagen = malloc(page_size);
        if (pager->imagen == NULL) {
            goto fallo;
        }
    }
    pager->resolver = resolver;
    pager->resolver_directorio = resolver_directorio;
    pager->solicitante = solicitante;
    pager->timeout_ms = timeout_ms;
    pager->vistas = vistas;
    pager->page_size = page_size;
    pager->max_trabajadores = max_trabajadores;
    pager->preparar = preparar;
    pager->datos = datos;
    for (size_t i = 0; i < max_trabajadores; i++) {
        dsm_solicitud_t *solicitud = &pager->trabajadores[i].solicitud;
        atomic_init(&solicitud->estado, DSM_SOLICITUD_QUIETA);
        solicitud->base = (uintptr_t)vistas->a_r;
        solicitud->bytes = vistas->bytes;
        solicitud->page_size = page_size;
        solicitud->aviso = &pager->aviso;
    }

    error = pthread_mutex_init(&pager->mutex, NULL);
    if (error != 0) {
        goto fallo;
    }
    if (sem_init(&pager->aviso, 0, 0) != 0) {
        error = errno;
        goto fallo_mutex;
    }
    error = pthread_create(&pager->hilo, NULL, paginar, pager);
    if (error != 0) {
        goto fallo_aviso;
    }
    if (dsm_senales_instalar(&pager->acciones) != 0) {
        error = errno;
        pthread_mutex_lock(&pager->mutex);
        solicitar_cierre(pager);
        pthread_mutex_unlock(&pager->mutex);
        pthread_join(pager->hilo, NULL);
        goto fallo_aviso;
    }
    if (paginas != NULL && resolver_directorio == NULL) {
        for (size_t i = 0; i < cantidad; i++) {
            int prot = permisos(paginas[i].msi);
            if (prot != PROT_NONE && dsm_vistas_proteger_pagina(vistas, i, page_size, prot) != 0) {
                error = errno;
                dsm_vistas_proteger(vistas, PROT_NONE);
                dsm_pager_cerrar(pager);
                return error_con(error);
            }
        }
    }
    *salida = pager;
    return 0;

fallo_aviso:
    sem_destroy(&pager->aviso);
fallo_mutex:
    pthread_mutex_destroy(&pager->mutex);
fallo:
    liberar(pager);
    return error_con(error);
}

int dsm_pager_iniciar(dsm_pager_t **salida, dsm_vistas_t *vistas, size_t page_size,
                      size_t max_trabajadores, dsm_pager_preparar_t preparar, void *datos)
{
    return iniciar(salida, vistas, page_size, max_trabajadores, preparar, datos,
                   NULL, 0, 0, NULL, NULL);
}

int dsm_pager_iniciar_msi(dsm_pager_t **salida, dsm_vistas_t *vistas, size_t page_size,
                          size_t max_trabajadores, const dsm_pager_pagina_t *paginas,
                          uint16_t solicitante, uint32_t timeout_ms,
                          dsm_pager_coherencia_t resolver, void *contexto)
{
    return iniciar(salida, vistas, page_size, max_trabajadores, NULL, contexto,
                   paginas, solicitante, timeout_ms, resolver, NULL);
}

int dsm_pager_iniciar_directorio(dsm_pager_t **salida, dsm_vistas_t *vistas, size_t page_size,
                                 size_t max_trabajadores, const dsm_pager_pagina_t *paginas,
                                 uint16_t solicitante, uint32_t timeout_ms,
                                 dsm_pager_fallo_t resolver, void *contexto)
{
    return iniciar(salida, vistas, page_size, max_trabajadores, NULL, contexto,
                   paginas, solicitante, timeout_ms, NULL, resolver);
}

int dsm_pager_consultar_pagina(dsm_pager_t *pager, size_t indice, dsm_pager_pagina_t *pagina)
{
    if (pager == NULL || pagina == NULL || indice >= pager->vistas->bytes / pager->page_size) {
        return error_con(EINVAL);
    }
    if (pager->resolver_directorio != NULL) {
        return error_con(ENOTSUP);
    }
    pthread_mutex_lock(&pager->mutex);
    *pagina = pager->paginas[indice];
    pthread_mutex_unlock(&pager->mutex);
    return 0;
}

int dsm_pager_registrar(dsm_pager_t *pager)
{
    if (pager == NULL) {
        return error_con(EINVAL);
    }

    int resultado = -1;
    int error = ENOSPC;
    pthread_mutex_lock(&pager->mutex);
    if (pager->cerrando) {
        error = EBUSY;
    } else {
        for (size_t i = 0; i < pager->max_trabajadores; i++) {
            struct trabajador *trabajador = &pager->trabajadores[i];
            if (trabajador->registrado) {
                continue;
            }
            resultado = dsm_senales_registrar(&trabajador->solicitud);
            error = errno;
            if (resultado == 0) {
                trabajador->hilo = pthread_self();
                trabajador->registrado = 1;
            }
            break;
        }
    }
    pthread_mutex_unlock(&pager->mutex);
    return resultado == 0 ? 0 : error_con(error);
}

int dsm_pager_desregistrar(dsm_pager_t *pager)
{
    if (pager == NULL) {
        return error_con(EINVAL);
    }

    pthread_t hilo = pthread_self();
    int resultado = -1;
    int error = ENOENT;
    pthread_mutex_lock(&pager->mutex);
    for (size_t i = 0; i < pager->max_trabajadores; i++) {
        struct trabajador *trabajador = &pager->trabajadores[i];
        if (trabajador->registrado && pthread_equal(trabajador->hilo, hilo)) {
            resultado = dsm_senales_desregistrar(&trabajador->solicitud);
            error = errno;
            if (resultado == 0) {
                trabajador->registrado = 0;
            }
            break;
        }
    }
    pthread_mutex_unlock(&pager->mutex);
    return resultado == 0 ? 0 : error_con(error);
}

int dsm_pager_cerrar(dsm_pager_t *pager)
{
    if (pager == NULL) {
        return error_con(EINVAL);
    }

    pthread_mutex_lock(&pager->mutex);
    if (pager->cerrando) {
        pthread_mutex_unlock(&pager->mutex);
        return error_con(EBUSY);
    }
    for (size_t i = 0; i < pager->max_trabajadores; i++) {
        if (pager->trabajadores[i].registrado) {
            pthread_mutex_unlock(&pager->mutex);
            return error_con(EBUSY);
        }
    }
    if (dsm_senales_restaurar(&pager->acciones) != 0) {
        int error = errno;
        pthread_mutex_unlock(&pager->mutex);
        return error_con(error);
    }
    solicitar_cierre(pager);
    pthread_mutex_unlock(&pager->mutex);

    int error = pthread_join(pager->hilo, NULL);
    if (error != 0) {
        return error_con(error);
    }
    sem_destroy(&pager->aviso);
    pthread_mutex_destroy(&pager->mutex);
    liberar(pager);
    return 0;
}
