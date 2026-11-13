#include "pager.h"
#include "signals.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
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
    unsigned char *paginas_listas;
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

static int cargar_pagina(dsm_pager_t *pager, size_t indice)
{
    if (indice >= pager->vistas->bytes / pager->page_size) {
        return DSM_SOLICITUD_ERROR;
    }
    if (!pager->paginas_listas[indice]) {
        unsigned char *pagina_cr = (unsigned char *)pager->vistas->c_r + indice * pager->page_size;
        if (pager->preparar(pagina_cr, indice, pager->page_size, pager->datos) != 0 ||
            dsm_vistas_proteger_pagina(pager->vistas, indice, pager->page_size, PROT_READ) != 0) {
            return DSM_SOLICITUD_ERROR;
        }
        pager->paginas_listas[indice] = 1;
    }
    return DSM_SOLICITUD_LISTA;
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
        int estado = cargar_pagina(pager, solicitud->pagina);

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
    free(pager->paginas_listas);
    free(pager);
}

int dsm_pager_iniciar(dsm_pager_t **salida, dsm_vistas_t *vistas, size_t page_size,
                      size_t max_trabajadores, dsm_pager_preparar_t preparar, void *datos)
{
    if (salida == NULL) {
        return error_con(EINVAL);
    }
    *salida = NULL;
    long pagina_so = sysconf(_SC_PAGESIZE);
    if (vistas == NULL || vistas->a_r == NULL || vistas->c_r == NULL || vistas->bytes == 0 ||
        pagina_so <= 0 || page_size == 0 || page_size % (size_t)pagina_so != 0 ||
        vistas->bytes % page_size != 0 || (uintptr_t)vistas->a_r % (size_t)pagina_so != 0 ||
        max_trabajadores == 0 || max_trabajadores > DSM_PAGER_MAX_TRABAJADORES || preparar == NULL) {
        return error_con(EINVAL);
    }

    dsm_pager_t *pager = calloc(1, sizeof *pager);
    if (pager == NULL) {
        return -1;
    }
    int error = ENOMEM;
    pager->paginas_listas = calloc(vistas->bytes / page_size, sizeof *pager->paginas_listas);
    pager->trabajadores = calloc(max_trabajadores, sizeof *pager->trabajadores);
    if (pager->paginas_listas == NULL || pager->trabajadores == NULL) {
        goto fallo;
    }
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
