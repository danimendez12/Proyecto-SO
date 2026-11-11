#include "network.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
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
    uint64_t base_esperada = (uint64_t)cx->peer * cx->cfg->memoria_inicial;
    if (esperado == NULL || id != cx->peer || puerto != esperado->puerto ||
        ip != esperado->ipv4_host || memoria != cx->cfg->memoria_inicial ||
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
    msg.memoria = cx->cfg->memoria_inicial;
    msg.base = (uint64_t)yo->id * cx->cfg->memoria_inicial;
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
    uint32_t totales = (uint32_t)(cx->cfg->memoria_inicial / cx->cfg->page_size);
    dsm_welcome_t msg;
    memset(&msg, 0, sizeof msg);
    msg.emisor = yo->id;
    msg.n = 1;
    msg.epoca = 1;
    msg.fichas[0].id = yo->id;
    msg.fichas[0].estado = DSM_NODO_UNION;
    msg.fichas[0].ipv4_host = yo->ipv4_host;
    msg.fichas[0].puerto = yo->puerto;
    msg.fichas[0].memoria = cx->cfg->memoria_inicial;
    msg.fichas[0].base = (uint64_t)yo->id * cx->cfg->memoria_inicial;
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
    uint32_t totales = (uint32_t)(cx->cfg->memoria_inicial / cx->cfg->page_size);
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
