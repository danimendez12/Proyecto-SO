#ifndef DSM_NETWORK_H
#define DSM_NETWORK_H

/* TCP de un puerto por nodo: send_all, recv_all, alta y latido. */

#include "config.h"
#include "protocol.h"

#include <stddef.h>
#include <stdint.h>
#include <time.h>

int send_all(int fd, const void *buf, size_t len);
int recv_all(int fd, void *buf, size_t len);
int dsm_recibir_trama(int fd, uint8_t *buf, size_t cap, size_t *out);

/* Plazo para toda la trama. Cero indica espera ilimitada. */
int dsm_enviar_trama(int fd, const void *buf, size_t len, uint32_t timeout_ms);
int dsm_recibir_trama_plazo(int fd, uint8_t *buf, size_t cap, size_t *out, uint32_t timeout_ms);

int dsm_escuchar(uint32_t ipv4_host, uint16_t puerto, int *escucha);
int dsm_aceptar(int escucha, int *conn);
int dsm_conectar(uint32_t ipv4_host, uint16_t puerto, int *conn);
int dsm_conectar_plazo(uint32_t ipv4_host, uint16_t puerto, uint32_t timeout_ms, int *conn);

typedef struct dsm_par {
    uint16_t id;
    uint8_t estado;
    uint16_t puerto;
    uint32_t ipv4_host;
    uint64_t memoria;
    uint64_t base;
    uint64_t sesion;
    char nombre[DSM_NOMBRE_MAX];
} dsm_par_t;

typedef struct dsm_alta_resultado {
    int hello;
    int welcome;
    int heartbeat_ack;
    uint8_t estado_tras_hello;
    uint32_t siguiente_msg;
    dsm_par_t par;
} dsm_alta_resultado_t;

int dsm_alta_par(int fd, const dsm_config_t *cfg, uint16_t self_id,
                 uint16_t peer_id, dsm_alta_resultado_t *out);

/* El servidor identifica al par configurado a partir de NODE_HELLO. */
int dsm_alta_aceptada(int fd, const dsm_config_t *cfg, uint16_t self_id, dsm_alta_resultado_t *out);

typedef struct dsm_red dsm_red_t;
typedef int (*dsm_red_trama_t)(void *ctx, uint16_t peer, const dsm_cabecera_t *cab,
                               const uint8_t *buf, size_t n);

/* cfg y ctx viven hasta el cierre. El callback no espera respuestas RPC y
 * debe copiar la trama si necesita conservarla despues de devolver el control.
 */
int dsm_red_iniciar(dsm_red_t **out, const dsm_config_t *cfg, uint16_t self,
                    dsm_red_trama_t atender, void *ctx);
/* Sesion no nula para avisos. La interfaz anterior conserva alta heredada. */
int dsm_red_iniciar_sesion(dsm_red_t **out, const dsm_config_t *cfg, uint16_t self,
                           dsm_red_trama_t atender, void *ctx, uint64_t sesion);
int dsm_red_sesion_par(dsm_red_t *red, uint16_t par, uint64_t *sesion);
/* Epoca local del fd: el envio comprueba y reserva ese mismo enlace bajo el
 * mutex de red, para que un snapshot no cruce a una conexion reemplazada. */
int dsm_red_enlace_par(dsm_red_t *red, uint16_t par, uint64_t *sesion, uint64_t *epoca);
int dsm_red_enviar_enlace(dsm_red_t *red, uint16_t par, uint64_t epoca,
                           const uint8_t *buf, size_t n);
int dsm_red_sondear(dsm_red_t *red, uint32_t plazo_ms);
/* Cierra tras terminar las operaciones externas y espera a los lectores. */
void dsm_red_cerrar(dsm_red_t *red);
int dsm_red_enviar(dsm_red_t *red, uint16_t peer, const uint8_t *buf, size_t n);
/* Copia los pares con conexión activa, sin incluir al propio nodo. */
int dsm_red_destinos(dsm_red_t *red, uint16_t *ids, uint16_t cap, uint16_t *cantidad);
uint32_t dsm_red_id(dsm_red_t *red);
int dsm_red_esperar(dsm_red_t *red, uint16_t peer);
/* Espera el alta usando el vencimiento CLOCK_MONOTONIC de la operación. */
int dsm_red_esperar_hasta(dsm_red_t *red, uint16_t peer, const struct timespec *plazo);

typedef void (*dsm_red_estado_t)(void *ctx, uint16_t par, uint8_t estado);
void dsm_red_observar(dsm_red_t *red, dsm_red_estado_t fn, void *ctx);
int dsm_red_estado(dsm_red_t *red, uint16_t par, uint8_t *estado);

#endif
