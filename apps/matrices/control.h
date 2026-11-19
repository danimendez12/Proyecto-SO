#ifndef MATRICES_CONTROL_H
#define MATRICES_CONTROL_H

#include "dsm.h"

#include <stdint.h>
#include <time.h>

/* Metadatos sin punteros. El creador es 0 y los trabajadores son 1..W. */
typedef struct matrices_control_datos {
    uint64_t filas;
    uint64_t interior;
    uint64_t columnas;
    dsm_reserva_id_t a, b, c;
    uint32_t trabajadores;
    uint32_t page_size;
} matrices_control_datos_t;

/* Todas las operaciones devuelven DSM_OK o un DSM_ERR_*. Crear solicita una
 * pagina; debe ser la primera reserva del creador, antes de anunciar su ID.
 * Publicar actualiza metadatos y fase, sin sobrescribir marcas de trabajadores.
 * Cada nodo escribe exclusivamente su marca mediante el API controlado. */
int matrices_control_crear(dsm_context_t *ctx, dsm_proc_id_t proceso,
                           const matrices_control_datos_t *datos, dsm_reserva_id_t *id);
int matrices_control_abrir(dsm_context_t *ctx, dsm_reserva_id_t id,
                           dsm_node_id_t nodo, matrices_control_datos_t *datos);
int matrices_control_listo(dsm_context_t *ctx, dsm_reserva_id_t id, dsm_node_id_t nodo);
int matrices_control_publicar(dsm_context_t *ctx, dsm_reserva_id_t id,
                              const matrices_control_datos_t *datos);
int matrices_control_esperar_listos(dsm_context_t *ctx, dsm_reserva_id_t id,
                                    const struct timespec *plazo);
int matrices_control_esperar_datos(dsm_context_t *ctx, dsm_reserva_id_t id,
                                   matrices_control_datos_t *datos,
                                   const struct timespec *plazo);
/* FIN se publica despues de desregistrar los hilos y desvincular A, B y C,
 * tambien cuando el calculo o la apertura de una matriz haya fallado. */
int matrices_control_fin(dsm_context_t *ctx, dsm_reserva_id_t id,
                         dsm_node_id_t nodo, int resultado);
int matrices_control_esperar_fin(dsm_context_t *ctx, dsm_reserva_id_t id,
                                 const struct timespec *plazo);
int matrices_control_abortar(dsm_context_t *ctx, dsm_reserva_id_t id, int resultado);

/* Reintentan solo OCUPADA: el contexto y su servicio TCP permanecen activos
 * mientras el creador verifica y devuelve aportes. Tras un fallo el contexto
 * no se considera cerrado. Los plazos son absolutos CLOCK_MONOTONIC; cada
 * llamada DSM conserva ademas su propio rpc_timeout configurado en el INI. */
int matrices_control_desvincular(dsm_context_t *ctx, dsm_reserva_id_t id,
                                 const struct timespec *plazo);
int matrices_control_liberar(dsm_context_t *ctx, dsm_reserva_id_t id,
                             const struct timespec *plazo);
int matrices_control_cerrar(dsm_context_t *ctx, const struct timespec *plazo);
int matrices_control_plazo(uint64_t segundos, struct timespec *plazo);

#endif
