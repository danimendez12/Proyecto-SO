#ifndef DSM_AVISO_H
#define DSM_AVISO_H

/* Avisos de la sesion actual, enviados directamente por su autor en la malla.
 * La tabla y su trabajador de salida pertenecen a una sola coherencia. */

#include "coherence.h"

#include <stddef.h>
#include <stdint.h>

typedef struct dsm_avisos dsm_avisos_t;
int dsm_avisos_crear(dsm_avisos_t **out, dsm_coherencia_t *coherencia,
                      uint16_t self, uint16_t cantidad, uint64_t sesion);
void dsm_avisos_activar(dsm_avisos_t *avisos);
void dsm_avisos_parar(dsm_avisos_t *avisos);
void dsm_avisos_destruir(dsm_avisos_t *avisos);
int dsm_aviso_recibir(dsm_coherencia_t *coherencia, const uint8_t *buf, size_t n);
void dsm_aviso_estado(dsm_coherencia_t *coherencia, uint16_t par, uint8_t estado);
int dsm_aviso_emitir(dsm_coherencia_t *coherencia, uint16_t autor, const char *clave,
                     const void *datos, size_t n);
/* Devuelve DSM_OK si hay un aviso de ese tamaño. Si hay varios, elige el autor menor. */
int dsm_aviso_leer(dsm_coherencia_t *coherencia, const char *clave,
                    uint16_t *autor, void *datos, size_t n);
int dsm_aviso_leer_autor(dsm_coherencia_t *coherencia, const char *clave,
                          uint16_t autor, void *datos, size_t n);
int dsm_aviso_quitar(dsm_coherencia_t *coherencia, const char *clave);
int dsm_aviso_autores(dsm_coherencia_t *coherencia, const char *clave,
                       uint16_t *autores, size_t cap, size_t *cantidad);

#endif
