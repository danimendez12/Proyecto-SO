#ifndef DSM_CONFIG_H
#define DSM_CONFIG_H

/* Carga del archivo compartido: clave = valor, nodos con un puerto cada uno. */

#include <stdint.h>

enum { DSM_CONFIG_MAX_NODOS = 16, DSM_CONFIG_NOMBRE = 64 };

typedef struct dsm_nodo_cfg {
    uint16_t id;
    uint16_t puerto;
    uint32_t ipv4_host;
    char nombre[DSM_CONFIG_NOMBRE];
} dsm_nodo_cfg_t;

typedef struct dsm_config {
    uint32_t page_size;
    uint64_t memoria_inicial;
    uint16_t max_nodos;
    uint16_t max_hilos;
    uint32_t heartbeat_intervalo_ms;
    uint32_t heartbeat_fallos;
    uint32_t rpc_timeout_ms;
    uint16_t puerto_rango_inicio;
    uint16_t puerto_rango_fin;
    uint16_t cantidad_nodos;
    dsm_nodo_cfg_t nodos[DSM_CONFIG_MAX_NODOS];
} dsm_config_t;

int dsm_config_cargar(const char *ruta, dsm_config_t *salida);
const dsm_nodo_cfg_t *dsm_config_nodo(const dsm_config_t *cfg, uint16_t id);

#endif
