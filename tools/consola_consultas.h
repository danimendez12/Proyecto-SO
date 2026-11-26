#ifndef DSM_CONSOLA_CONSULTAS_H
#define DSM_CONSOLA_CONSULTAS_H

#include "dsm.h"
#include "config.h"

/* 1 reconocido, 0 ajeno; un error devuelve el codigo DSM negativo.
 * No modifica el registro de reservas de la consola. */
int consola_consultar(dsm_context_t *ctx, const dsm_config_t *cfg, uint16_t nodo,
                       int argc, char *const argv[], FILE *salida);

#endif
