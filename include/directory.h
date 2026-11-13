#ifndef DSM_DIRECTORY_H
#define DSM_DIRECTORY_H

/* Tabla basica de paginas y el manifiesto de una reserva. */

#include "config.h"
#include "memory.h"
#include "protocol.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

enum {
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
    uint64_t mascara_copias;
    void *ubicacion_local;
    uint32_t orden;
} dsm_pagina_desc_t;

typedef struct dsm_directorio {
    uint16_t nodo_id;
    uint16_t cantidad_nodos;
    uint32_t page_size;
    uint32_t rpc_timeout_ms;
    uint64_t memoria_inicial;
    uint64_t limites_gpn[DSM_CONFIG_MAX_NODOS + 1];
    uint32_t paginas_totales;
    uint32_t paginas_libres;
    uint32_t reservas_globales;
    uint16_t siguiente_local;
    int tiene_manifiesto;
    dsm_region_info_t manifiesto;
    /* Cada reserva de una pagina ocupa el mismo indice en ambos arreglos. */
    dsm_pagina_desc_t paginas[DSM_DIR_MAX_PAGINAS];
    size_t npaginas;
    dsm_vistas_t vistas[DSM_DIR_MAX_VISTAS];
    size_t nvistas;
    pthread_mutex_t mutex;
    int inicializado;
} dsm_directorio_t;

/* Un directorio inicializado contiene un mutex y no se copia. */
int dsm_directorio_init(dsm_directorio_t *dir, const dsm_config_t *cfg,
                        uint16_t nodo_id);
/* Cerrar tras terminar las operaciones y el uso de los punteros prestados. */
void dsm_directorio_cerrar(dsm_directorio_t *dir);
int dsm_directorio_reservar_pagina(dsm_directorio_t *dir, dsm_pagina_desc_t *salida);
int dsm_directorio_consultar(const dsm_directorio_t *dir, uint32_t reserva_id,
                             uint32_t primer_tramo, dsm_region_info_t *info,
                             uint16_t *estado);
int dsm_directorio_instalar(dsm_directorio_t *dir, const dsm_region_info_t *info);
/* Estas dos operaciones requieren que el llamador sea el unico lector del fd. */
int dsm_directorio_publicar(dsm_directorio_t *dir, int fd, uint32_t *seq);
int dsm_directorio_traer(dsm_directorio_t *dir, int fd, uint32_t reserva_id,
                         uint32_t *seq, dsm_cabecera_t *cab, dsm_region_info_t *info);
/* Operaciones para un despachador que ya controla la lectura del fd y seq. */
int dsm_directorio_solicitar(const dsm_directorio_t *dir, int fd, uint32_t reserva_id,
                            uint32_t *seq, uint32_t *id_lookup);
int dsm_directorio_responder(dsm_directorio_t *dir, int fd, uint16_t peer,
                             const dsm_cabecera_t *pedido,
                             const dsm_region_lookup_t *consulta, uint32_t *seq);
int dsm_directorio_codificar_info(dsm_directorio_t *dir, const dsm_cabecera_t *pedido,
                                  const dsm_region_lookup_t *consulta, uint32_t id_msg,
                                  uint8_t *buf, size_t cap, size_t *n);
int dsm_directorio_recibir_info(dsm_directorio_t *dir, uint32_t reserva_id,
                                uint32_t id_lookup, const uint8_t *buf, size_t n,
                                dsm_cabecera_t *cab, dsm_region_info_t *info);
void *dsm_directorio_c_r_reserva(const dsm_directorio_t *dir, uint32_t reserva_id);
/* Acceso de conveniencia cuando existe una sola vista. */
void *dsm_directorio_c_r(const dsm_directorio_t *dir);

/* Copia el descriptor y presta la vista hasta el cierre del directorio. */
int dsm_directorio_vista(dsm_directorio_t *dir, uint32_t reserva_id,
                         dsm_pagina_desc_t *desc, dsm_vistas_t **vista);
/* Presta datos de C_r luego de retirar la escritura en A_r. */
int dsm_directorio_servir_pagina(dsm_directorio_t *dir, uint16_t solicitante,
                                 const dsm_page_request_t *pedido, dsm_page_data_t *datos,
                                 int *cambio);
/* Instala los bytes por C_r. El paginador habilita luego la lectura de A_r. */
int dsm_directorio_compartir_pagina(dsm_directorio_t *dir, uint32_t reserva_id,
                                     const dsm_page_data_t *datos);

#endif
