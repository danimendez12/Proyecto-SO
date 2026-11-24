#ifndef DSM_AVISO_H
#define DSM_AVISO_H

/* Avisos replicados por inundación. Cada nodo guarda la tabla completa.
 * No hay un nodo que centralice las claves. */

#include "coherence.h"

#include <stddef.h>
#include <stdint.h>

int dsm_aviso_recibir(dsm_coherencia_t *coherencia, const uint8_t *buf, size_t n);
void dsm_aviso_reenviar(dsm_coherencia_t *coherencia, uint16_t par);
int dsm_aviso_emitir(dsm_coherencia_t *coherencia, uint16_t autor, const char *clave,
                     const void *datos, size_t n);
/* Devuelve DSM_OK si hay un aviso de ese tamaño. Si hay varios, elige el autor menor. */
int dsm_aviso_leer(const char *clave, uint16_t *autor, void *datos, size_t n);
/* Lista los autores de una clave. datos puede ser NULL. */
int dsm_aviso_autores(const char *clave, uint16_t *autores, size_t cap, size_t *cantidad);

#endif
