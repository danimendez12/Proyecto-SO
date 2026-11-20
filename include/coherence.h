#ifndef DSM_COHERENCE_H
#define DSM_COHERENCE_H

#include "directory.h"
#include "pager.h"

typedef struct dsm_coherencia dsm_coherencia_t;

/* cfg y directorio viven hasta el cierre. El servicio controla la red y el
 * despachador. El directorio es la unica autoridad sobre Ar y la copia local.
 */
int dsm_coherencia_iniciar(dsm_coherencia_t **salida, const dsm_config_t *cfg,
                           uint16_t nodo, dsm_directorio_t *directorio);
/* Cerrar despues de terminar los trabajadores y las llamadas externas. */
void dsm_coherencia_cerrar(dsm_coherencia_t *coherencia);
int dsm_coherencia_esperar(dsm_coherencia_t *coherencia, uint16_t par);
/* Cada operacion de capacidad conserva el token aleatorio de esta sesion.
 * PREPARE retiene hasta max_pag paginas reales, sin publicar la reserva.
 * Los cuatro metodos admiten hogar local sin una espera TCP.
 */
int dsm_coherencia_preparar(dsm_coherencia_t *coherencia, uint16_t hogar,
                            uint32_t id, uint32_t orden, uint32_t max_pag,
                            uint32_t max_tramos, dsm_region_info_t *parcial);
int dsm_coherencia_confirmar(dsm_coherencia_t *coherencia, uint16_t hogar,
                             uint32_t id, const dsm_region_info_t *manifiesto);
int dsm_coherencia_cancelar(dsm_coherencia_t *coherencia, uint16_t hogar, uint32_t id);
int dsm_coherencia_soltar(dsm_coherencia_t *coherencia, uint16_t hogar, uint32_t id);
/* Barrera FIFO del hogar: devuelve M y revoca las copias de este nodo antes
 * de reconocer. No quita el pin que mantiene el creador de la reserva.
 */
int dsm_coherencia_cerrar_copia(dsm_coherencia_t *coherencia, uint16_t hogar, uint32_t id);
int dsm_coherencia_abrir(dsm_coherencia_t *coherencia, uint32_t reserva,
                         dsm_pagina_desc_t *pagina, dsm_vistas_t **vista);
/* Espera la retirada de las copias y la confirmacion del creador. */
int dsm_coherencia_desvincular(dsm_coherencia_t *coherencia, uint32_t reserva);
int dsm_coherencia_asociar(dsm_coherencia_t *coherencia, dsm_pager_t *pager, uint32_t id,
                          dsm_vistas_t *vista, size_t *bytes_logicos);
/* Adaptador de dsm_pager_iniciar_directorio. El contexto es dsm_coherencia_t*. */
int dsm_coherencia_resolver(const dsm_pager_peticion_t *peticion, void *contexto);
/* Reutiliza la resolucion de permisos y copia solo un trozo de una pagina.
 * El buffer scratch no pertenece a una vista de aplicacion.
 */
int dsm_coherencia_copiar_controlado(dsm_coherencia_t *coherencia, uint32_t reserva,
                                     uint64_t gpn, size_t offset, void *scratch, size_t bytes,
                                     int escritura, const struct timespec *plazo);

#endif
