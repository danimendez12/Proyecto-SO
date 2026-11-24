#include "protocol.h"

#include <stdlib.h>
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

static int cabecera_pedido(const dsm_cabecera_t *cab, uint8_t tipo)
{
    return cab != NULL && cab->tipo == tipo && cab->banderas == 0 && cab->id_corr == 0 &&
           cab->estado == DSM_ESTADO_OK;
}

static int cabecera_respuesta(const dsm_cabecera_t *cab, uint8_t tipo)
{
    return cab != NULL && cab->tipo == tipo && cab->banderas == DSM_FLAG_RESPUESTA &&
           cab->id_corr != 0;
}

int dsm_aviso_codificar(uint8_t *dst, size_t cap, dsm_cabecera_t *cab, const dsm_aviso_t *msg,
                        size_t *out)
{
    if (msg == NULL || out == NULL || cab == NULL ||
        (cab->tipo != DSM_TIPO_NODE_BYE && cab->tipo != DSM_TIPO_NODE_DOWN_HINT) ||
        cab->banderas != 0 || cab->id_corr != 0) {
        return -1;
    }
    if (preparar(dst, cap, cab, DSM_AVISO_BYTES) != 0) {
        return -1;
    }
    put_u16(dst + 32, msg->node_id);
    put_u16(dst + 34, msg->razon);
    *out = DSM_AVISO_BYTES;
    return 0;
}

int dsm_aviso_decodificar(const uint8_t *src, size_t n, dsm_cabecera_t *cab, dsm_aviso_t *msg)
{
    if (msg == NULL || n != DSM_AVISO_BYTES || dsm_cabecera_decodificar(src, n, cab) != 0) {
        return -1;
    }
    if ((cab->tipo != DSM_TIPO_NODE_BYE && cab->tipo != DSM_TIPO_NODE_DOWN_HINT) ||
        cab->banderas != 0 || cab->id_corr != 0 || cab->longitud != DSM_AVISO_BYTES) {
        return -1;
    }
    msg->node_id = get_u16(src + 32);
    msg->razon = get_u16(src + 34);
    return 0;
}

int dsm_downgrade_codificar(uint8_t *dst, size_t cap, dsm_cabecera_t *cab,
                            const dsm_downgrade_t *msg, size_t *out)
{
    if (!cabecera_pedido(cab, DSM_TIPO_PAGE_DOWNGRADE) || msg == NULL ||
        out == NULL || msg->id_solicitud == 0 || msg->motivo > DSM_MOTIVO_A_I ||
        preparar(dst, cap, cab, DSM_CONTROL_BYTES) != 0) {
        return -1;
    }
    put_u64(dst + 32, msg->gpn);
    put_u32(dst + 40, msg->id_solicitud);
    put_u16(dst + 44, msg->solicitante);
    dst[46] = msg->motivo;
    *out = DSM_CONTROL_BYTES;
    return 0;
}

int dsm_downgrade_decodificar(const uint8_t *src, size_t n, dsm_cabecera_t *cab,
                              dsm_downgrade_t *msg)
{
    if (msg == NULL || n != DSM_CONTROL_BYTES || dsm_cabecera_decodificar(src, n, cab) != 0 ||
        !cabecera_pedido(cab, DSM_TIPO_PAGE_DOWNGRADE) || cab->longitud != DSM_CONTROL_BYTES ||
        get_u32(src + 40) == 0 || src[46] > DSM_MOTIVO_A_I) {
        return -1;
    }
    *msg = (dsm_downgrade_t){
        .gpn = get_u64(src + 32),
        .id_solicitud = get_u32(src + 40),
        .solicitante = get_u16(src + 44),
        .motivo = src[46],
    };
    return 0;
}

int dsm_downgrade_ack_codificar(uint8_t *dst, size_t cap, dsm_cabecera_t *cab,
                                const dsm_downgrade_ack_t *msg, size_t *out)
{
    if (!cabecera_respuesta(cab, DSM_TIPO_PAGE_DOWNGRADE_ACK) || msg == NULL ||
        out == NULL || msg->id_solicitud == 0 || msg->resultado > DSM_DOWNGRADE_NO_M ||
        preparar(dst, cap, cab, DSM_CONTROL_BYTES) != 0) {
        return -1;
    }
    put_u64(dst + 32, msg->gpn);
    put_u32(dst + 40, msg->id_solicitud);
    put_u16(dst + 44, msg->resultado);
    *out = DSM_CONTROL_BYTES;
    return 0;
}

