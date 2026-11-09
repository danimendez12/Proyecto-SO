#include "protocol.h"

#include <string.h>

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void put_u64(uint8_t *p, uint64_t v)
{
    p[0] = (uint8_t)(v >> 56);
    p[1] = (uint8_t)(v >> 48);
    p[2] = (uint8_t)(v >> 40);
    p[3] = (uint8_t)(v >> 32);
    p[4] = (uint8_t)(v >> 24);
    p[5] = (uint8_t)(v >> 16);
    p[6] = (uint8_t)(v >> 8);
    p[7] = (uint8_t)v;
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t get_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v = (v << 8) | p[i];
    }
    return v;
}

static void copiar_nombre(char *dst, const char *src)
{
    memset(dst, 0, DSM_NOMBRE_MAX);
    if (src != NULL) {
        memcpy(dst, src, DSM_NOMBRE_MAX);
        dst[DSM_NOMBRE_MAX - 1] = '\0';
    }
}

int dsm_cabecera_codificar(uint8_t *dst, const dsm_cabecera_t *cab)
{
    if (dst == NULL || cab == NULL) {
        return -1;
    }
    if (cab->version != DSM_PROTO_VERSION || cab->id_msg == 0 ||
        cab->longitud < DSM_CABECERA_BYTES || cab->longitud > DSM_LONGITUD_MAX) {
        return -1;
    }
    memset(dst, 0, DSM_CABECERA_BYTES);
    put_u32(dst + 0, DSM_MAGIC);
    dst[4] = cab->version;
    dst[5] = cab->tipo;
    put_u16(dst + 6, cab->banderas);
    put_u32(dst + 8, cab->longitud);
    put_u16(dst + 12, cab->origen);
    put_u16(dst + 14, cab->destino);
    put_u32(dst + 16, cab->id_msg);
    put_u32(dst + 20, cab->id_corr);
    put_u16(dst + 24, cab->estado);
    return 0;
}

int dsm_cabecera_decodificar(const uint8_t *src, size_t n, dsm_cabecera_t *cab)
{
    if (src == NULL || cab == NULL || n < DSM_CABECERA_BYTES) {
        return -1;
    }
    if (get_u32(src) != DSM_MAGIC || src[4] != DSM_PROTO_VERSION) {
        return -1;
    }
    uint32_t longitud = get_u32(src + 8);
    if (longitud < DSM_CABECERA_BYTES || longitud > DSM_LONGITUD_MAX) {
        return -1;
    }
    if (n > DSM_CABECERA_BYTES && n != longitud) {
        return -1;
    }
    memset(cab, 0, sizeof *cab);
    cab->version = src[4];
    cab->tipo = src[5];
    cab->banderas = get_u16(src + 6);
    cab->longitud = longitud;
    cab->origen = get_u16(src + 12);
    cab->destino = get_u16(src + 14);
    cab->id_msg = get_u32(src + 16);
    cab->id_corr = get_u32(src + 20);
    cab->estado = get_u16(src + 24);
    if (cab->id_msg == 0) {
        return -1;
    }
    return 0;
}

static int preparar(uint8_t *dst, size_t cap, dsm_cabecera_t *cab, uint32_t longitud)
{
    if (dst == NULL || cab == NULL || longitud > cap) {
        return -1;
    }
    cab->longitud = longitud;
    if (dsm_cabecera_codificar(dst, cab) != 0) {
        return -1;
    }
    if (longitud > DSM_CABECERA_BYTES) {
        memset(dst + DSM_CABECERA_BYTES, 0, longitud - DSM_CABECERA_BYTES);
    }
    return 0;
}

int dsm_hello_codificar(uint8_t *dst, size_t cap, dsm_cabecera_t *cab,
                        const dsm_hello_t *msg, size_t *out)
{
    if (msg == NULL || out == NULL) {
        return -1;
    }
    if (preparar(dst, cap, cab, 128) != 0) {
        return -1;
    }
    put_u16(dst + 32, msg->node_id);
    put_u16(dst + 34, msg->conocidos);
    put_u32(dst + 36, msg->ipv4_host);
    put_u16(dst + 40, msg->puerto);
    put_u64(dst + 48, msg->memoria);
    put_u64(dst + 56, msg->base);
    memcpy(dst + 64, msg->nombre, DSM_NOMBRE_MAX);
    *out = 128;
    return 0;
}

