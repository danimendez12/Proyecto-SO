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
    DSM_PAGE_REQUEST_MAP_BYTES = 56,
    DSM_PAGE_DATA_BASE = 80,
    DSM_NOMBRE_MAX = 64,
    DSM_MAX_FICHAS = 40,
    DSM_TRAMOS_POR_HOGAR = 16,
    /* Un cálculo reparte pocos tramos por hogar. 256 cubre los 40 puestos. */
    DSM_MAX_TRAMOS = 256,
    DSM_REGION_INFO_MAX = 60 + 16 * DSM_MAX_TRAMOS,
    DSM_CAPACIDAD_BASE_BYTES = 56,
    DSM_CAPACIDAD_MAX = DSM_CAPACIDAD_BASE_BYTES + DSM_REGION_INFO_MAX - DSM_CABECERA_BYTES
};

enum {
    DSM_TIPO_NODE_HELLO = 0x01,
    DSM_TIPO_NODE_WELCOME = 0x02,
    DSM_TIPO_NODE_BYE = 0x03,
    DSM_TIPO_NODE_DOWN_HINT = 0x04,
    DSM_TIPO_HEARTBEAT = 0x05,
    DSM_TIPO_HEARTBEAT_ACK = 0x06,
    DSM_TIPO_PAGE_REQUEST = 0x10,
    DSM_TIPO_PAGE_DATA = 0x11,
    DSM_TIPO_PAGE_DOWNGRADE = 0x12,
    DSM_TIPO_PAGE_DOWNGRADE_ACK = 0x13,
    DSM_TIPO_INVALIDATE = 0x14,
    DSM_TIPO_INVALIDATE_ACK = 0x15,
    DSM_TIPO_REGION_LOOKUP = 0x32,
    DSM_TIPO_REGION_INFO = 0x33,
    DSM_TIPO_REGION_CLOSE = 0x34,
    DSM_TIPO_REGION_CLOSE_ACK = 0x35,
    DSM_TIPO_CAP_PREPARE = 0x40,
    DSM_TIPO_CAP_PREPARE_ACK = 0x41,
    DSM_TIPO_CAP_COMMIT = 0x42,
    DSM_TIPO_CAP_COMMIT_ACK = 0x43,
    DSM_TIPO_CAP_CANCEL = 0x44,
    DSM_TIPO_CAP_CANCEL_ACK = 0x45,
    DSM_TIPO_CAP_RELEASE = 0x46,
    DSM_TIPO_CAP_RELEASE_ACK = 0x47,
    DSM_TIPO_STATS_REQUEST = 0x30,
    DSM_TIPO_STATS_REPLY = 0x31,
    DSM_TIPO_REGION_COPY_CLOSE = 0x48,
    DSM_TIPO_REGION_COPY_CLOSE_ACK = 0x49,
    DSM_TIPO_PROTO_ERROR = 0x7F,
    DSM_TIPO_ANUNCIO = 0x60
};

enum {
    DSM_STATS_NODOS = 1,
    DSM_STATS_PAGINAS = 2,
    DSM_STATS_AMBOS = 3,
    DSM_STATS_CONTADORES = 8,
    DSM_STATS_PREF = 104,
    DSM_FICHA_PAGINA = 40,
    DSM_PROTO_ERROR_BYTES = 104,
    DSM_PROTO_TEXTO = 64,
    DSM_ANUNCIO_CLAVE = 32,
    DSM_ANUNCIO_DATOS = 128,
    DSM_ANUNCIO_BASE = 68
};

