#include "network.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct copia {
    int fd;
    uint8_t *datos;
    size_t total;
    int ok;
};

struct intercambio {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int listo;
    int ok0;
    int ok1;
    char texto0[768];
    char texto1[768];
    const dsm_config_t *cfg;
};

static int fallar(const char *mensaje)
{
    fprintf(stderr, "fallo: %s\n", mensaje);
    return 1;
}

static void *leer_de_a_uno(void *arg)
{
    struct copia *c = arg;
    for (size_t i = 0; i < c->total; i++) {
        uint8_t b = 0;
        ssize_t n = recv(c->fd, &b, 1, 0);
        if (n != 1) {
            c->ok = 0;
            return NULL;
        }
        c->datos[i] = b;
    }
    c->ok = 1;
    return NULL;
}

static void *enviar_de_a_uno(void *arg)
{
    struct copia *c = arg;
    for (size_t i = 0; i < c->total; i++) {
        ssize_t n = send(c->fd, c->datos + i, 1, MSG_NOSIGNAL);
        if (n != 1) {
            c->ok = 0;
            return NULL;
        }
    }
    c->ok = 1;
    return NULL;
}

static int probar_parciales(void)
{
    enum { N = 8192 };
    uint8_t origen[N];
    uint8_t destino[N];
    for (int i = 0; i < N; i++) {
        origen[i] = (uint8_t)(i * 3 + 1);
    }

    int par[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, par) != 0) {
        return fallar("socketpair");
    }
    int chico = 1024;
    setsockopt(par[0], SOL_SOCKET, SO_SNDBUF, &chico, sizeof chico);
    setsockopt(par[1], SOL_SOCKET, SO_RCVBUF, &chico, sizeof chico);
    int flags = fcntl(par[0], F_GETFL, 0);
    if (flags < 0 || fcntl(par[0], F_SETFL, flags | O_NONBLOCK) != 0) {
        close(par[0]);
        close(par[1]);
        return fallar("fcntl");
    }
    struct copia lectura = {.fd = par[1], .datos = destino, .total = N, .ok = 0};
    pthread_t hilo;
    if (pthread_create(&hilo, NULL, leer_de_a_uno, &lectura) != 0) {
        close(par[0]);
        close(par[1]);
        return fallar("hilo de lectura");
    }
    int envio = send_all(par[0], origen, N);
    if (envio != 0) {
        close(par[0]);
    }
    pthread_join(hilo, NULL);
    close(par[0]);
    close(par[1]);
    if (envio != 0 || !lectura.ok || memcmp(origen, destino, N) != 0) {
        return fallar("send_all parcial");
    }

    memset(destino, 0, N);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, par) != 0) {
        return fallar("socketpair recv");
    }
    struct copia envio_lento = {.fd = par[0], .datos = origen, .total = N, .ok = 0};
    if (pthread_create(&hilo, NULL, enviar_de_a_uno, &envio_lento) != 0) {
        close(par[0]);
        close(par[1]);
        return fallar("hilo de envio");
    }
    int recibo = recv_all(par[1], destino, N);
    pthread_join(hilo, NULL);
    close(par[0]);
    close(par[1]);
    if (recibo != 0 || !envio_lento.ok || memcmp(origen, destino, N) != 0) {
        return fallar("recv_all parcial");
    }
    printf("recv_all parcial %d\n", N);
    printf("send_all parcial %d\n", N);
    return 0;
}

static int probar_cabecera(void)
{
    dsm_cabecera_t cab;
    memset(&cab, 0, sizeof cab);
    cab.version = DSM_PROTO_VERSION;
    cab.tipo = DSM_TIPO_NODE_HELLO;
    cab.banderas = 0;
    cab.longitud = 128;
    cab.origen = 0;
    cab.destino = 1;
    cab.id_msg = 1;
    cab.id_corr = 0;
    cab.estado = DSM_ESTADO_OK;
    uint8_t raw[DSM_CABECERA_BYTES];
    if (dsm_cabecera_codificar(raw, &cab) != 0) {
        return fallar("codificar cabecera");
    }
    if (raw[0] != 0x44 || raw[1] != 0x53 || raw[2] != 0x4d || raw[3] != 0x31 || raw[4] != 1 ||
        raw[5] != DSM_TIPO_NODE_HELLO || raw[11] != 128 || raw[15] != 1 || raw[19] != 1) {
        return fallar("bytes de la cabecera");
    }
    dsm_cabecera_t vuelta;
    if (dsm_cabecera_decodificar(raw, sizeof raw, &vuelta) != 0 || vuelta.longitud != 128 ||
        vuelta.destino != 1 || vuelta.tipo != DSM_TIPO_NODE_HELLO) {
        return fallar("decodificar cabecera");
    }
    printf("cabecera big-endian 44534d31\n");
    return 0;
}