int dsm_hello_decodificar(const uint8_t *src, size_t n, dsm_cabecera_t *cab, dsm_hello_t *msg)
{
    if (msg == NULL || dsm_cabecera_decodificar(src, n, cab) != 0) {
        return -1;
    }
    if (cab->tipo != DSM_TIPO_NODE_HELLO || cab->longitud != 128 || n != 128) {
        return -1;
    }
    memset(msg, 0, sizeof *msg);
    msg->node_id = get_u16(src + 32);
    msg->conocidos = get_u16(src + 34);
    msg->ipv4_host = get_u32(src + 36);
    msg->puerto = get_u16(src + 40);
    msg->memoria = get_u64(src + 48);
    msg->base = get_u64(src + 56);
    copiar_nombre(msg->nombre, (const char *)(src + 64));
    return 0;
}

static void escribir_ficha(uint8_t *p, const dsm_ficha_t *ficha)
{
    memset(p, 0, 104);
    put_u16(p + 0, ficha->id);
    p[2] = ficha->estado;
    put_u32(p + 4, ficha->ipv4_host);
    put_u16(p + 8, ficha->puerto);
    put_u64(p + 12, ficha->memoria);
    put_u64(p + 20, ficha->base);
    put_u32(p + 28, ficha->libres);
    put_u32(p + 32, ficha->totales);
    memcpy(p + 36, ficha->nombre, DSM_NOMBRE_MAX);
}

static void leer_ficha(const uint8_t *p, dsm_ficha_t *ficha)
{
    memset(ficha, 0, sizeof *ficha);
    ficha->id = get_u16(p + 0);
    ficha->estado = p[2];
    ficha->ipv4_host = get_u32(p + 4);
    ficha->puerto = get_u16(p + 8);
    ficha->memoria = get_u64(p + 12);
    ficha->base = get_u64(p + 20);
    ficha->libres = get_u32(p + 28);
    ficha->totales = get_u32(p + 32);
    copiar_nombre(ficha->nombre, (const char *)(p + 36));
}

int dsm_welcome_codificar(uint8_t *dst, size_t cap, dsm_cabecera_t *cab,
                          const dsm_welcome_t *msg, size_t *out)
{
    if (msg == NULL || out == NULL || msg->n > DSM_MAX_FICHAS) {
        return -1;
    }
    uint32_t longitud = 40u + 104u * msg->n;
    if (preparar(dst, cap, cab, longitud) != 0) {
        return -1;
    }
    put_u16(dst + 32, msg->emisor);
    put_u16(dst + 34, msg->n);
    put_u32(dst + 36, msg->epoca);
    for (uint16_t i = 0; i < msg->n; i++) {
        escribir_ficha(dst + 40 + (size_t)i * 104u, &msg->fichas[i]);
    }
    *out = longitud;
    return 0;
}

int dsm_welcome_decodificar(const uint8_t *src, size_t n, dsm_cabecera_t *cab, dsm_welcome_t *msg)
{
    if (msg == NULL || dsm_cabecera_decodificar(src, n, cab) != 0) {
        return -1;
    }
    if (cab->tipo != DSM_TIPO_NODE_WELCOME || n != cab->longitud) {
        return -1;
    }
    if (cab->estado != DSM_ESTADO_OK) {
        if (cab->longitud != DSM_CABECERA_BYTES) {
            return -1;
        }
        memset(msg, 0, sizeof *msg);
        return 0;
    }
    if (cab->longitud < 40 || (cab->longitud - 40u) % 104u != 0) {
        return -1;
    }
    uint16_t cantidad = (uint16_t)((cab->longitud - 40u) / 104u);
    if (cantidad > DSM_MAX_FICHAS || get_u16(src + 34) != cantidad) {
        return -1;
    }
    memset(msg, 0, sizeof *msg);
    msg->emisor = get_u16(src + 32);
    msg->n = cantidad;
    msg->epoca = get_u32(src + 36);
    for (uint16_t i = 0; i < cantidad; i++) {
        leer_ficha(src + 40 + (size_t)i * 104u, &msg->fichas[i]);
    }
    return 0;
}

int dsm_heartbeat_codificar(uint8_t *dst, size_t cap, dsm_cabecera_t *cab,
                            const dsm_heartbeat_t *msg, size_t *out)
{
    if (msg == NULL || out == NULL) {
        return -1;
    }
    if (preparar(dst, cap, cab, 48) != 0) {
        return -1;
    }
    put_u64(dst + 32, msg->marca);
    put_u64(dst + 40, msg->instante_ms);
    *out = 48;
    return 0;
}

