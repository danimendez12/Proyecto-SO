#ifndef MERGESORT_CONTROL_H
#define MERGESORT_CONTROL_H

#include "dsm.h"

#include <stdint.h>
#include <time.h>

enum {
    MERGESORT_CONTROL_ARRANQUE = 0,
    MERGESORT_CONTROL_ORDENAR = 1,
    MERGESORT_CONTROL_FUSIONAR = 2,
    MERGESORT_CONTROL_CERRAR = 3
};

/* Creador 0; indice del trabajador = nodo - 1, entre 0 y trabajadores - 1.
 * bloque y ancho se expresan en elementos int32_t, alineados a paginas. */
typedef struct mergesort_control_datos {
    uint64_t elementos, bloque;
    uint32_t trabajadores, hilos, page_size;
    dsm_reserva_id_t a, b;
} mergesort_control_datos_t;

typedef struct mergesort_control_fase {
    uint64_t generacion, ancho;
    uint32_t tipo, origen; /* 0 = A, 1 = B; fusion escribe en 1 - origen. */
} mergesort_control_fase_t;

/* Todas las funciones devuelven DSM_OK o DSM_ERR_*. No se comparten punteros,
 * mutexes ni tablas de tareas. La cabecera ocupa una pagina y cada trabajador
 * dispone de otra pagina para su marca, con un unico escritor.
 * Crear debe ser la primera reserva: el caller comprueba antes que el nodo 0
 * aporte al menos (1 + trabajadores) * page_size, para no esperar donantes
 * antes de anunciar el control. Solo despues de la barrera 0 se crean A/B. */
int mergesort_control_crear(dsm_context_t *ctx, dsm_proc_id_t proceso,
                            const mergesort_control_datos_t *datos, dsm_reserva_id_t *id);
int mergesort_control_abrir(dsm_context_t *ctx, dsm_reserva_id_t id, uint32_t indice,
                            mergesort_control_datos_t *datos);
int mergesort_control_datos(dsm_context_t *ctx, dsm_reserva_id_t id,
                            mergesort_control_datos_t *datos);
int mergesort_control_publicar_datos(dsm_context_t *ctx, dsm_reserva_id_t id,
                                     const mergesort_control_datos_t *datos);
/* gen 0 = LISTO; gen 1 = ORDENAR; siguientes = FUSIONAR o CERRAR.
 * Una marca de exito es definitiva para su generacion, nunca se retracta.
 * En CERRAR, marcar solo despues de join/desregistro y desvincular A y B.
 * Despues se desvincula control y se conserva el servicio con cerrar(). */
int mergesort_control_marcar(dsm_context_t *ctx, dsm_reserva_id_t id, uint32_t indice,
                             uint64_t generacion, int resultado);
int mergesort_control_esperar(dsm_context_t *ctx, dsm_reserva_id_t id, uint64_t generacion,
                              const struct timespec *plazo);
/* Publicar gen actual + 1 exige todas las marcas previas; si faltan devuelve
 * OCUPADA sin avanzar. Generaciones saltadas/antiguas devuelven PROTO.
 * ORDENAR: ancho=bloque, origen=0. FUSIONAR: ancho del run de entrada y origen
 * actual. CERRAR: ancho=0, origen del resultado final. Un unico publicador. */
int mergesort_control_publicar_fase(dsm_context_t *ctx, dsm_reserva_id_t id,
                                    const mergesort_control_fase_t *fase);
int mergesort_control_esperar_fase(dsm_context_t *ctx, dsm_reserva_id_t id,
                                   uint64_t anterior, mergesort_control_fase_t *fase,
                                   const struct timespec *plazo);
int mergesort_control_abortar(dsm_context_t *ctx, dsm_reserva_id_t id, int resultado);

/* Plazos absolutos CLOCK_MONOTONIC. El sondeo solo espera entre lecturas DSM;
 * ninguna pausa confirma exito. Cada llamada DSM conserva su rpc_timeout INI.
 * Limpieza intenta la operacion una vez incluso si vencio el plazo; solo los
 * reintentos de OCUPADA estan condicionados por el tiempo restante. */
int mergesort_control_plazo(uint64_t segundos, struct timespec *plazo);
int mergesort_control_desvincular(dsm_context_t *ctx, dsm_reserva_id_t id,
                                  const struct timespec *plazo);
int mergesort_control_liberar(dsm_context_t *ctx, dsm_reserva_id_t id,
                              const struct timespec *plazo);
int mergesort_control_cerrar(dsm_context_t *ctx, const struct timespec *plazo);

#endif
