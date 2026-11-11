#include "network.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int monotono_ms(uint64_t *ms)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return -1;
    }
    *ms = (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
    return 0;
}

static int iniciar_plazo(uint32_t timeout_ms, uint64_t *plazo)
{
    *plazo = 0;
    if (timeout_ms != 0) {
        if (monotono_ms(plazo) != 0) {
            return -1;
        }
        *plazo += timeout_ms;
    }
    return 0;
}

static int restante_ms(uint64_t plazo)
{
    uint64_t ahora;
    if (monotono_ms(&ahora) != 0) {
        return -1;
    }
    if (ahora >= plazo) {
        errno = ETIMEDOUT;
        return -1;
    }
    uint64_t restante = plazo - ahora;
    return restante > INT_MAX ? INT_MAX : (int)restante;
}

static int esperar(int fd, short evento, uint64_t plazo)
{
    struct pollfd pfd = {.fd = fd, .events = evento};
    for (;;) {
        int espera = plazo == 0 ? -1 : restante_ms(plazo);
        if (plazo != 0 && espera < 0) {
            return -1;
        }
        int rc = poll(&pfd, 1, espera);
        if (rc > 0) {
            return 0;
        }
        if (rc < 0 && errno != EINTR) {
            return -1;
        }
    }
}

static int enviar_completo(int fd, const void *buf, size_t len, uint64_t plazo)
{
    const uint8_t *p = buf;
    size_t off = 0;
    if (p == NULL && len != 0) {
        errno = EINVAL;
        return -1;
    }
    while (off < len) {
        if (plazo != 0 && restante_ms(plazo) < 0) {
            return -1;
        }
        ssize_t n = send(fd, p + off, len - off, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) {
            off += (size_t)n;
        } else if (n == 0) {
            errno = EPIPE;
            return -1;
        } else if (errno != EINTR) {
            if ((errno != EAGAIN && errno != EWOULDBLOCK) || esperar(fd, POLLOUT, plazo) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int recibir_completo(int fd, void *buf, size_t len, uint64_t plazo)
{
    uint8_t *p = buf;
    size_t off = 0;
    if (p == NULL && len != 0) {
        errno = EINVAL;
        return -1;
    }
    while (off < len) {
        if (plazo != 0 && restante_ms(plazo) < 0) {
            return -1;
        }
        ssize_t n = recv(fd, p + off, len - off, MSG_DONTWAIT);
        if (n > 0) {
            off += (size_t)n;
        } else if (n == 0) {
            errno = ECONNRESET;
            return -1;
        } else if (errno != EINTR) {
            if ((errno != EAGAIN && errno != EWOULDBLOCK) || esperar(fd, POLLIN, plazo) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

int send_all(int fd, const void *buf, size_t len)
{
    return enviar_completo(fd, buf, len, 0);
}

int recv_all(int fd, void *buf, size_t len)
{
    return recibir_completo(fd, buf, len, 0);
}

int dsm_enviar_trama(int fd, const void *buf, size_t len, uint32_t timeout_ms)
{
    uint64_t plazo;
    if (iniciar_plazo(timeout_ms, &plazo) != 0) {
        return -1;
    }
    return enviar_completo(fd, buf, len, plazo);
}

static int recibir_trama(int fd, uint8_t *buf, size_t cap, size_t *out, uint64_t plazo)
{
    if (buf == NULL || out == NULL || cap < DSM_CABECERA_BYTES) {
        errno = EINVAL;
        return -1;
    }
    if (recibir_completo(fd, buf, DSM_CABECERA_BYTES, plazo) != 0) {
        return -1;
    }
    dsm_cabecera_t cab;
    if (dsm_cabecera_decodificar(buf, DSM_CABECERA_BYTES, &cab) != 0) {
        errno = EPROTO;
        return -1;
    }
    if (cab.longitud > cap) {
        errno = EMSGSIZE;
        return -1;
    }
    size_t resto = cab.longitud - DSM_CABECERA_BYTES;
    if (recibir_completo(fd, buf + DSM_CABECERA_BYTES, resto, plazo) != 0) {
        return -1;
    }
    *out = cab.longitud;
    return 0;
}

int dsm_recibir_trama(int fd, uint8_t *buf, size_t cap, size_t *out)
{
    return recibir_trama(fd, buf, cap, out, 0);
}

int dsm_recibir_trama_plazo(int fd, uint8_t *buf, size_t cap, size_t *out, uint32_t timeout_ms)
{
    uint64_t plazo;
    if (iniciar_plazo(timeout_ms, &plazo) != 0) {
        return -1;
    }
    return recibir_trama(fd, buf, cap, out, plazo);
}

static int activar_nodelay(int fd)
{
    int on = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on) != 0) {
        return -1;
    }
    return 0;
}

int dsm_escuchar(uint32_t ipv4_host, uint16_t puerto, int *escucha)
{
    if (escucha == NULL || puerto == 0) {
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) != 0) {
        close(fd);
        return -1;
    }
    struct sockaddr_in dir;
    memset(&dir, 0, sizeof dir);
    dir.sin_family = AF_INET;
    dir.sin_port = htons(puerto);
    dir.sin_addr.s_addr = htonl(ipv4_host);
    if (bind(fd, (struct sockaddr *)&dir, sizeof dir) != 0 || listen(fd, DSM_CONFIG_MAX_NODOS) != 0) {
        close(fd);
        return -1;
    }
    *escucha = fd;
    return 0;
}

int dsm_aceptar(int escucha, int *conn)
{
    if (conn == NULL) {
        return -1;
    }
    int fd = accept4(escucha, NULL, NULL, SOCK_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    if (activar_nodelay(fd) != 0) {
        close(fd);
        return -1;
    }
    *conn = fd;
    return 0;
}

static int cerrar_error(int fd)
{
    int error = errno;
    close(fd);
    errno = error;
    return -1;
}

int dsm_conectar_plazo(uint32_t ipv4_host, uint16_t puerto, uint32_t timeout_ms, int *conn)
{
    if (conn == NULL || puerto == 0) {
        errno = EINVAL;
        return -1;
    }
    uint64_t plazo;
    if (iniciar_plazo(timeout_ms, &plazo) != 0) {
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        return -1;
    }
    if (activar_nodelay(fd) != 0) {
        return cerrar_error(fd);
    }
    struct sockaddr_in dir;
    memset(&dir, 0, sizeof dir);
    dir.sin_family = AF_INET;
    dir.sin_port = htons(puerto);
    dir.sin_addr.s_addr = htonl(ipv4_host);
    if (connect(fd, (struct sockaddr *)&dir, sizeof dir) != 0) {
        if ((errno != EINPROGRESS && errno != EINTR) || esperar(fd, POLLOUT, plazo) != 0) {
            return cerrar_error(fd);
        }
        int error = 0;
        socklen_t largo = sizeof error;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &largo) != 0) {
            return cerrar_error(fd);
        }
        if (error != 0) {
            errno = error;
            return cerrar_error(fd);
        }
    }
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0) {
        return cerrar_error(fd);
    }
    *conn = fd;
    return 0;
}

int dsm_conectar(uint32_t ipv4_host, uint16_t puerto, int *conn)
{
    return dsm_conectar_plazo(ipv4_host, puerto, 0, conn);
}

struct alta_ctx {
    int fd;
    const dsm_config_t *cfg;
    uint16_t self;
    uint16_t peer;
    uint64_t plazo;
    uint32_t seq;
    uint32_t id_hello;
    int tiene;
    int hello;
    int welcome;
    int heartbeat_ack;
    uint8_t estado_tras_hello;
    dsm_par_t par;
};

static int nombres_iguales(const char *a, const char *b)
{
    return strncmp(a, b, DSM_NOMBRE_MAX) == 0;
}

static int anotar(struct alta_ctx *cx, uint16_t id, uint16_t puerto, uint32_t ip,
                  uint64_t memoria, uint64_t base, const char *nombre)
{
    const dsm_nodo_cfg_t *esperado = dsm_config_nodo(cx->cfg, cx->peer);
    uint64_t base_esperada = dsm_config_base(cx->cfg, cx->peer);
    if (esperado == NULL || id != cx->peer || puerto != esperado->puerto ||
        ip != esperado->ipv4_host || memoria != esperado->memoria ||
        base != base_esperada || !nombres_iguales(nombre, esperado->nombre)) {
        return -1;
    }
    if (cx->tiene) {
        if (cx->par.id != id || cx->par.puerto != puerto || cx->par.ipv4_host != ip ||
            cx->par.memoria != memoria || cx->par.base != base ||
            !nombres_iguales(cx->par.nombre, nombre)) {
            return -1;
        }
        return 0;
    }
    memset(&cx->par, 0, sizeof cx->par);
    cx->par.id = id;
    cx->par.estado = DSM_NODO_UNION;
    cx->par.puerto = puerto;
    cx->par.ipv4_host = ip;
    cx->par.memoria = memoria;
    cx->par.base = base;
    memcpy(cx->par.nombre, nombre, DSM_NOMBRE_MAX);
    cx->par.nombre[DSM_NOMBRE_MAX - 1] = '\0';
    cx->tiene = 1;
    return 0;
}

static int enviar_hello(struct alta_ctx *cx)
{
    const dsm_nodo_cfg_t *yo = dsm_config_nodo(cx->cfg, cx->self);
    if (yo == NULL) {
        return -1;
    }
    dsm_hello_t msg;
    memset(&msg, 0, sizeof msg);
    msg.node_id = yo->id;
    msg.conocidos = 0;
    msg.ipv4_host = yo->ipv4_host;
    msg.puerto = yo->puerto;
    msg.memoria = yo->memoria;
    msg.base = dsm_config_base(cx->cfg, yo->id);
    memcpy(msg.nombre, yo->nombre, DSM_NOMBRE_MAX);

    dsm_cabecera_t cab;
    memset(&cab, 0, sizeof cab);
    cab.version = DSM_PROTO_VERSION;
    cab.tipo = DSM_TIPO_NODE_HELLO;
    cab.origen = cx->self;
    cab.destino = cx->peer;
    cab.id_msg = cx->seq++;
    cab.estado = DSM_ESTADO_OK;
    cx->id_hello = cab.id_msg;

    uint8_t buf[128];
    size_t n = 0;
    if (dsm_hello_codificar(buf, sizeof buf, &cab, &msg, &n) != 0) {
        return -1;
    }
    return enviar_completo(cx->fd, buf, n, cx->plazo);
}

static int enviar_welcome(struct alta_ctx *cx, uint32_t id_corr)
{
    const dsm_nodo_cfg_t *yo = dsm_config_nodo(cx->cfg, cx->self);
    if (yo == NULL) {
        return -1;
    }
    uint32_t totales = (uint32_t)(yo->memoria / cx->cfg->page_size);
    dsm_welcome_t msg;
    memset(&msg, 0, sizeof msg);
    msg.emisor = yo->id;
    msg.n = 1;
    msg.epoca = 1;
    msg.fichas[0].id = yo->id;
    msg.fichas[0].estado = DSM_NODO_UNION;
    msg.fichas[0].ipv4_host = yo->ipv4_host;
    msg.fichas[0].puerto = yo->puerto;
    msg.fichas[0].memoria = yo->memoria;
    msg.fichas[0].base = dsm_config_base(cx->cfg, yo->id);
    msg.fichas[0].libres = totales;
    msg.fichas[0].totales = totales;
    memcpy(msg.fichas[0].nombre, yo->nombre, DSM_NOMBRE_MAX);

    dsm_cabecera_t cab;
    memset(&cab, 0, sizeof cab);
    cab.version = DSM_PROTO_VERSION;
    cab.tipo = DSM_TIPO_NODE_WELCOME;
    cab.banderas = DSM_FLAG_RESPUESTA;
    cab.origen = cx->self;
    cab.destino = cx->peer;
    cab.id_msg = cx->seq++;
    cab.id_corr = id_corr;
    cab.estado = DSM_ESTADO_OK;

    uint8_t buf[256];
    size_t n = 0;
    if (dsm_welcome_codificar(buf, sizeof buf, &cab, &msg, &n) != 0) {
        return -1;
    }
    return enviar_completo(cx->fd, buf, n, cx->plazo);
}

static int responder_fuera_de_rango(struct alta_ctx *cx, uint16_t destino, uint32_t id_corr)
{
    dsm_cabecera_t cab;
    memset(&cab, 0, sizeof cab);
    cab.version = DSM_PROTO_VERSION;
    cab.tipo = DSM_TIPO_NODE_WELCOME;
    cab.banderas = DSM_FLAG_RESPUESTA;
    cab.longitud = DSM_CABECERA_BYTES;
    cab.origen = cx->self;
    cab.destino = destino;
    cab.id_msg = cx->seq++;
    cab.id_corr = id_corr;
    cab.estado = DSM_ESTADO_RANGO;
    uint8_t buf[DSM_CABECERA_BYTES];
    if (dsm_cabecera_codificar(buf, &cab) != 0) {
        return -1;
    }
    if (enviar_completo(cx->fd, buf, sizeof buf, cx->plazo) != 0) {
        return -1;
    }
    errno = EPROTO;
    return -1;
}

static uint64_t ahora_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static int enviar_latido(struct alta_ctx *cx, dsm_heartbeat_t *enviado, uint32_t *id_msg)
{
    dsm_cabecera_t cab;
    memset(&cab, 0, sizeof cab);
    cab.version = DSM_PROTO_VERSION;
    cab.tipo = DSM_TIPO_HEARTBEAT;
    cab.origen = cx->self;
    cab.destino = cx->peer;
    cab.id_msg = cx->seq++;
    cab.estado = DSM_ESTADO_OK;
    enviado->marca = ((uint64_t)cx->self << 32) | cab.id_msg;
    enviado->instante_ms = ahora_ms();
    *id_msg = cab.id_msg;
    uint8_t buf[48];
    size_t n = 0;
    if (dsm_heartbeat_codificar(buf, sizeof buf, &cab, enviado, &n) != 0) {
        return -1;
    }
    return enviar_completo(cx->fd, buf, n, cx->plazo);
}

static int enviar_latido_ack(struct alta_ctx *cx, const dsm_cabecera_t *pedido,
                             const dsm_heartbeat_t *eco)
{
    dsm_cabecera_t cab;
    memset(&cab, 0, sizeof cab);
    cab.version = DSM_PROTO_VERSION;
    cab.tipo = DSM_TIPO_HEARTBEAT_ACK;
    cab.banderas = DSM_FLAG_RESPUESTA;
    cab.origen = cx->self;
    cab.destino = cx->peer;
    cab.id_msg = cx->seq++;
    cab.id_corr = pedido->id_msg;
    cab.estado = DSM_ESTADO_OK;
    uint8_t buf[48];
    size_t n = 0;
    if (dsm_heartbeat_codificar(buf, sizeof buf, &cab, eco, &n) != 0) {
        return -1;
    }
    return enviar_completo(cx->fd, buf, n, cx->plazo);
}

static int recibir(struct alta_ctx *cx, uint8_t *buf, size_t cap, size_t *n, dsm_cabecera_t *cab)
{
    if (recibir_trama(cx->fd, buf, cap, n, cx->plazo) != 0) {
        return -1;
    }
    if (dsm_cabecera_decodificar(buf, *n, cab) != 0) {
        return -1;
    }
    if (cab->destino != cx->self) {
        return -1;
    }
    return 0;
}

static int tomar_hello(struct alta_ctx *cx, const uint8_t *buf, size_t n,
                      const dsm_cabecera_t *cab, int identificar_par)
{
    dsm_hello_t msg;
    dsm_cabecera_t copia;
    if (cab->banderas != 0 || cab->id_corr != 0 || cab->estado != DSM_ESTADO_OK) {
        return -1;
    }
    if (dsm_hello_decodificar(buf, n, &copia, &msg) != 0) {
        return -1;
    }
    if (msg.node_id != cab->origen) {
        return -1;
    }
    if (dsm_config_nodo(cx->cfg, msg.node_id) == NULL ||
        (identificar_par && msg.node_id >= cx->self)) {
        return responder_fuera_de_rango(cx, cab->origen, cab->id_msg);
    }
    if (identificar_par) {
        cx->peer = msg.node_id;
    }
    if (anotar(cx, msg.node_id, msg.puerto, msg.ipv4_host, msg.memoria, msg.base, msg.nombre) != 0 ||
        msg.conocidos != 0) {
        return responder_fuera_de_rango(cx, cab->origen, cab->id_msg);
    }
    cx->hello = 1;
    cx->estado_tras_hello = cx->par.estado;
    return 0;
}

static int tomar_welcome(struct alta_ctx *cx, const uint8_t *buf, size_t n, const dsm_cabecera_t *cab)
{
    dsm_welcome_t msg;
    dsm_cabecera_t copia;
    if (cab->banderas != DSM_FLAG_RESPUESTA || cab->id_corr != cx->id_hello) {
        return -1;
    }
    if (dsm_welcome_decodificar(buf, n, &copia, &msg) != 0) {
        return -1;
    }
    if (cab->estado != DSM_ESTADO_OK) {
        return -1;
    }
    const dsm_nodo_cfg_t *par = dsm_config_nodo(cx->cfg, cx->peer);
    if (par == NULL) {
        return -1;
    }
    uint32_t totales = (uint32_t)(par->memoria / cx->cfg->page_size);
    if (msg.emisor != cab->origen || msg.n != 1 || msg.epoca != 1 || msg.fichas[0].id != msg.emisor ||
        msg.fichas[0].estado != DSM_NODO_UNION || msg.fichas[0].libres != totales ||
        msg.fichas[0].totales != totales) {
        return -1;
    }
    if (anotar(cx, msg.fichas[0].id, msg.fichas[0].puerto, msg.fichas[0].ipv4_host, msg.fichas[0].memoria,
               msg.fichas[0].base, msg.fichas[0].nombre) != 0) {
        return -1;
    }
    cx->welcome = 1;
    return 0;
}

static int tomar_latido_ajeno(struct alta_ctx *cx, const uint8_t *buf, size_t n,
                              const dsm_cabecera_t *cab)
{
    dsm_heartbeat_t msg;
    dsm_cabecera_t copia;
    if (cab->tipo != DSM_TIPO_HEARTBEAT || cab->banderas != 0 || cab->id_corr != 0 ||
        cab->estado != DSM_ESTADO_OK || cab->origen != cx->peer) {
        return -1;
    }
    if (dsm_heartbeat_decodificar(buf, n, &copia, &msg) != 0) {
        return -1;
    }
    return enviar_latido_ack(cx, cab, &msg);
}

static int tomar_ack(struct alta_ctx *cx, const uint8_t *buf, size_t n, const dsm_cabecera_t *cab,
                     uint32_t id_msg, const dsm_heartbeat_t *enviado)
{
    dsm_heartbeat_t msg;
    dsm_cabecera_t copia;
    if (cab->tipo != DSM_TIPO_HEARTBEAT_ACK || cab->banderas != DSM_FLAG_RESPUESTA ||
        cab->id_corr != id_msg || cab->estado != DSM_ESTADO_OK || cab->origen != cx->peer) {
        return -1;
    }
    if (dsm_heartbeat_decodificar(buf, n, &copia, &msg) != 0) {
        return -1;
    }
    if (msg.marca != enviado->marca || msg.instante_ms != enviado->instante_ms) {
        return -1;
    }
    cx->par.estado = DSM_NODO_EN_LINEA;
    cx->heartbeat_ack = 1;
    return 0;
}

static int ronda_latido_primero_yo(struct alta_ctx *cx)
{
    dsm_heartbeat_t enviado;
    uint32_t id_msg = 0;
    if (enviar_latido(cx, &enviado, &id_msg) != 0) {
        return -1;
    }
    uint8_t buf[256];
    size_t n = 0;
    dsm_cabecera_t cab;
    if (recibir(cx, buf, sizeof buf, &n, &cab) != 0 ||
        tomar_ack(cx, buf, n, &cab, id_msg, &enviado) != 0) {
        return -1;
    }
    if (recibir(cx, buf, sizeof buf, &n, &cab) != 0 || tomar_latido_ajeno(cx, buf, n, &cab) != 0) {
        return -1;
    }
    return 0;
}

static int ronda_latido_primero_el(struct alta_ctx *cx)
{
    uint8_t buf[256];
    size_t n = 0;
    dsm_cabecera_t cab;
    if (recibir(cx, buf, sizeof buf, &n, &cab) != 0 || tomar_latido_ajeno(cx, buf, n, &cab) != 0) {
        return -1;
    }
    dsm_heartbeat_t enviado;
    uint32_t id_msg = 0;
    if (enviar_latido(cx, &enviado, &id_msg) != 0) {
        return -1;
    }
    if (recibir(cx, buf, sizeof buf, &n, &cab) != 0 ||
        tomar_ack(cx, buf, n, &cab, id_msg, &enviado) != 0) {
        return -1;
    }
    return 0;
}

static int secuencia_cliente(struct alta_ctx *cx)
{
    uint8_t buf[256];
    size_t n = 0;
    dsm_cabecera_t cab;
    if (enviar_hello(cx) != 0 || recibir(cx, buf, sizeof buf, &n, &cab) != 0) {
        return -1;
    }
    if (cab.tipo != DSM_TIPO_NODE_WELCOME || tomar_welcome(cx, buf, n, &cab) != 0) {
        return -1;
    }
    if (recibir(cx, buf, sizeof buf, &n, &cab) != 0 || cab.tipo != DSM_TIPO_NODE_HELLO ||
        tomar_hello(cx, buf, n, &cab, 0) != 0) {
        return -1;
    }
    if (enviar_welcome(cx, cab.id_msg) != 0) {
        return -1;
    }
    return ronda_latido_primero_yo(cx);
}

static int secuencia_servidor(struct alta_ctx *cx, int identificar_par)
{
    uint8_t buf[256];
    size_t n = 0;
    dsm_cabecera_t cab;
    if (recibir(cx, buf, sizeof buf, &n, &cab) != 0 || cab.tipo != DSM_TIPO_NODE_HELLO ||
        tomar_hello(cx, buf, n, &cab, identificar_par) != 0) {
        return -1;
    }
    uint32_t id_hello_ajeno = cab.id_msg;
    if (enviar_welcome(cx, id_hello_ajeno) != 0 || enviar_hello(cx) != 0) {
        return -1;
    }
    if (recibir(cx, buf, sizeof buf, &n, &cab) != 0 || cab.tipo != DSM_TIPO_NODE_WELCOME ||
        tomar_welcome(cx, buf, n, &cab) != 0) {
        return -1;
    }
    return ronda_latido_primero_el(cx);
}

static int alta(int fd, const dsm_config_t *cfg, uint16_t self_id, uint16_t peer_id,
                int identificar_par, dsm_alta_resultado_t *out)
{
    if (cfg == NULL || out == NULL || cfg->page_size == 0 ||
        dsm_config_nodo(cfg, self_id) == NULL ||
        (!identificar_par && (self_id == peer_id || dsm_config_nodo(cfg, peer_id) == NULL))) {
        errno = EINVAL;
        return -1;
    }
    struct alta_ctx cx;
    memset(&cx, 0, sizeof cx);
    cx.fd = fd;
    cx.cfg = cfg;
    cx.self = self_id;
    cx.peer = peer_id;
    cx.seq = 1;
    if (iniciar_plazo(cfg->rpc_timeout_ms, &cx.plazo) != 0) {
        return -1;
    }
    int rc = (!identificar_par && self_id < peer_id) ?
             secuencia_cliente(&cx) : secuencia_servidor(&cx, identificar_par);
    memset(out, 0, sizeof *out);
    out->hello = cx.hello;
    out->welcome = cx.welcome;
    out->heartbeat_ack = cx.heartbeat_ack;
    out->estado_tras_hello = cx.estado_tras_hello;
    out->siguiente_msg = cx.seq;
    out->par = cx.par;
    if (rc != 0 || !cx.hello || !cx.welcome || !cx.heartbeat_ack ||
        cx.par.estado != DSM_NODO_EN_LINEA || cx.estado_tras_hello != DSM_NODO_UNION) {
        return -1;
    }
    return 0;
}

int dsm_alta_par(int fd, const dsm_config_t *cfg, uint16_t self_id, uint16_t peer_id,
                 dsm_alta_resultado_t *out)
{
    return alta(fd, cfg, self_id, peer_id, 0, out);
}

int dsm_alta_aceptada(int fd, const dsm_config_t *cfg, uint16_t self_id, dsm_alta_resultado_t *out)
{
    return alta(fd, cfg, self_id, 0, 1, out);
}

struct red_par {
    int fd;
    int activo;
    int enviando;
};

struct red_trabajador {
    dsm_red_t *red;
    pthread_t hilo;
    uint16_t indice;
    int creado;
    int fd;
    uint8_t *buf;
};

struct dsm_red {
    const dsm_config_t *cfg;
    uint16_t self;
    dsm_red_trama_t atender;
    void *ctx;
    int escucha;
    pthread_t receptor;
    int receptor_creado;
    pthread_mutex_t mutex;
    pthread_cond_t cambio;
    int iniciado;
    int cerrando;
    uint32_t siguiente;
    struct red_par pares[DSM_CONFIG_MAX_NODOS];
    struct red_trabajador trabajadores[DSM_CONFIG_MAX_NODOS];
};

static int error_red(int error)
{
    errno = error;
    return -1;
}

/* Se llama con el mutex de la red tomado. */
static int esperar_cambio(dsm_red_t *red, uint64_t plazo)
{
    struct timespec fin = {
        .tv_sec = (time_t)(plazo / 1000),
        .tv_nsec = (long)(plazo % 1000) * 1000000L,
    };
    return pthread_cond_timedwait(&red->cambio, &red->mutex, &fin);
}

static int esperar_inicio(dsm_red_t *red)
{
    pthread_mutex_lock(&red->mutex);
    while (!red->iniciado && !red->cerrando) {
        pthread_cond_wait(&red->cambio, &red->mutex);
    }
    int seguir = !red->cerrando;
    pthread_mutex_unlock(&red->mutex);
    return seguir;
}

static int par_valido(const dsm_red_t *red, uint16_t par)
{
    return red != NULL && par != red->self && par < red->cfg->cantidad_nodos;
}

uint32_t dsm_red_id(dsm_red_t *red)
{
    pthread_mutex_lock(&red->mutex);
    if (red->siguiente == 0) {
        red->siguiente = 1;
    }
    uint32_t id = red->siguiente++;
    pthread_mutex_unlock(&red->mutex);
    return id;
}

int dsm_red_esperar(dsm_red_t *red, uint16_t par)
{
    if (!par_valido(red, par)) {
        return error_red(EINVAL);
    }
    uint64_t plazo;
    if (iniciar_plazo(red->cfg->rpc_timeout_ms, &plazo) != 0) {
        return -1;
    }
    pthread_mutex_lock(&red->mutex);
    int error = 0;
    while (!red->cerrando && !red->pares[par].activo && error == 0) {
        error = esperar_cambio(red, plazo);
    }
    if (red->cerrando) {
        error = ECANCELED;
    }
    pthread_mutex_unlock(&red->mutex);
    return error == 0 ? 0 : error_red(error);
}

int dsm_red_enviar(dsm_red_t *red, uint16_t par, const uint8_t *buf, size_t n)
{
    if (!par_valido(red, par) || buf == NULL || n < DSM_CABECERA_BYTES || n > DSM_LONGITUD_MAX) {
        return error_red(EINVAL);
    }
    uint64_t plazo;
    if (iniciar_plazo(red->cfg->rpc_timeout_ms, &plazo) != 0) {
        return -1;
    }

    struct red_par *enlace = &red->pares[par];
    pthread_mutex_lock(&red->mutex);
    int error = 0;
    while (!red->cerrando && enlace->activo && enlace->enviando && error == 0) {
        error = esperar_cambio(red, plazo);
    }
    if (red->cerrando) {
        error = ECANCELED;
    } else if (!enlace->activo) {
        error = ENOTCONN;
    }
    if (error != 0) {
        pthread_mutex_unlock(&red->mutex);
        return error_red(error);
    }
    enlace->enviando = 1;
    int fd = enlace->fd;
    pthread_mutex_unlock(&red->mutex);

    int rc = enviar_completo(fd, buf, n, plazo);
    error = errno;
    pthread_mutex_lock(&red->mutex);
    if (rc != 0) {
        /* Un envio parcial deja la conexion inutilizable. El lector cierra el fd. */
        enlace->activo = 0;
        shutdown(fd, SHUT_RDWR);
    }
    enlace->enviando = 0;
    pthread_cond_broadcast(&red->cambio);
    pthread_mutex_unlock(&red->mutex);
    return rc == 0 ? 0 : error_red(error);
}

static int registrar_conexion(dsm_red_t *red, int fd, const dsm_alta_resultado_t *alta)
{
    struct red_par *enlace = &red->pares[alta->par.id];
    pthread_mutex_lock(&red->mutex);
    int registrar = !red->cerrando && enlace->fd < 0;
    if (registrar) {
        enlace->fd = fd;
        enlace->activo = 1;
        if (red->siguiente < alta->siguiente_msg) {
            red->siguiente = alta->siguiente_msg;
        }
        pthread_cond_broadcast(&red->cambio);
    }
    pthread_mutex_unlock(&red->mutex);
    if (registrar) {
        printf("nodo %u par %u hello %d welcome %d estado %u\n",
               red->self, alta->par.id, alta->hello, alta->welcome, alta->par.estado);
    }
    return registrar;
}

static void atender_conexion(struct red_trabajador *trabajador, uint16_t par)
{
    dsm_red_t *red = trabajador->red;
    int fd = trabajador->fd;
    while (esperar(fd, POLLIN, 0) == 0) {
        dsm_cabecera_t cab;
        size_t n;
        if (dsm_recibir_trama_plazo(fd, trabajador->buf, DSM_LONGITUD_MAX, &n,
                                   red->cfg->rpc_timeout_ms) != 0 ||
            dsm_cabecera_decodificar(trabajador->buf, n, &cab) != 0 ||
            cab.origen != par || cab.destino != red->self ||
            red->atender(red->ctx, par, &cab, trabajador->buf, n) != 0) {
            break;
        }
    }
}

static int procesar_conexion(struct red_trabajador *trabajador, int saliente)
{
    dsm_red_t *red = trabajador->red;
    int fd = trabajador->fd;
    dsm_alta_resultado_t alta;
    int rc = saliente ? dsm_alta_par(fd, red->cfg, red->self, trabajador->indice, &alta)
                      : dsm_alta_aceptada(fd, red->cfg, red->self, &alta);
    struct red_par *enlace = NULL;
    if (rc == 0 && registrar_conexion(red, fd, &alta)) {
        enlace = &red->pares[alta.par.id];
        atender_conexion(trabajador, alta.par.id);
    }

    pthread_mutex_lock(&red->mutex);
    if (enlace != NULL) {
        enlace->activo = 0;
        shutdown(fd, SHUT_RDWR);
        pthread_cond_broadcast(&red->cambio);
        while (enlace->enviando) {
            pthread_cond_wait(&red->cambio, &red->mutex);
        }
        enlace->fd = -1;
    }
    close(fd);
    trabajador->fd = -1;
    pthread_mutex_unlock(&red->mutex);
    return rc;
}

static int esperar_reintento(dsm_red_t *red)
{
    uint64_t plazo;
    if (iniciar_plazo(red->cfg->heartbeat_intervalo_ms, &plazo) != 0) {
        return 0;
    }
    pthread_mutex_lock(&red->mutex);
    int error = 0;
    while (!red->cerrando && error == 0) {
        error = esperar_cambio(red, plazo);
    }
    int seguir = !red->cerrando && error == ETIMEDOUT;
    pthread_mutex_unlock(&red->mutex);
    return seguir;
}

static void conectar_pares(struct red_trabajador *trabajador)
{
    dsm_red_t *red = trabajador->red;
    const dsm_nodo_cfg_t *par = dsm_config_nodo(red->cfg, trabajador->indice);
    int aviso = 0;
    do {
        int fd;
        int rc = dsm_conectar_plazo(par->ipv4_host, par->puerto, red->cfg->rpc_timeout_ms, &fd);
        if (rc == 0) {
            pthread_mutex_lock(&red->mutex);
            if (red->cerrando) {
                close(fd);
                pthread_mutex_unlock(&red->mutex);
                return;
            }
            trabajador->fd = fd;
            pthread_mutex_unlock(&red->mutex);
            rc = procesar_conexion(trabajador, 1);
        }
        if (rc != 0 && !aviso) {
            pthread_mutex_lock(&red->mutex);
            int cerrando = red->cerrando;
            pthread_mutex_unlock(&red->mutex);
            if (!cerrando) {
                fprintf(stderr, "nodo %u par %u no disponible\n", red->self, par->id);
            }
        }
        aviso = rc != 0;
    } while (esperar_reintento(red));
}

static void aceptar_pares(struct red_trabajador *trabajador)
{
    dsm_red_t *red = trabajador->red;
    for (;;) {
        pthread_mutex_lock(&red->mutex);
        while (trabajador->fd < 0 && !red->cerrando) {
            pthread_cond_wait(&red->cambio, &red->mutex);
        }
        int cerrando = red->cerrando;
        pthread_mutex_unlock(&red->mutex);
        if (cerrando) {
            return;
        }
        procesar_conexion(trabajador, 0);
    }
}

static void *ejecutar_trabajador(void *arg)
{
    struct red_trabajador *trabajador = arg;
    dsm_red_t *red = trabajador->red;
    if (esperar_inicio(red)) {
        if (trabajador->indice < red->self) {
            aceptar_pares(trabajador);
        } else {
            conectar_pares(trabajador);
        }
    }
    return NULL;
}

static void *escuchar_pares(void *arg)
{
    dsm_red_t *red = arg;
    if (!esperar_inicio(red)) {
        return NULL;
    }
    struct pollfd pfd = {.fd = red->escucha, .events = POLLIN};
    for (;;) {
        int listo = poll(&pfd, 1, 100);
        pthread_mutex_lock(&red->mutex);
        int cerrando = red->cerrando;
        pthread_mutex_unlock(&red->mutex);
        if (cerrando || (listo < 0 && errno != EINTR)) {
            return NULL;
        }
        if (listo <= 0) {
            continue;
        }
        if (!(pfd.revents & POLLIN)) {
            return NULL;
        }
        int fd;
        if (dsm_aceptar(red->escucha, &fd) != 0) {
            continue;
        }
        pthread_mutex_lock(&red->mutex);
        for (uint16_t i = 0; i < red->self && !red->cerrando; i++) {
            if (red->trabajadores[i].fd < 0) {
                red->trabajadores[i].fd = fd;
                pthread_cond_broadcast(&red->cambio);
                fd = -1;
                break;
            }
        }
        if (fd >= 0) {
            close(fd);
        }
        pthread_mutex_unlock(&red->mutex);
    }
}

void dsm_red_cerrar(dsm_red_t *red)
{
    if (red == NULL) {
        return;
    }
    pthread_mutex_lock(&red->mutex);
    red->cerrando = 1;
    for (uint16_t i = 0; i < red->cfg->cantidad_nodos; i++) {
        if (red->trabajadores[i].fd >= 0) {
            shutdown(red->trabajadores[i].fd, SHUT_RDWR);
        }
    }
    pthread_cond_broadcast(&red->cambio);
    pthread_mutex_unlock(&red->mutex);

    if (red->receptor_creado) {
        pthread_join(red->receptor, NULL);
    }
    for (uint16_t i = 0; i < red->cfg->cantidad_nodos; i++) {
        struct red_trabajador *trabajador = &red->trabajadores[i];
        if (trabajador->creado) {
            pthread_join(trabajador->hilo, NULL);
        }
        if (trabajador->fd >= 0) {
            close(trabajador->fd);
        }
        free(trabajador->buf);
    }
    if (red->escucha >= 0) {
        close(red->escucha);
    }
    pthread_cond_destroy(&red->cambio);
    pthread_mutex_destroy(&red->mutex);
    free(red);
}

int dsm_red_iniciar(dsm_red_t **out, const dsm_config_t *cfg, uint16_t self,
                    dsm_red_trama_t atender, void *ctx)
{
    if (out == NULL) {
        return error_red(EINVAL);
    }
    *out = NULL;
    const dsm_nodo_cfg_t *nodo = dsm_config_nodo(cfg, self);
    if (nodo == NULL || cfg->cantidad_nodos > DSM_CONFIG_MAX_NODOS ||
        cfg->rpc_timeout_ms == 0 || cfg->heartbeat_intervalo_ms == 0 || atender == NULL) {
        return error_red(EINVAL);
    }

    dsm_red_t *red = calloc(1, sizeof *red);
    if (red == NULL) {
        return -1;
    }
    red->cfg = cfg;
    red->self = self;
    red->atender = atender;
    red->ctx = ctx;
    red->escucha = -1;
    for (uint16_t i = 0; i < cfg->cantidad_nodos; i++) {
        red->pares[i].fd = -1;
        red->trabajadores[i].fd = -1;
    }
    int error = pthread_mutex_init(&red->mutex, NULL);
    if (error != 0) {
        free(red);
        return error_red(error);
    }
    pthread_condattr_t atributos;
    error = pthread_condattr_init(&atributos);
    if (error == 0) {
        error = pthread_condattr_setclock(&atributos, CLOCK_MONOTONIC);
        if (error == 0) {
            error = pthread_cond_init(&red->cambio, &atributos);
        }
        pthread_condattr_destroy(&atributos);
    }
    if (error != 0) {
        pthread_mutex_destroy(&red->mutex);
        free(red);
        return error_red(error);
    }
    if (dsm_escuchar(nodo->ipv4_host, nodo->puerto, &red->escucha) != 0 ||
        fcntl(red->escucha, F_SETFL, O_NONBLOCK) != 0) {
        error = errno;
        goto fallo;
    }
    for (uint16_t i = 0; i < cfg->cantidad_nodos; i++) {
        if (i == self) {
            continue;
        }
        struct red_trabajador *trabajador = &red->trabajadores[i];
        trabajador->red = red;
        trabajador->indice = i;
        trabajador->buf = malloc(DSM_LONGITUD_MAX);
        if (trabajador->buf == NULL) {
            error = errno;
            goto fallo;
        }
        error = pthread_create(&trabajador->hilo, NULL, ejecutar_trabajador, trabajador);
        if (error != 0) {
            goto fallo;
        }
        trabajador->creado = 1;
    }
    error = pthread_create(&red->receptor, NULL, escuchar_pares, red);
    if (error != 0) {
        goto fallo;
    }
    red->receptor_creado = 1;
    pthread_mutex_lock(&red->mutex);
    *out = red;
    red->iniciado = 1;
    pthread_cond_broadcast(&red->cambio);
    pthread_mutex_unlock(&red->mutex);
    return 0;

fallo:
    dsm_red_cerrar(red);
    return error_red(error);
}
