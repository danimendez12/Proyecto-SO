#ifndef DSM_MEMORY_H
#define DSM_MEMORY_H

/* Dos vistas MAP_SHARED de un mismo memfd: A_r para la aplicacion y C_r para el gestor. */

#include <stddef.h>

typedef struct dsm_vistas {
    int fd;
    size_t bytes;
    void *a_r;
    void *c_r;
} dsm_vistas_t;

int dsm_vistas_crear(size_t bytes, dsm_vistas_t *salida);
int dsm_vistas_proteger(dsm_vistas_t *vistas, int prot);
/* Cambia solo la pagina indicada de A_r con un tamano multiplo de la pagina del sistema. */
int dsm_vistas_proteger_pagina(dsm_vistas_t *vistas, size_t indice, size_t page_size, int prot);
void dsm_vistas_cerrar(dsm_vistas_t *vistas);

#endif
