#ifndef DSM_DEMANDA_GRUPO_PRIVADO_H
#define DSM_DEMANDA_GRUPO_PRIVADO_H

#include "demanda_grupo.h"

/* Formato privado DGR3, compartido por las dos apps y sus aceptaciones.
 * Conserva la ABI comun de las apps; no cambia el wire de ANUNCIO DSM.
 * Solo esta lista serializada usa IDs de 8 bits. El motor conserva uint16_t. */
enum { GRUPO_MAGIC = 0x44475233, FASE_MAGIC = 0x44465332,
       ENTRAR = 1, PROPONER, ACEPTAR, CONFIRMAR, CONFIRMADO, EJECUTAR, FALLAR };

struct miembro {
    uint64_t nonce, token, digest;
    uint32_t magic;
    int32_t resultado;
    uint16_t etapa, lider, cantidad;
    uint8_t ids[DSM_CONFIG_MAX_NODOS];
};

_Static_assert(DSM_CONFIG_MAX_NODOS <= UINT8_MAX, "Los IDs compactos deben caber");
_Static_assert(sizeof(struct miembro) <= DSM_ANUNCIO_DATOS, "Grupo excede aviso");

int demanda_miembro_desde_grupo(const demanda_grupo_t *g, unsigned etapa,
                                struct miembro *salida);
int demanda_miembro_codificar(const struct miembro *msg, void *salida,
                              size_t capacidad, size_t *bytes);
int demanda_miembro_decodificar(const void *datos, size_t bytes, struct miembro *salida);

#endif