static void informar(char *dst, size_t cap, unsigned nodo, const dsm_alta_resultado_t *alta)
{
    snprintf(dst, cap,
             "nodo %u hello %d welcome %d heartbeat_ack %d estado_tras_hello %u estado %u par %u "
             "puerto %u memoria %llu base %llu nombre %s",
             nodo, alta->hello, alta->welcome, alta->heartbeat_ack, alta->estado_tras_hello,
             alta->par.estado, alta->par.id, alta->par.puerto, (unsigned long long)alta->par.memoria,
             (unsigned long long)alta->par.base, alta->par.nombre);
}

static void *nodo_servidor(void *arg)
{
    struct intercambio *ix = arg;
    const dsm_nodo_cfg_t *yo = dsm_config_nodo(ix->cfg, 1);
    int escucha = -1;
    if (yo == NULL || dsm_escuchar(yo->ipv4_host, yo->puerto, &escucha) != 0) {
        pthread_mutex_lock(&ix->mu);
        ix->listo = -1;
        pthread_cond_signal(&ix->cv);
        pthread_mutex_unlock(&ix->mu);
        return NULL;
    }
    pthread_mutex_lock(&ix->mu);
    ix->listo = 1;
    pthread_cond_signal(&ix->cv);
    pthread_mutex_unlock(&ix->mu);

    int conn = -1;
    if (dsm_aceptar(escucha, &conn) != 0) {
        close(escucha);
        return NULL;
    }
    close(escucha);
    dsm_alta_resultado_t alta;
    int rc = dsm_alta_par(conn, ix->cfg, 1, 0, &alta);
    close(conn);
    if (rc == 0) {
        informar(ix->texto1, sizeof ix->texto1, 1, &alta);
        ix->ok1 = 1;
    }
    return NULL;
}

static void *nodo_cliente(void *arg)
{
    struct intercambio *ix = arg;
    const dsm_nodo_cfg_t *par = dsm_config_nodo(ix->cfg, 1);
    pthread_mutex_lock(&ix->mu);
    while (ix->listo == 0) {
        pthread_cond_wait(&ix->cv, &ix->mu);
    }
    int listo = ix->listo;
    pthread_mutex_unlock(&ix->mu);
    if (listo < 0 || par == NULL) {
        return NULL;
    }
    int conn = -1;
    if (dsm_conectar(par->ipv4_host, par->puerto, &conn) != 0) {
        return NULL;
    }
    dsm_alta_resultado_t alta;
    int rc = dsm_alta_par(conn, ix->cfg, 0, 1, &alta);
    close(conn);
    if (rc == 0) {
        informar(ix->texto0, sizeof ix->texto0, 0, &alta);
        ix->ok0 = 1;
    }
    return NULL;
}

static int probar_nodos(const dsm_config_t *cfg)
{
    struct intercambio ix = {
        .mu = PTHREAD_MUTEX_INITIALIZER,
        .cv = PTHREAD_COND_INITIALIZER,
        .cfg = cfg,
    };
    pthread_t servidor;
    pthread_t cliente;
    if (pthread_create(&servidor, NULL, nodo_servidor, &ix) != 0) {
        return fallar("hilo nodo 1");
    }
    if (pthread_create(&cliente, NULL, nodo_cliente, &ix) != 0) {
        ix.listo = -1;
        pthread_cond_signal(&ix.cv);
        pthread_join(servidor, NULL);
        return fallar("hilo nodo 0");
    }
    pthread_join(servidor, NULL);
    pthread_join(cliente, NULL);
    if (!ix.ok0 || !ix.ok1) {
        return fallar("intercambio NODE_HELLO NODE_WELCOME heartbeat");
    }
    printf("%s\n%s\n", ix.texto0, ix.texto1);
    return 0;
}

int main(void)
{
    alarm(20);
    if (probar_cabecera() != 0 || probar_parciales() != 0) {
        return 1;
    }
    dsm_config_t cfg;
    if (dsm_config_cargar("config/lab.ini", &cfg) != 0) {
        return fallar("config");
    }
    if (probar_nodos(&cfg) != 0) {
        return 1;
    }
    alarm(0);
    return 0;
}