int dsm_downgrade_ack_decodificar(const uint8_t *src, size_t n, dsm_cabecera_t *cab,
                                  dsm_downgrade_ack_t *msg)
{
    if (msg == NULL || n != DSM_CONTROL_BYTES || dsm_cabecera_decodificar(src, n, cab) != 0 ||
        !cabecera_respuesta(cab, DSM_TIPO_PAGE_DOWNGRADE_ACK) || cab->longitud != DSM_CONTROL_BYTES ||
        get_u32(src + 40) == 0 || get_u16(src + 44) > DSM_DOWNGRADE_NO_M) {
        return -1;
    }
    *msg = (dsm_downgrade_ack_t){
        .gpn = get_u64(src + 32),
        .id_solicitud = get_u32(src + 40),
        .resultado = get_u16(src + 44),
    };
    return 0;
}

int dsm_invalidate_codificar(uint8_t *dst, size_t cap, dsm_cabecera_t *cab,
                             const dsm_invalidate_t *msg, size_t *out)
{
    if (!cabecera_pedido(cab, DSM_TIPO_INVALIDATE) || msg == NULL ||
        out == NULL || msg->id_solicitud == 0 || preparar(dst, cap, cab, DSM_CONTROL_BYTES) != 0) {
        return -1;
    }
    put_u64(dst + 32, msg->gpn);
    put_u32(dst + 40, msg->id_solicitud);
    put_u16(dst + 44, msg->escritor);
    *out = DSM_CONTROL_BYTES;
    return 0;
}

int dsm_invalidate_decodificar(const uint8_t *src, size_t n, dsm_cabecera_t *cab,
                               dsm_invalidate_t *msg)
{
    if (msg == NULL || n != DSM_CONTROL_BYTES || dsm_cabecera_decodificar(src, n, cab) != 0 ||
        !cabecera_pedido(cab, DSM_TIPO_INVALIDATE) || cab->longitud != DSM_CONTROL_BYTES ||
        get_u32(src + 40) == 0) {
        return -1;
    }
    *msg = (dsm_invalidate_t){
        .gpn = get_u64(src + 32),
        .id_solicitud = get_u32(src + 40),
        .escritor = get_u16(src + 44),
    };
    return 0;
}

int dsm_invalidate_ack_codificar(uint8_t *dst, size_t cap, dsm_cabecera_t *cab,
                                 const dsm_invalidate_ack_t *msg, size_t *out)
{
    if (!cabecera_respuesta(cab, DSM_TIPO_INVALIDATE_ACK) || msg == NULL ||
        out == NULL || msg->id_solicitud == 0 || msg->resultado > DSM_INVALIDA_YA_I ||
        preparar(dst, cap, cab, DSM_CONTROL_BYTES) != 0) {
        return -1;
    }
    put_u64(dst + 32, msg->gpn);
    put_u32(dst + 40, msg->id_solicitud);
    put_u16(dst + 44, msg->resultado);
    *out = DSM_CONTROL_BYTES;
    return 0;
}

int dsm_invalidate_ack_decodificar(const uint8_t *src, size_t n, dsm_cabecera_t *cab,
                                   dsm_invalidate_ack_t *msg)
{
    if (msg == NULL || n != DSM_CONTROL_BYTES || dsm_cabecera_decodificar(src, n, cab) != 0 ||
        !cabecera_respuesta(cab, DSM_TIPO_INVALIDATE_ACK) || cab->longitud != DSM_CONTROL_BYTES ||
        get_u32(src + 40) == 0 || get_u16(src + 44) > DSM_INVALIDA_YA_I) {
        return -1;
    }
    *msg = (dsm_invalidate_ack_t){
        .gpn = get_u64(src + 32),
        .id_solicitud = get_u32(src + 40),
        .resultado = get_u16(src + 44),
    };
    return 0;
}

