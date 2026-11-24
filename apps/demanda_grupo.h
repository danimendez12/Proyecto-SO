#ifndef DSM_DEMANDA_GRUPO_H
#define DSM_DEMANDA_GRUPO_H

#include "config.h"
#include "dsm.h"
#include "protocol.h"

#include <stdint.h>
#include <time.h>

/* Coordinador temporal de una aplicacion: los puestos DSM nunca son ordinales
 * de trabajo. Los avisos solo acuerdan el grupo; datos y barreras usan DSM. */
typedef struct demanda_grupo {
    dsm_context_t *ctx;
    uint16_t yo, lider, cantidad, rango;
    uint16_t ids[DSM_CONFIG_MAX_NODOS];
    uint64_t nonce, token, digest, limite;
    char clave[DSM_ANUNCIO_CLAVE], recursos[DSM_ANUNCIO_CLAVE];
} demanda_grupo_t;

typedef struct demanda_fase {
    uint64_t token;
    uint32_t magic, generacion, tipo, origen;
    uint64_t ancho;
    int32_t resultado;
    uint32_t reservado;
} demanda_fase_t;

enum { DEMANDA_FASE_PREPARAR = 0, DEMANDA_FASE_CERRAR = 3 };

uint64_t demanda_transcurrido_ms(const struct timespec *inicio, const struct timespec *fin);
uint64_t demanda_ahora_ms(void);
int demanda_grupo_iniciar(demanda_grupo_t *g, dsm_context_t *ctx, uint16_t yo,
                          const char *parametros);
int demanda_grupo_publicar(demanda_grupo_t *g, const void *datos, size_t n);
int demanda_grupo_recibir(demanda_grupo_t *g, void *datos, size_t n);
int demanda_grupo_estado(demanda_grupo_t *g);
void demanda_grupo_error(demanda_grupo_t *g, int resultado);
int demanda_grupo_retirar(demanda_grupo_t *g);

int demanda_fase_publicar(demanda_grupo_t *g, dsm_reserva_id_t control,
                          demanda_fase_t *fase);
int demanda_fase_esperar(demanda_grupo_t *g, dsm_reserva_id_t control,
                        uint32_t generacion, demanda_fase_t *fase);
int demanda_marcar(demanda_grupo_t *g, dsm_reserva_id_t control,
                   uint32_t generacion, int resultado);
int demanda_marcas_esperar(demanda_grupo_t *g, dsm_reserva_id_t control,
                          uint32_t generacion);

/* Solo OCUPADA se reintenta. Cada exito consume el ID/contexto una sola vez. */
int demanda_reserva_retirar(dsm_context_t *ctx, dsm_reserva_id_t *id,
                            int propia, uint64_t limite);
int demanda_contexto_cerrar(dsm_context_t **ctx, uint64_t limite);
int demanda_mapa(dsm_context_t *ctx, const char *nombre, dsm_reserva_id_t id,
                 size_t bytes, size_t pagina);

#endif