enum {
    DSM_ESTADO_OK = 0,
    DSM_ESTADO_TRAMA = 1,
    DSM_ESTADO_VERSION = 2,
    DSM_ESTADO_TIPO = 3,
    DSM_ESTADO_NO_HOGAR = 4,
    DSM_ESTADO_NO_POSEEDOR = 5,
    DSM_ESTADO_PLAZO = 6,
    DSM_ESTADO_FUERA = 7,
    DSM_ESTADO_SIN_TRAMO = 8,
    DSM_ESTADO_CONFLICTO = 9,
    DSM_ESTADO_COLA = 10,
    DSM_ESTADO_CIERRE = 11,
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
    DSM_PAGE_DATA_SIN_CARGA = 0x01,
    DSM_MOTIVO_A_S = 0,
    DSM_MOTIVO_A_I = 1,
    DSM_DOWNGRADE_HECHO = 0,
    DSM_DOWNGRADE_NO_M = 1,
    DSM_INVALIDA_HECHO = 0,
    DSM_INVALIDA_YA_I = 1,
    DSM_AVISO_BYTES = 40,
    DSM_CONTROL_BYTES = 48,
    DSM_REGION_CIERRE_BYTES = 40
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
    /* Extension para mapas fragmentados/multihogar. Cero conserva la trama
     * heredada de 48 bytes; la extendida identifica la asignacion del GPN. */
    uint32_t reserva_id;
    uint32_t orden;
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

typedef struct dsm_aviso {
    uint16_t node_id;
    uint16_t razon;
} dsm_aviso_t;

typedef struct dsm_downgrade {
    uint64_t gpn;
    uint32_t id_solicitud;
    uint16_t solicitante;
    uint8_t motivo;
} dsm_downgrade_t;

typedef struct dsm_downgrade_ack {
    uint64_t gpn;
    uint32_t id_solicitud;
    uint16_t resultado;
} dsm_downgrade_ack_t;

typedef struct dsm_invalidate {
    uint64_t gpn;
    uint32_t id_solicitud;
    uint16_t escritor;
} dsm_invalidate_t;

typedef struct dsm_invalidate_ack {
    uint64_t gpn;
    uint32_t id_solicitud;
    uint16_t resultado;
} dsm_invalidate_ack_t;

typedef struct dsm_stats_request {
    uint8_t seleccion;
} dsm_stats_request_t;

typedef struct dsm_pagina_consulta {
    uint64_t gpn;
    uint64_t mascara;
    uint32_t proc;
    uint32_t reserva;
    uint32_t orden;
    uint16_t hogar;
    uint16_t poseedor;
    uint8_t msi;
    uint8_t permiso;
    uint8_t banderas;
    uint8_t copias;
} dsm_pagina_consulta_t;

typedef struct dsm_stats_reply {
    uint16_t emisor;
    uint16_t n;
    uint32_t p;
    uint64_t contadores[DSM_STATS_CONTADORES];
    dsm_ficha_t fichas[DSM_MAX_FICHAS];
    /* Al decodificar, paginas es memoria propia que el llamador libera. */
    dsm_pagina_consulta_t *paginas;
} dsm_stats_reply_t;

typedef struct dsm_proto_error {
    uint16_t codigo;
    uint32_t id_causante;
    char texto[DSM_PROTO_TEXTO];
} dsm_proto_error_t;

typedef struct dsm_region_lookup {
    uint32_t reserva_id;
    uint32_t primer_tramo;
} dsm_region_lookup_t;

int dsm_region_cierre_codificar(uint8_t *buf, size_t cap, dsm_cabecera_t *cab,
                                uint32_t reserva, size_t *out);
int dsm_region_cierre_decodificar(const uint8_t *buf, size_t n, dsm_cabecera_t *cab,
                                  uint32_t *reserva);

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

/* El token identifica la sesion del creador. Un ID no se puede recuperar ni
 * cancelar desde otra sesion. Solo PREPARE_ACK y COMMIT llevan manifiesto. */
typedef struct dsm_capacidad {
    uint32_t reserva_id;
    uint32_t orden;
    uint32_t max_paginas;
    uint32_t max_tramos;
    uint64_t token;
    dsm_region_info_t info;
} dsm_capacidad_t;

int dsm_capacidad_codificar(uint8_t *dst, size_t cap, dsm_cabecera_t *cab,
                             const dsm_capacidad_t *msg, size_t *out);
int dsm_capacidad_decodificar(const uint8_t *src, size_t n, dsm_cabecera_t *cab,
                               dsm_capacidad_t *msg);

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

int dsm_aviso_codificar(uint8_t *dst, size_t cap, dsm_cabecera_t *cab,
                       const dsm_aviso_t *msg, size_t *out);
int dsm_aviso_decodificar(const uint8_t *src, size_t n, dsm_cabecera_t *cab, dsm_aviso_t *msg);

int dsm_downgrade_codificar(uint8_t *dst, size_t cap, dsm_cabecera_t *cab,
                            const dsm_downgrade_t *msg, size_t *out);
int dsm_downgrade_decodificar(const uint8_t *src, size_t n, dsm_cabecera_t *cab,
                              dsm_downgrade_t *msg);
int dsm_downgrade_ack_codificar(uint8_t *dst, size_t cap, dsm_cabecera_t *cab,
                                const dsm_downgrade_ack_t *msg, size_t *out);
int dsm_downgrade_ack_decodificar(const uint8_t *src, size_t n, dsm_cabecera_t *cab,
                                  dsm_downgrade_ack_t *msg);

int dsm_invalidate_codificar(uint8_t *dst, size_t cap, dsm_cabecera_t *cab,
                             const dsm_invalidate_t *msg, size_t *out);
int dsm_invalidate_decodificar(const uint8_t *src, size_t n, dsm_cabecera_t *cab,
                               dsm_invalidate_t *msg);
int dsm_invalidate_ack_codificar(uint8_t *dst, size_t cap, dsm_cabecera_t *cab,
                                 const dsm_invalidate_ack_t *msg, size_t *out);
int dsm_invalidate_ack_decodificar(const uint8_t *src, size_t n, dsm_cabecera_t *cab,
                                   dsm_invalidate_ack_t *msg);

int dsm_region_lookup_codificar(uint8_t *dst, size_t cap, dsm_cabecera_t *cab,
                                const dsm_region_lookup_t *msg, size_t *out);
int dsm_region_lookup_decodificar(const uint8_t *src, size_t n,
                                  dsm_cabecera_t *cab, dsm_region_lookup_t *msg);

int dsm_region_info_codificar(uint8_t *dst, size_t cap, dsm_cabecera_t *cab,
                              const dsm_region_info_t *msg, size_t *out);
int dsm_region_info_decodificar(const uint8_t *src, size_t n,
                                dsm_cabecera_t *cab, dsm_region_info_t *msg);

int dsm_stats_request_codificar(uint8_t *dst, size_t cap, dsm_cabecera_t *cab,
                                const dsm_stats_request_t *msg, size_t *out);
int dsm_stats_request_decodificar(const uint8_t *src, size_t n, dsm_cabecera_t *cab,
                                  dsm_stats_request_t *msg);
int dsm_stats_reply_codificar(uint8_t *dst, size_t cap, dsm_cabecera_t *cab,
                              const dsm_stats_reply_t *msg, size_t *out);
/* paginas queda en memoria propia. Liberarla si el retorno es 0. */
int dsm_stats_reply_decodificar(const uint8_t *src, size_t n, dsm_cabecera_t *cab,
                                dsm_stats_reply_t *msg);
int dsm_proto_error_codificar(uint8_t *dst, size_t cap, dsm_cabecera_t *cab,
                              const dsm_proto_error_t *msg, size_t *out);
int dsm_proto_error_decodificar(const uint8_t *src, size_t n, dsm_cabecera_t *cab,
                                dsm_proto_error_t *msg);
int dsm_anuncio_codificar(uint8_t *dst, size_t cap, dsm_cabecera_t *cab, uint16_t autor,
                          const char *clave, const void *datos, size_t n, size_t *out);
int dsm_anuncio_decodificar(const uint8_t *src, size_t n, dsm_cabecera_t *cab, uint16_t *autor,
                            char clave[DSM_ANUNCIO_CLAVE], uint8_t *datos, size_t cap, size_t *largo);

#endif
