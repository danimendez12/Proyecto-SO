#ifndef DSM_SIGNALS_H
#define DSM_SIGNALS_H

#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

enum {
    DSM_SOLICITUD_QUIETA,
    DSM_SOLICITUD_PENDIENTE,
    DSM_SOLICITUD_LISTA,
    DSM_SOLICITUD_ERROR
};

typedef enum dsm_acceso {
    DSM_FALLO_LECTURA,
    DSM_FALLO_ESCRITURA
} dsm_acceso_t;

/* Los nodos publicados se conservan mientras haya trabajadores registrados. */
typedef struct dsm_ventana_senal {
    uintptr_t base;
    size_t bytes;
    size_t page_size;
    struct dsm_ventana_senal *siguiente;
} dsm_ventana_senal_t;

typedef struct dsm_solicitud {
    atomic_int estado;
    uintptr_t base;
    size_t bytes;
    size_t page_size;
    size_t pagina;
    dsm_acceso_t acceso;
    const _Atomic(dsm_ventana_senal_t *) *ventanas;
    dsm_ventana_senal_t *ventana;
    sem_t *aviso;
    int usr1_bloqueada;
} dsm_solicitud_t;

typedef struct dsm_acciones {
    struct sigaction segv;
    struct sigaction usr1;
} dsm_acciones_t;

int dsm_senales_instalar(dsm_acciones_t *acciones);
int dsm_senales_restaurar(const dsm_acciones_t *acciones);

/* El trabajador registra su ranura inicializada en QUIETA y reserva SIGUSR1. */
int dsm_senales_registrar(dsm_solicitud_t *solicitud);
/* El paginador no puede enviar otra notificacion al trabajador al desregistrar. */
int dsm_senales_desregistrar(dsm_solicitud_t *solicitud);

#endif