static int cabecera_page_request_valida(const dsm_cabecera_t *cab)
{
    return cab != NULL && cab->tipo == DSM_TIPO_PAGE_REQUEST && cab->banderas == 0 &&
           cab->id_corr == 0 && cab->estado == DSM_ESTADO_OK;
}

int dsm_page_request_codificar(uint8_t *dst, size_t cap, dsm_cabecera_t *cab,
                                const dsm_page_request_t *msg, size_t *out)
{
    if (!cabecera_page_request_valida(cab) || msg == NULL || out == NULL ||
        msg->acceso > DSM_ACCESO_ESCRITURA || msg->msi > DSM_MSI_M ||
        (msg->reserva_id != 0 && (uint16_t)msg->reserva_id == 0)) {
        return -1;
    }
    uint32_t longitud = msg->reserva_id == 0 ? DSM_PAGE_REQUEST_BYTES : DSM_PAGE_REQUEST_MAP_BYTES;
    if (preparar(dst, cap, cab, longitud) != 0) {
        return -1;
    }
    put_u64(dst + 32, msg->gpn);
    put_u32(dst + 40, msg->proc_id);
    dst[44] = msg->acceso;
    dst[45] = msg->msi;
    if (msg->reserva_id != 0) {
        put_u32(dst + 48, msg->reserva_id);
        put_u32(dst + 52, msg->orden);
    }
    *out = longitud;
    return 0;
}

int dsm_page_request_decodificar(const uint8_t *src, size_t n, dsm_cabecera_t *cab,
                                  dsm_page_request_t *msg)
{
    if (msg == NULL || (n != DSM_PAGE_REQUEST_BYTES && n != DSM_PAGE_REQUEST_MAP_BYTES) ||
        dsm_cabecera_decodificar(src, n, cab) != 0 || !cabecera_page_request_valida(cab) ||
        src[44] > DSM_ACCESO_ESCRITURA || src[45] > DSM_MSI_M) {
        return -1;
    }
    *msg = (dsm_page_request_t){
        .gpn = get_u64(src + 32),
        .proc_id = get_u32(src + 40),
        .acceso = src[44],
        .msi = src[45],
    };
    if (n == DSM_PAGE_REQUEST_MAP_BYTES) {
        msg->reserva_id = get_u32(src + 48);
        msg->orden = get_u32(src + 52);
        if ((uint16_t)msg->reserva_id == 0) {
            return -1;
        }
    }
    return 0;
}

static int cabecera_page_data_valida(const dsm_cabecera_t *cab)
{
    return cab != NULL && cab->tipo == DSM_TIPO_PAGE_DATA &&
           cab->banderas == DSM_FLAG_RESPUESTA && cab->id_corr != 0;
}

static int page_data_valida(const dsm_page_data_t *msg)
{
    if (msg == NULL || msg->page_nbytes > DSM_LONGITUD_MAX - DSM_PAGE_DATA_BASE ||
        (msg->msi != DSM_MSI_S && msg->msi != DSM_MSI_M) ||
        msg->permiso != (msg->msi == DSM_MSI_S ? DSM_PAGE_PERMISO_LEER : DSM_PAGE_PERMISO_ESCRIBIR)) {
        return 0;
    }
    if (msg->page_nbytes == 0) {
        return msg->banderas == DSM_PAGE_DATA_SIN_CARGA;
    }
    return msg->banderas == 0 && msg->datos != NULL;
}

int dsm_page_data_codificar(uint8_t *dst, size_t cap, dsm_cabecera_t *cab,
                             const dsm_page_data_t *msg, size_t *out)
{
    if (!cabecera_page_data_valida(cab) || out == NULL) {
        return -1;
    }
    if (cab->estado != DSM_ESTADO_OK) {
        if (preparar(dst, cap, cab, DSM_CABECERA_BYTES) != 0) {
            return -1;
        }
        *out = DSM_CABECERA_BYTES;
        return 0;
    }
    if (!page_data_valida(msg) ||
        preparar(dst, cap, cab, DSM_PAGE_DATA_BASE + msg->page_nbytes) != 0) {
        return -1;
    }
    put_u64(dst + 32, msg->gpn);
    put_u32(dst + 40, msg->proc_id);
    put_u32(dst + 44, msg->reserva_id);
    put_u32(dst + 48, msg->orden);
    put_u16(dst + 52, msg->hogar);
    put_u16(dst + 54, msg->poseedor);
    put_u16(dst + 56, msg->msi);
    dst[58] = msg->permiso;
    dst[59] = msg->banderas;
    put_u64(dst + 64, msg->mascara_copias);
    put_u32(dst + 72, msg->page_nbytes);
    put_u32(dst + 76, msg->version);
    if (msg->page_nbytes != 0) {
        memcpy(dst + DSM_PAGE_DATA_BASE, msg->datos, msg->page_nbytes);
    }
    *out = cab->longitud;
    return 0;
}

