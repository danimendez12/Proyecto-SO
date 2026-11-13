#ifndef DSM_PROTOCOL_H
#define DSM_PROTOCOL_H

/* Encabezado de 32 bytes en big-endian y mensajes TCP del prototipo. */

#include <stddef.h>
#include <stdint.h>

enum {
    DSM_MAGIC = 0x44534D31,
    DSM_PROTO_VERSION = 1,
    DSM_FLAG_RESPUESTA = 0x0001,
    DSM_CABECERA_BYTES = 32,
    DSM_LONGITUD_MAX = 1048576,
    DSM_PAGE_REQUEST_BYTES = 48,
    DSM_PAGE_DATA_BASE = 80,
    DSM_NOMBRE_MAX = 64,
    DSM_MAX_FICHAS = 16,
    DSM_MAX_TRAMOS = 16
};

enum {
    DSM_TIPO_NODE_HELLO = 0x01,
    DSM_TIPO_NODE_WELCOME = 0x02,
    DSM_TIPO_HEARTBEAT = 0x05,
    DSM_TIPO_HEARTBEAT_ACK = 0x06,
    DSM_TIPO_PAGE_REQUEST = 0x10,
    DSM_TIPO_PAGE_DATA = 0x11,
    DSM_TIPO_REGION_LOOKUP = 0x32,
    DSM_TIPO_REGION_INFO = 0x33
};

enum {
    DSM_ESTADO_OK = 0,
    DSM_ESTADO_RANGO = 12
};

enum {
    DSM_MSI_I = 0,
    DSM_MSI_S = 1,
    DSM_MSI_M = 2,
    DSM_ACCESO_LECTURA = 0,
    DSM_ACCESO_ESCRITURA = 1,
    DSM_PAGE_PERMISO_LEER = 1,
    DSM_PAGE_PERMISO_ESCRIBIR = 3,
    DSM_PAGE_DATA_SIN_CARGA = 0x01
};

enum {
    DSM_NODO_VACIO = 0,
    DSM_NODO_UNION = 1,
    DSM_NODO_EN_LINEA = 2,
    DSM_NODO_SOSPECHA = 3,
    DSM_NODO_FUERA = 4
};

typedef struct dsm_cabecera {
    uint8_t version;
    uint8_t tipo;
    uint16_t banderas;
    uint32_t longitud;
    uint16_t origen;
    uint16_t destino;
    uint32_t id_msg;
    uint32_t id_corr;
    uint16_t estado;
} dsm_cabecera_t;

typedef struct dsm_hello {
    uint16_t node_id;
    uint16_t conocidos;
    uint32_t ipv4_host;
    uint16_t puerto;
    uint64_t memoria;
    uint64_t base;
    char nombre[DSM_NOMBRE_MAX];
} dsm_hello_t;

typedef struct dsm_ficha {
    uint16_t id;
    uint8_t estado;
    uint32_t ipv4_host;
    uint16_t puerto;
    uint64_t memoria;
    uint64_t base;
    uint32_t libres;
    uint32_t totales;
    char nombre[DSM_NOMBRE_MAX];
} dsm_ficha_t;

typedef struct dsm_welcome {
    uint16_t emisor;
    uint16_t n;
    uint32_t epoca;
    dsm_ficha_t fichas[DSM_MAX_FICHAS];
} dsm_welcome_t;

typedef struct dsm_heartbeat {
    uint64_t marca;
    uint64_t instante_ms;
} dsm_heartbeat_t;

typedef struct dsm_page_request {
    uint64_t gpn;
    uint32_t proc_id;
    uint8_t acceso;
    uint8_t msi;
} dsm_page_request_t;

typedef struct dsm_page_data {
    uint64_t gpn;
    uint32_t proc_id;
    uint32_t reserva_id;
    uint32_t orden;
    uint16_t hogar;
    uint16_t poseedor;
    uint16_t msi;
    uint8_t permiso;
    uint8_t banderas;
    uint64_t mascara_copias;
    uint32_t page_nbytes;
    uint32_t version;
    /* Al decodificar, apunta a la carga prestada del bufer recibido. */
    const uint8_t *datos;
} dsm_page_data_t;

typedef struct dsm_region_lookup {
    uint32_t reserva_id;
    uint32_t primer_tramo;
} dsm_region_lookup_t;

typedef struct dsm_tramo {
    uint32_t orden_inicial;
    uint32_t cantidad;
    uint64_t gpn_inicial;
} dsm_tramo_t;

typedef struct dsm_region_info {
    uint32_t reserva_id;
    uint32_t total_tramos;
    uint64_t tamano_bytes;
    uint32_t primer_tramo;
    uint32_t cantidad_tramos;
    uint32_t total_paginas;
    dsm_tramo_t tramos[DSM_MAX_TRAMOS];
} dsm_region_info_t;

int dsm_cabecera_codificar(uint8_t *dst, const dsm_cabecera_t *cab);
int dsm_cabecera_decodificar(const uint8_t *src, size_t n, dsm_cabecera_t *cab);

int dsm_hello_codificar(uint8_t *dst, size_t cap, dsm_cabecera_t *cab,
                        const dsm_hello_t *msg, size_t *out);
int dsm_hello_decodificar(const uint8_t *src, size_t n, dsm_cabecera_t *cab,
                          dsm_hello_t *msg);

int dsm_welcome_codificar(uint8_t *dst, size_t cap, dsm_cabecera_t *cab,
                          const dsm_welcome_t *msg, size_t *out);
int dsm_welcome_decodificar(const uint8_t *src, size_t n, dsm_cabecera_t *cab,
                            dsm_welcome_t *msg);

int dsm_heartbeat_codificar(uint8_t *dst, size_t cap, dsm_cabecera_t *cab,
                            const dsm_heartbeat_t *msg, size_t *out);
int dsm_heartbeat_decodificar(const uint8_t *src, size_t n, dsm_cabecera_t *cab,
                              dsm_heartbeat_t *msg);

int dsm_page_request_codificar(uint8_t *dst, size_t cap, dsm_cabecera_t *cab,
                                const dsm_page_request_t *msg, size_t *out);
int dsm_page_request_decodificar(const uint8_t *src, size_t n, dsm_cabecera_t *cab,
                                  dsm_page_request_t *msg);
int dsm_page_data_codificar(uint8_t *dst, size_t cap, dsm_cabecera_t *cab,
                             const dsm_page_data_t *msg, size_t *out);
int dsm_page_data_decodificar(const uint8_t *src, size_t n, dsm_cabecera_t *cab,
                               dsm_page_data_t *msg);

int dsm_region_lookup_codificar(uint8_t *dst, size_t cap, dsm_cabecera_t *cab,
                                const dsm_region_lookup_t *msg, size_t *out);
int dsm_region_lookup_decodificar(const uint8_t *src, size_t n,
                                  dsm_cabecera_t *cab, dsm_region_lookup_t *msg);

int dsm_region_info_codificar(uint8_t *dst, size_t cap, dsm_cabecera_t *cab,
                              const dsm_region_info_t *msg, size_t *out);
int dsm_region_info_decodificar(const uint8_t *src, size_t n,
                                dsm_cabecera_t *cab, dsm_region_info_t *msg);

#endif
