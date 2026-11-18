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
int dsm_coherencia_abrir(dsm_coherencia_t *coherencia, uint32_t reserva,
                         dsm_pagina_desc_t *pagina, dsm_vistas_t **vista);
/* Adaptador de dsm_pager_iniciar_directorio. El contexto es dsm_coherencia_t*. */
int dsm_coherencia_resolver(const dsm_pager_peticion_t *peticion, void *contexto);

#endif
