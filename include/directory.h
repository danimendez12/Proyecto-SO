#ifndef DSM_DIRECTORY_H
#define DSM_DIRECTORY_H

/* Tabla basica de paginas y el manifiesto de una reserva. */

#include "config.h"
#include "memory.h"
#include "protocol.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

enum {
    DSM_POSEEDOR_NINGUNO = 0xFFFF,
    DSM_DIR_COLA = 64
};

typedef int (*dsm_directorio_envio_t)(void *ctx, uint16_t destino, const uint8_t *trama, size_t n);
typedef uint32_t (*dsm_directorio_id_t)(void *ctx);

struct dsm_item {
    uint16_t par;
    uint16_t n;
    uint8_t trama[64];
};

struct dsm_cola {
    struct dsm_directorio *directorio;
    struct dsm_item items[DSM_DIR_COLA];
    size_t inicio;
    size_t cantidad;
    pthread_t hilo;
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
    uint8_t perdida;
    uint32_t proc;
} dsm_pagina_desc_t;

enum {
    DSM_CONTA_LECTURA = 0,
    DSM_CONTA_ESCRITURA,
    DSM_CONTA_INV_ENV,
    DSM_CONTA_INV_REC,
    DSM_CONTA_PAG_ENV,
    DSM_CONTA_PAG_REC,
    DSM_CONTA_BYTES_ENV,
    DSM_CONTA_BYTES_REC,
    DSM_CONTA_N
};

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
    uint32_t siguiente_local;
    struct dsm_reserva_dir *reservas;
    struct dsm_tumba *tumbas;
    dsm_pagina_desc_t *paginas;
    size_t npaginas;
    size_t capacidad_paginas;
    unsigned char *ocupadas;
    size_t nvistas;
    int cerrando;
    pthread_mutex_t mutex;
    /* Serializa a los iniciadores de campanas. Los receptores y las replicas
     * completan las respuestas usando solo mutex. */
    pthread_mutex_t coordinador;
    int inicializado;
    /* Servicio de coherencia. El hilo receptor solo encola. */
    int servicio_activo;
    int servicio_parar;
    struct dsm_cola hogar;
    struct dsm_cola replica;
    pthread_cond_t cola_cv;
    pthread_cond_t campana_cv;
    dsm_directorio_envio_t enviar;
    dsm_directorio_id_t nuevo_id;
    void *envio_ctx;
    uint32_t retardo_ms;
    uint64_t fuera;
    int campana_espera;
    uint16_t campana_par;
    uint32_t campana_id;
    int campana_error;
    int tiene_datos;
    int tiene_ack;
    size_t datos_n;
    uint8_t ack_trama[DSM_CONTROL_BYTES];
    uint8_t *campana_buf;
    size_t campana_cap;
    dsm_nodo_cfg_t local;
    _Atomic uint64_t contadores[DSM_CONTA_N];
} dsm_directorio_t;

/* Un directorio inicializado contiene un mutex y no se copia. */
int dsm_directorio_init(dsm_directorio_t *dir, const dsm_config_t *cfg,
                        uint16_t nodo_id);
/* Cerrar tras terminar las operaciones y el uso de los punteros prestados. */
void dsm_directorio_cerrar(dsm_directorio_t *dir);
int dsm_directorio_id_reserva(dsm_directorio_t *dir, uint32_t *id);
int dsm_directorio_preparar(dsm_directorio_t *dir, uint32_t id, uint64_t token,
                            uint32_t orden, uint32_t max_paginas, uint32_t max_tramos,
                            dsm_region_info_t *parcial);
int dsm_directorio_confirmar(dsm_directorio_t *dir, uint32_t id, uint64_t token,
                              const dsm_region_info_t *info);
int dsm_directorio_cancelar(dsm_directorio_t *dir, uint32_t id, uint64_t token);
int dsm_directorio_soltar(dsm_directorio_t *dir, uint32_t id, uint64_t token);
int dsm_directorio_iniciar_retiro(dsm_directorio_t *dir, uint32_t id);
int dsm_directorio_reservar(dsm_directorio_t *dir, size_t bytes,
                             dsm_pagina_desc_t *salida, dsm_vistas_t **vista);