int dsm_heartbeat_decodificar(const uint8_t *src, size_t n, dsm_cabecera_t *cab,
                              dsm_heartbeat_t *msg)
{
    if (msg == NULL || dsm_cabecera_decodificar(src, n, cab) != 0) {
        return -1;
    }
    if ((cab->tipo != DSM_TIPO_HEARTBEAT && cab->tipo != DSM_TIPO_HEARTBEAT_ACK) ||
        cab->longitud != 48 || n != 48) {
        return -1;
    }
    msg->marca = get_u64(src + 32);
    msg->instante_ms = get_u64(src + 40);
    return 0;
}

int dsm_region_lookup_codificar(uint8_t *dst, size_t cap, dsm_cabecera_t *cab,
                                const dsm_region_lookup_t *msg, size_t *out)
{
    if (msg == NULL || out == NULL) {
        return -1;
    }
    if (preparar(dst, cap, cab, 40) != 0) {
        return -1;
    }
    put_u32(dst + 32, msg->reserva_id);
    put_u32(dst + 36, msg->primer_tramo);
    *out = 40;
    return 0;
}

int dsm_region_lookup_decodificar(const uint8_t *src, size_t n, dsm_cabecera_t *cab,
                                  dsm_region_lookup_t *msg)
{
    if (msg == NULL || dsm_cabecera_decodificar(src, n, cab) != 0) {
        return -1;
    }
    if (cab->tipo != DSM_TIPO_REGION_LOOKUP || cab->longitud != 40 || n != 40) {
        return -1;
    }
    msg->reserva_id = get_u32(src + 32);
    msg->primer_tramo = get_u32(src + 36);
    return 0;
}

int dsm_region_info_codificar(uint8_t *dst, size_t cap, dsm_cabecera_t *cab,
                              const dsm_region_info_t *msg, size_t *out)
{
    if (msg == NULL || out == NULL || msg->cantidad_tramos > DSM_MAX_TRAMOS) {
        return -1;
    }
    uint32_t longitud = 60u + 16u * msg->cantidad_tramos;
    if (preparar(dst, cap, cab, longitud) != 0) {
        return -1;
    }
    put_u32(dst + 32, msg->reserva_id);
    put_u32(dst + 36, msg->total_tramos);
    put_u64(dst + 40, msg->tamano_bytes);
    put_u32(dst + 48, msg->primer_tramo);
    put_u32(dst + 52, msg->cantidad_tramos);
    put_u32(dst + 56, msg->total_paginas);
    for (uint32_t i = 0; i < msg->cantidad_tramos; i++) {
        uint8_t *p = dst + 60 + (size_t)i * 16u;
        put_u32(p + 0, msg->tramos[i].orden_inicial);
        put_u32(p + 4, msg->tramos[i].cantidad);
        put_u64(p + 8, msg->tramos[i].gpn_inicial);
    }
    *out = longitud;
    return 0;
}

int dsm_region_info_decodificar(const uint8_t *src, size_t n, dsm_cabecera_t *cab,
                                dsm_region_info_t *msg)
{
    if (msg == NULL || dsm_cabecera_decodificar(src, n, cab) != 0) {
        return -1;
    }
    if (cab->tipo != DSM_TIPO_REGION_INFO || n != cab->longitud) {
        return -1;
    }
    if (cab->estado != DSM_ESTADO_OK) {
        if (cab->longitud != DSM_CABECERA_BYTES) {
            return -1;
        }
        memset(msg, 0, sizeof *msg);
        return 0;
    }
    if (cab->longitud < 60 || (cab->longitud - 60u) % 16u != 0) {
        return -1;
    }
    uint32_t cantidad = (cab->longitud - 60u) / 16u;
    if (cantidad > DSM_MAX_TRAMOS || get_u32(src + 52) != cantidad) {
        return -1;
    }
    memset(msg, 0, sizeof *msg);
    msg->reserva_id = get_u32(src + 32);
    msg->total_tramos = get_u32(src + 36);
    msg->tamano_bytes = get_u64(src + 40);
    msg->primer_tramo = get_u32(src + 48);
    msg->cantidad_tramos = cantidad;
    msg->total_paginas = get_u32(src + 56);
    for (uint32_t i = 0; i < cantidad; i++) {
        const uint8_t *p = src + 60 + (size_t)i * 16u;
        msg->tramos[i].orden_inicial = get_u32(p + 0);
        msg->tramos[i].cantidad = get_u32(p + 4);
        msg->tramos[i].gpn_inicial = get_u64(p + 8);
    }
    return 0;
}
