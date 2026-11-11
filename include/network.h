#ifndef DSM_NETWORK_H
#define DSM_NETWORK_H

/* TCP de un puerto por nodo: send_all, recv_all, alta y latido. */

#include "config.h"
#include "protocol.h"

#include <stddef.h>
#include <stdint.h>

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

#endif