int dsm_directorio_publicar_reserva(dsm_directorio_t *dir, uint32_t id);
int dsm_directorio_info(dsm_directorio_t *dir, uint32_t id, dsm_region_info_t *info);
int dsm_directorio_pagina_reserva(dsm_directorio_t *dir, uint32_t id, size_t orden,
                                  dsm_pagina_desc_t *pagina);
int dsm_directorio_liberar(dsm_directorio_t *dir, uint32_t id);
/* Desmontar despues del ACK de REGION_CLOSE y sin trabajadores que usen la vista. */
int dsm_directorio_desvincular(dsm_directorio_t *dir, uint32_t id);
int dsm_directorio_preparar_cierre(dsm_directorio_t *dir);
/* Solo CLI: cerrar admision y esperar CANCEL/RELEASE con el servicio aun activo. */
int dsm_directorio_esperar_fin_aportes(dsm_directorio_t *dir);
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
                                 void *imagen, size_t imagen_cap, int *cambio);
/* Instala los bytes por C_r. El paginador habilita luego la lectura de A_r. */
int dsm_directorio_compartir_pagina(dsm_directorio_t *dir, uint32_t reserva_id,
                                     const dsm_page_data_t *datos);

/* Copia una imagen S o M. Sin carga solo concede M si la copia S local sigue vigente. */
int dsm_directorio_aplicar_pagina(dsm_directorio_t *dir, const dsm_page_data_t *datos);
int dsm_directorio_describir(dsm_directorio_t *dir, uint64_t gpn, dsm_pagina_desc_t *desc);
/* scratch pertenece al gestor. Copia por Cr bajo mutex tras revalidar la copia
 * local. EAGAIN solicita preparar permisos mediante el servicio de coherencia.
 */
int dsm_directorio_copiar_controlado(dsm_directorio_t *dir, uint32_t reserva, uint64_t gpn,
                                     size_t offset, void *scratch, size_t bytes, int escritura);
/* Captura y escritura atomicas respecto del directorio. Los buffers son del
 * gestor; restaurar no pide permisos ni cambia version/estado MSI. Ar local
 * solapada requiere sincronizacion de la aplicacion tambien durante la copia. */
int dsm_directorio_escribir_respaldo(dsm_directorio_t *dir, uint32_t reserva, uint64_t gpn,
                                     size_t offset, const void *nuevo, void *previo,
                                     size_t bytes, uint32_t *version);
int dsm_directorio_restaurar_respaldo(dsm_directorio_t *dir, uint32_t reserva, uint64_t gpn,
                                      size_t offset, const void *previo, const void *escrito,
                                      size_t bytes, uint32_t version);
/* Copia la version vigente. Si el poseedor es remoto, pide la bajada por la red activa. */
int dsm_directorio_copiar(dsm_directorio_t *dir, uint64_t gpn, void *imagen, size_t n,
                          dsm_pagina_desc_t *desc);

/* El callback de envio no se invoca con el mutex del directorio tomado. */
int dsm_directorio_activar(dsm_directorio_t *dir, dsm_directorio_envio_t enviar,
                           dsm_directorio_id_t nuevo_id, void *ctx);
void dsm_directorio_desactivar(dsm_directorio_t *dir);
/* Encola y regresa. Una cola llena responde estado de cola sin esperar. */
int dsm_directorio_ofrecer(dsm_directorio_t *dir, uint16_t par, const uint8_t *trama, size_t n);
/* 0 si la trama cierra una campana de este directorio. */
int dsm_directorio_completar(dsm_directorio_t *dir, uint16_t par, const uint8_t *trama, size_t n);
void dsm_directorio_par_fuera(dsm_directorio_t *dir, uint16_t par);
void dsm_directorio_par_linea(dsm_directorio_t *dir, uint16_t par);
/* Retardo del poseedor antes de reconocer una invalidacion. Cero en produccion. */
void dsm_directorio_fijar_retardo(dsm_directorio_t *dir, uint32_t ms);
void dsm_directorio_contar(dsm_directorio_t *dir, int indice, uint64_t n);
/* Copia las paginas conocidas y los ocho contadores. paginas puede ser NULL. */
int dsm_directorio_exportar(dsm_directorio_t *dir, uint64_t contadores[DSM_CONTA_N],
                            dsm_pagina_desc_t *paginas, size_t capacidad, size_t *cantidad,
                            uint32_t *libres, uint32_t *totales);

#endif
