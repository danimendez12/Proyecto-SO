#ifndef DSM_DIRECTORY_H
#define DSM_DIRECTORY_H

/* Tabla basica de paginas y el manifiesto de una reserva. */

#include "config.h"
#include "memory.h"
#include "protocol.h"

#include <stddef.h>
#include <stdint.h>

enum {
    DSM_MSI_I = 0,
    DSM_MSI_S = 1,
    DSM_MSI_M = 2,
    DSM_POSEEDOR_NINGUNO = 0xFFFF,
    DSM_DIR_MAX_PAGINAS = 8,
    DSM_DIR_MAX_VISTAS = 4
};

typedef struct dsm_pagina_desc {
    uint64_t gpn;
    uint16_t hogar;
    uint16_t poseedor;
    uint32_t reserva;
    uint8_t msi;
    uint32_t version;
    void *ubicacion_local;
    uint32_t orden;
} dsm_pagina_desc_t;

typedef struct dsm_directorio {
    uint16_t nodo_id;
    uint32_t page_size;
    uint64_t memoria_inicial;
    uint32_t paginas_totales;
    uint32_t paginas_libres;
    uint32_t reservas_globales;
    uint16_t siguiente_local;
    int tiene_manifiesto;
    dsm_region_info_t manifiesto;
    dsm_pagina_desc_t paginas[DSM_DIR_MAX_PAGINAS];
    size_t npaginas;
    dsm_vistas_t vistas[DSM_DIR_MAX_VISTAS];
    size_t nvistas;
} dsm_directorio_t;

int dsm_directorio_init(dsm_directorio_t *dir, const dsm_config_t *cfg,
                        uint16_t nodo_id);
void dsm_directorio_cerrar(dsm_directorio_t *dir);
int dsm_directorio_reservar_pagina(dsm_directorio_t *dir, dsm_pagina_desc_t *salida);
int dsm_directorio_consultar(const dsm_directorio_t *dir, uint32_t reserva_id,
                             uint32_t primer_tramo, dsm_region_info_t *info,
                             uint16_t *estado);
int dsm_directorio_instalar(dsm_directorio_t *dir, const dsm_region_info_t *info);
int dsm_directorio_publicar(dsm_directorio_t *dir, int fd, uint32_t *seq);
int dsm_directorio_traer(dsm_directorio_t *dir, int fd, uint32_t reserva_id,
                         uint32_t *seq, dsm_cabecera_t *cab, dsm_region_info_t *info);
void *dsm_directorio_c_r(const dsm_directorio_t *dir);

#endif