int dsm_page_data_decodificar(const uint8_t *src, size_t n, dsm_cabecera_t *cab,
                               dsm_page_data_t *msg)
{
    if (msg == NULL || dsm_cabecera_decodificar(src, n, cab) != 0 ||
        !cabecera_page_data_valida(cab) || n != cab->longitud) {
        return -1;
    }
    if (cab->estado != DSM_ESTADO_OK) {
        if (n != DSM_CABECERA_BYTES) {
            return -1;
        }
        memset(msg, 0, sizeof *msg);
        return 0;
    }
    if (n < DSM_PAGE_DATA_BASE || get_u32(src + 72) != n - DSM_PAGE_DATA_BASE) {
        return -1;
    }
    dsm_page_data_t datos = {
        .gpn = get_u64(src + 32),
        .proc_id = get_u32(src + 40),
        .reserva_id = get_u32(src + 44),
        .orden = get_u32(src + 48),
        .hogar = get_u16(src + 52),
        .poseedor = get_u16(src + 54),
        .msi = get_u16(src + 56),
        .permiso = src[58],
        .banderas = src[59],
        .mascara_copias = get_u64(src + 64),
        .page_nbytes = get_u32(src + 72),
        .version = get_u32(src + 76),
        .datos = n > DSM_PAGE_DATA_BASE ? src + DSM_PAGE_DATA_BASE : NULL,
    };
    if (!page_data_valida(&datos)) {
        return -1;
    }
    *msg = datos;
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

static int cabecera_cierre_valida(const dsm_cabecera_t *cab)
{
    return cabecera_pedido(cab, DSM_TIPO_REGION_CLOSE) ||
           cabecera_respuesta(cab, DSM_TIPO_REGION_CLOSE_ACK) ||
           cabecera_pedido(cab, DSM_TIPO_REGION_COPY_CLOSE) ||
           cabecera_respuesta(cab, DSM_TIPO_REGION_COPY_CLOSE_ACK);
}

int dsm_region_cierre_codificar(uint8_t *buf, size_t cap, dsm_cabecera_t *cab,
                                uint32_t reserva, size_t *out)
{
    if (!cabecera_cierre_valida(cab) || (uint16_t)reserva == 0 || out == NULL ||
        preparar(buf, cap, cab, DSM_REGION_CIERRE_BYTES) != 0) {
        return -1;
    }
    put_u32(buf + 32, reserva);
    put_u16(buf + 36, cab->estado);
    *out = DSM_REGION_CIERRE_BYTES;
    return 0;
}

int dsm_region_cierre_decodificar(const uint8_t *buf, size_t n, dsm_cabecera_t *cab,
                                  uint32_t *reserva)
{
    if (reserva == NULL || n != DSM_REGION_CIERRE_BYTES ||
        dsm_cabecera_decodificar(buf, n, cab) != 0 || !cabecera_cierre_valida(cab) ||
        get_u16(buf + 36) != cab->estado || get_u16(buf + 38) != 0 ||
        (uint16_t)get_u32(buf + 32) == 0) {
        return -1;
    }
    *reserva = get_u32(buf + 32);
    return 0;
}

static void escribir_info(uint8_t *p, const dsm_region_info_t *msg)
{
    put_u32(p + 0, msg->reserva_id);
    put_u32(p + 4, msg->total_tramos);
    put_u64(p + 8, msg->tamano_bytes);
    put_u32(p + 16, msg->primer_tramo);
    put_u32(p + 20, msg->cantidad_tramos);
    put_u32(p + 24, msg->total_paginas);
    for (uint32_t i = 0; i < msg->cantidad_tramos; i++) {
        uint8_t *tramo = p + 28 + (size_t)i * 16u;
        put_u32(tramo + 0, msg->tramos[i].orden_inicial);
        put_u32(tramo + 4, msg->tramos[i].cantidad);
        put_u64(tramo + 8, msg->tramos[i].gpn_inicial);
    }
}

static int leer_info(const uint8_t *p, size_t n, dsm_region_info_t *msg)
{
    if (n < 28 || (n - 28u) % 16u != 0) {
        return -1;
    }
    size_t cantidad = (n - 28u) / 16u;
    if (cantidad > DSM_MAX_TRAMOS || get_u32(p + 20) != cantidad) {
        return -1;
    }
    memset(msg, 0, sizeof *msg);
    msg->reserva_id = get_u32(p + 0);
    msg->total_tramos = get_u32(p + 4);
    msg->tamano_bytes = get_u64(p + 8);
    msg->primer_tramo = get_u32(p + 16);
    msg->cantidad_tramos = (uint32_t)cantidad;
    msg->total_paginas = get_u32(p + 24);
    for (size_t i = 0; i < cantidad; i++) {
        const uint8_t *tramo = p + 28 + i * 16u;
        msg->tramos[i].orden_inicial = get_u32(tramo + 0);
        msg->tramos[i].cantidad = get_u32(tramo + 4);
        msg->tramos[i].gpn_inicial = get_u64(tramo + 8);
    }
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
    escribir_info(dst + DSM_CABECERA_BYTES, msg);
    *out = longitud;
    return 0;
}

int dsm_region_info_decodificar(const uint8_t *src, size_t n, dsm_cabecera_t *cab,
                                dsm_region_info_t *msg)
{
    if (msg == NULL || dsm_cabecera_decodificar(src, n, cab) != 0 ||
        cab->tipo != DSM_TIPO_REGION_INFO || n != cab->longitud) {
        return -1;
    }
    if (cab->estado != DSM_ESTADO_OK) {
        if (n != DSM_CABECERA_BYTES) {
            return -1;
        }
        memset(msg, 0, sizeof *msg);
        return 0;
    }
    return leer_info(src + DSM_CABECERA_BYTES, n - DSM_CABECERA_BYTES, msg);
}

static int cabecera_capacidad_valida(const dsm_cabecera_t *cab)
{
    if (cab == NULL || cab->tipo < DSM_TIPO_CAP_PREPARE ||
        cab->tipo > DSM_TIPO_CAP_RELEASE_ACK) {
        return 0;
    }
    return (cab->tipo & 1u) ? cabecera_respuesta(cab, cab->tipo) :
                              cabecera_pedido(cab, cab->tipo);
}

static int capacidad_valida(const dsm_cabecera_t *cab, const dsm_capacidad_t *msg)
{
    if (!cabecera_capacidad_valida(cab) || msg == NULL || msg->token == 0 ||
        (uint16_t)msg->reserva_id == 0) {
        return 0;
    }
    if (cab->tipo == DSM_TIPO_CAP_PREPARE || cab->tipo == DSM_TIPO_CAP_PREPARE_ACK) {
        if (msg->max_paginas == 0 || msg->max_tramos == 0 ||
            msg->max_tramos > DSM_TRAMOS_POR_HOGAR ||
            msg->orden > UINT32_MAX - msg->max_paginas) {
            return 0;
        }
    } else if (msg->orden != 0 || msg->max_paginas != 0 || msg->max_tramos != 0) {
        return 0;
    }
    if (cab->estado == DSM_ESTADO_OK &&
        (cab->tipo == DSM_TIPO_CAP_COMMIT || cab->tipo == DSM_TIPO_CAP_PREPARE_ACK)) {
        return msg->info.reserva_id == msg->reserva_id &&
               msg->info.cantidad_tramos <= DSM_MAX_TRAMOS &&
               msg->info.cantidad_tramos != 0 && msg->info.total_paginas != 0;
    }
    return 1;
}

int dsm_capacidad_codificar(uint8_t *dst, size_t cap, dsm_cabecera_t *cab,
                             const dsm_capacidad_t *msg, size_t *out)
{
    if (out == NULL || !capacidad_valida(cab, msg)) {
        return -1;
    }
    int manifiesto = cab->estado == DSM_ESTADO_OK &&
                     (cab->tipo == DSM_TIPO_CAP_COMMIT || cab->tipo == DSM_TIPO_CAP_PREPARE_ACK);
    uint32_t longitud = DSM_CAPACIDAD_BASE_BYTES;
    if (manifiesto) {
        longitud += 28u + 16u * msg->info.cantidad_tramos;
    }
    if (preparar(dst, cap, cab, longitud) != 0) {
        return -1;
    }
    put_u32(dst + 32, msg->reserva_id);
    put_u32(dst + 36, msg->orden);
    put_u32(dst + 40, msg->max_paginas);
    put_u32(dst + 44, msg->max_tramos);
    put_u64(dst + 48, msg->token);
    if (manifiesto) {
        escribir_info(dst + DSM_CAPACIDAD_BASE_BYTES, &msg->info);
    }
    *out = longitud;
    return 0;
}

int dsm_capacidad_decodificar(const uint8_t *src, size_t n, dsm_cabecera_t *cab,
                               dsm_capacidad_t *msg)
{
    if (msg == NULL || n < DSM_CAPACIDAD_BASE_BYTES || n > DSM_CAPACIDAD_MAX ||
        dsm_cabecera_decodificar(src, n, cab) != 0 || n != cab->longitud ||
        !cabecera_capacidad_valida(cab)) {
        return -1;
    }
    memset(msg, 0, sizeof *msg);
    msg->reserva_id = get_u32(src + 32);
    msg->orden = get_u32(src + 36);
    msg->max_paginas = get_u32(src + 40);
    msg->max_tramos = get_u32(src + 44);
    msg->token = get_u64(src + 48);
    int manifiesto = cab->estado == DSM_ESTADO_OK &&
                     (cab->tipo == DSM_TIPO_CAP_COMMIT || cab->tipo == DSM_TIPO_CAP_PREPARE_ACK);
    if (manifiesto) {
        if (leer_info(src + DSM_CAPACIDAD_BASE_BYTES, n - DSM_CAPACIDAD_BASE_BYTES, &msg->info) != 0) {
            return -1;
        }
    } else if (n != DSM_CAPACIDAD_BASE_BYTES) {
        return -1;
    }
    return capacidad_valida(cab, msg) ? 0 : -1;
}

int dsm_stats_request_codificar(uint8_t *dst, size_t cap, dsm_cabecera_t *cab,
                                const dsm_stats_request_t *msg, size_t *out)
{
    if (msg == NULL || out == NULL || cab == NULL || cab->tipo != DSM_TIPO_STATS_REQUEST ||
        cab->banderas != 0 || cab->id_corr != 0 ||
        (msg->seleccion != DSM_STATS_NODOS && msg->seleccion != DSM_STATS_PAGINAS &&
         msg->seleccion != DSM_STATS_AMBOS) ||
        preparar(dst, cap, cab, 40) != 0) {
        return -1;
    }
    dst[32] = msg->seleccion;
    *out = 40;
    return 0;
}

int dsm_stats_request_decodificar(const uint8_t *src, size_t n, dsm_cabecera_t *cab,
                                  dsm_stats_request_t *msg)
{
    if (msg == NULL || n != 40 || dsm_cabecera_decodificar(src, n, cab) != 0 ||
        cab->tipo != DSM_TIPO_STATS_REQUEST || cab->banderas != 0 || cab->id_corr != 0 ||
        cab->longitud != 40 ||
        (src[32] != DSM_STATS_NODOS && src[32] != DSM_STATS_PAGINAS && src[32] != DSM_STATS_AMBOS)) {
        return -1;
    }
    msg->seleccion = src[32];
    return 0;
}

static void escribir_pagina_consulta(uint8_t *p, const dsm_pagina_consulta_t *pag)
{
    memset(p, 0, DSM_FICHA_PAGINA);
    put_u64(p + 0, pag->gpn);
    put_u64(p + 8, pag->mascara);
    put_u32(p + 16, pag->proc);
    put_u32(p + 20, pag->reserva);
    put_u32(p + 24, pag->orden);
    put_u16(p + 28, pag->hogar);
    put_u16(p + 30, pag->poseedor);
    p[32] = pag->msi;
    p[33] = pag->permiso;
    p[34] = pag->banderas;
    p[35] = pag->copias;
}

static void leer_pagina_consulta(const uint8_t *p, dsm_pagina_consulta_t *pag)
{
    memset(pag, 0, sizeof *pag);
    pag->gpn = get_u64(p + 0);
    pag->mascara = get_u64(p + 8);
    pag->proc = get_u32(p + 16);
    pag->reserva = get_u32(p + 20);
    pag->orden = get_u32(p + 24);
    pag->hogar = get_u16(p + 28);
    pag->poseedor = get_u16(p + 30);
    pag->msi = p[32];
    pag->permiso = p[33];
    pag->banderas = p[34];
    pag->copias = p[35];
}

int dsm_stats_reply_codificar(uint8_t *dst, size_t cap, dsm_cabecera_t *cab,
                              const dsm_stats_reply_t *msg, size_t *out)
{
    if (msg == NULL || out == NULL || cab == NULL || cab->tipo != DSM_TIPO_STATS_REPLY ||
        (cab->banderas & DSM_FLAG_RESPUESTA) == 0 || cab->id_corr == 0 ||
        msg->n > DSM_MAX_FICHAS || (msg->p != 0 && msg->paginas == NULL)) {
        return -1;
    }
    uint64_t longitud = (uint64_t)DSM_STATS_PREF + (uint64_t)msg->n * 104u +
                        (uint64_t)msg->p * DSM_FICHA_PAGINA;
    if (longitud > DSM_LONGITUD_MAX || preparar(dst, cap, cab, (uint32_t)longitud) != 0) {
        return -1;
    }
    put_u16(dst + 32, msg->emisor);
    put_u16(dst + 34, msg->n);
    put_u32(dst + 36, msg->p);
    for (int i = 0; i < DSM_STATS_CONTADORES; i++) {
        put_u64(dst + 40 + (size_t)i * 8u, msg->contadores[i]);
    }
    for (uint16_t i = 0; i < msg->n; i++) {
        escribir_ficha(dst + DSM_STATS_PREF + (size_t)i * 104u, &msg->fichas[i]);
    }
    uint8_t *base = dst + DSM_STATS_PREF + (size_t)msg->n * 104u;
    for (uint32_t i = 0; i < msg->p; i++) {
        escribir_pagina_consulta(base + (size_t)i * DSM_FICHA_PAGINA, &msg->paginas[i]);
    }
    *out = (size_t)longitud;
    return 0;
}

int dsm_stats_reply_decodificar(const uint8_t *src, size_t n, dsm_cabecera_t *cab,
                                dsm_stats_reply_t *msg)
{
    if (msg == NULL || n < DSM_STATS_PREF || dsm_cabecera_decodificar(src, n, cab) != 0 ||
        cab->tipo != DSM_TIPO_STATS_REPLY || (cab->banderas & DSM_FLAG_RESPUESTA) == 0 ||
        cab->id_corr == 0 || n != cab->longitud) {
        return -1;
    }
    uint16_t cantidad_nodos = get_u16(src + 34);
    uint32_t cantidad_paginas = get_u32(src + 36);
    uint64_t esperada = (uint64_t)DSM_STATS_PREF + (uint64_t)cantidad_nodos * 104u +
                        (uint64_t)cantidad_paginas * DSM_FICHA_PAGINA;
    if (cantidad_nodos > DSM_MAX_FICHAS || esperada != n) {
        return -1;
    }
    dsm_pagina_consulta_t *paginas = NULL;
    if (cantidad_paginas != 0) {
        paginas = calloc(cantidad_paginas, sizeof *paginas);
        if (paginas == NULL) {
            return -1;
        }
    }
    memset(msg, 0, sizeof *msg);
    msg->emisor = get_u16(src + 32);
    msg->n = cantidad_nodos;
    msg->p = cantidad_paginas;
    for (int i = 0; i < DSM_STATS_CONTADORES; i++) {
        msg->contadores[i] = get_u64(src + 40 + (size_t)i * 8u);
    }
    for (uint16_t i = 0; i < cantidad_nodos; i++) {
        leer_ficha(src + DSM_STATS_PREF + (size_t)i * 104u, &msg->fichas[i]);
    }
    const uint8_t *base = src + DSM_STATS_PREF + (size_t)cantidad_nodos * 104u;
    for (uint32_t i = 0; i < cantidad_paginas; i++) {
        leer_pagina_consulta(base + (size_t)i * DSM_FICHA_PAGINA, &paginas[i]);
    }
    msg->paginas = paginas;
    return 0;
}

int dsm_proto_error_codificar(uint8_t *dst, size_t cap, dsm_cabecera_t *cab,
                              const dsm_proto_error_t *msg, size_t *out)
{
    if (msg == NULL || out == NULL || cab == NULL || cab->tipo != DSM_TIPO_PROTO_ERROR ||
        cab->banderas != 0 || preparar(dst, cap, cab, DSM_PROTO_ERROR_BYTES) != 0) {
        return -1;
    }
    put_u16(dst + 32, msg->codigo);
    put_u32(dst + 36, msg->id_causante);
    memcpy(dst + 40, msg->texto, DSM_PROTO_TEXTO);
    *out = DSM_PROTO_ERROR_BYTES;
    return 0;
}

int dsm_proto_error_decodificar(const uint8_t *src, size_t n, dsm_cabecera_t *cab,
                                dsm_proto_error_t *msg)
{
    if (msg == NULL || n != DSM_PROTO_ERROR_BYTES || dsm_cabecera_decodificar(src, n, cab) != 0 ||
        cab->tipo != DSM_TIPO_PROTO_ERROR || cab->longitud != DSM_PROTO_ERROR_BYTES) {
        return -1;
    }
    memset(msg, 0, sizeof *msg);
    msg->codigo = get_u16(src + 32);
    msg->id_causante = get_u32(src + 36);
    memcpy(msg->texto, src + 40, DSM_PROTO_TEXTO);
    msg->texto[DSM_PROTO_TEXTO - 1] = '\0';
    return 0;
}

int dsm_anuncio_codificar(uint8_t *dst, size_t cap, dsm_cabecera_t *cab, uint16_t autor,
                          const char *clave, const void *datos, size_t n, size_t *out)
{
    size_t clave_n = clave == NULL ? 0 : strlen(clave);
    if (out == NULL || cab == NULL || cab->tipo != DSM_TIPO_ANUNCIO || cab->banderas != 0 ||
        cab->id_corr != 0 || clave_n == 0 || clave_n >= DSM_ANUNCIO_CLAVE || n > DSM_ANUNCIO_DATOS ||
        (n != 0 && datos == NULL)) {
        return -1;
    }
    if (preparar(dst, cap, cab, (uint32_t)(DSM_ANUNCIO_BASE + n)) != 0) {
        return -1;
    }
    put_u16(dst + 32, autor);
    put_u16(dst + 34, (uint16_t)n);
    memcpy(dst + 36, clave, clave_n);
    if (n != 0) {
        memcpy(dst + DSM_ANUNCIO_BASE, datos, n);
    }
    *out = DSM_ANUNCIO_BASE + n;
    return 0;
}

int dsm_anuncio_decodificar(const uint8_t *src, size_t n, dsm_cabecera_t *cab, uint16_t *autor,
                            char clave[DSM_ANUNCIO_CLAVE], uint8_t *datos, size_t cap, size_t *largo)
{
    if (autor == NULL || clave == NULL || largo == NULL || n < DSM_ANUNCIO_BASE ||
        dsm_cabecera_decodificar(src, n, cab) != 0 || cab->tipo != DSM_TIPO_ANUNCIO ||
        cab->banderas != 0 || cab->longitud != n) {
        return -1;
    }
    uint16_t bytes = get_u16(src + 34);
    if ((size_t)DSM_ANUNCIO_BASE + bytes != n || bytes > cap || (bytes != 0 && datos == NULL)) {
        return -1;
    }
    *autor = get_u16(src + 32);
    memcpy(clave, src + 36, DSM_ANUNCIO_CLAVE);
    clave[DSM_ANUNCIO_CLAVE - 1] = '\0';
    if (clave[0] == '\0') {
        return -1;
    }
    if (bytes != 0) {
        memcpy(datos, src + DSM_ANUNCIO_BASE, bytes);
    }
    *largo = bytes;
    return 0;
}
