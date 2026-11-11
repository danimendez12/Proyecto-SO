#include "config.h"
#include "directory.h"
#include "network.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct enlace {
    int fd;
    uint32_t siguiente_msg;
    uint32_t consulta;
    uint64_t vence;
    uint64_t reintentar;
    int aviso;
};

struct servicio {
    const dsm_config_t *cfg;
    const dsm_nodo_cfg_t *nodo;
    dsm_directorio_t directorio;
    uint32_t reserva_remota;
    struct enlace pares[DSM_CONFIG_MAX_NODOS];
};

static volatile sig_atomic_t detener;

static void parar(int signo)
{
    (void)signo;
    detener = 1;
}

static uint64_t ahora_ms(void)
{
    struct timespec ts = {0};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void cerrar_par(struct servicio *s, uint16_t par)
{
    close(s->pares[par].fd);
    s->pares[par] = (struct enlace){
        .fd = -1,
        .reintentar = ahora_ms() + s->cfg->heartbeat_intervalo_ms,
    };
}

static void registrar_par(struct servicio *s, int fd, const dsm_alta_resultado_t *alta)
{
    uint16_t par = alta->par.id;
    struct enlace *enlace = &s->pares[par];
    if (detener || enlace->fd >= 0) {
        close(fd);
        return;
    }
    *enlace = (struct enlace){.fd = fd, .siguiente_msg = alta->siguiente_msg};
    printf("nodo %u par %u hello %d welcome %d estado %u\n",
           s->nodo->id, par, alta->hello, alta->welcome, alta->par.estado);
    if (s->reserva_remota && (s->reserva_remota >> 16) == par) {
        enlace->vence = ahora_ms() + s->cfg->rpc_timeout_ms;
        if (dsm_directorio_solicitar(&s->directorio, fd, s->reserva_remota,
                                    &enlace->siguiente_msg, &enlace->consulta) != 0) {
            fprintf(stderr, "nodo %u no pudo enviar REGION_LOOKUP al par %u\n", s->nodo->id, par);
            cerrar_par(s, par);
            return;
        }
    }
}

static void aceptar_par(struct servicio *s, int escucha)
{
    int fd;
    dsm_alta_resultado_t alta;
    if (dsm_aceptar(escucha, &fd) != 0) {
        if (!detener && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            perror("aceptar");
        }
        return;
    }
    if (detener) {
        close(fd);
        return;
    }
    if (dsm_alta_aceptada(fd, s->cfg, s->nodo->id, &alta) != 0) {
        if (!detener) {
            perror("alta entrante");
        }
        close(fd);
        return;
    }
    registrar_par(s, fd, &alta);
}

static void conectar_par(struct servicio *s)
{
    uint64_t ahora = ahora_ms();
    for (uint16_t id = s->nodo->id + 1; id < s->cfg->cantidad_nodos; id++) {
        struct enlace *enlace = &s->pares[id];
        if (enlace->fd >= 0 || ahora < enlace->reintentar) {
            continue;
        }
        const dsm_nodo_cfg_t *par = dsm_config_nodo(s->cfg, id);
        dsm_alta_resultado_t alta;
        int fd = -1;
        if (dsm_conectar_plazo(par->ipv4_host, par->puerto, s->cfg->rpc_timeout_ms, &fd) == 0 &&
            !detener && dsm_alta_par(fd, s->cfg, s->nodo->id, id, &alta) == 0) {
            registrar_par(s, fd, &alta);
        } else {
            if (!enlace->aviso && !detener) {
                fprintf(stderr, "nodo %u par %u no disponible: %s\n",
                        s->nodo->id, id, strerror(errno));
            }
            if (fd >= 0) {
                close(fd);
            }
            enlace->aviso = 1;
            enlace->reintentar = ahora_ms() + s->cfg->heartbeat_intervalo_ms;
        }
        return;
    }
}

static void vencer_consulta(struct servicio *s, uint16_t par)
{
    struct enlace *enlace = &s->pares[par];
    if (enlace->consulta && ahora_ms() >= enlace->vence) {
        enlace->consulta = 0;
        fprintf(stderr, "nodo %u plazo REGION_INFO vencido para par %u\n", s->nodo->id, par);
    }
}

static int despachar(struct servicio *s, uint16_t par)
{
    struct enlace *enlace = &s->pares[par];
    uint8_t buf[512];
    size_t n;
    dsm_cabecera_t cab;
    uint32_t plazo = s->cfg->rpc_timeout_ms;
    if (enlace->consulta) {
        uint64_t ahora = ahora_ms();
        if (ahora < enlace->vence && enlace->vence - ahora < plazo) {
            plazo = (uint32_t)(enlace->vence - ahora);
        }
    }
    if (dsm_recibir_trama_plazo(enlace->fd, buf, sizeof buf, &n, plazo) != 0 ||
        dsm_cabecera_decodificar(buf, n, &cab) != 0 ||
        cab.origen != par || cab.destino != s->nodo->id) {
        return -1;
    }

    if (cab.tipo == DSM_TIPO_REGION_LOOKUP) {
        dsm_region_lookup_t consulta;
        if (dsm_region_lookup_decodificar(buf, n, &cab, &consulta) != 0) {
            return -1;
        }
        return dsm_directorio_responder(&s->directorio, enlace->fd, par, &cab, &consulta,
                                        &enlace->siguiente_msg);
    }

    if (cab.tipo == DSM_TIPO_REGION_INFO && cab.banderas == DSM_FLAG_RESPUESTA) {
        vencer_consulta(s, par);
        if (!enlace->consulta) {
            return 0;
        }
        dsm_region_info_t info;
        uint32_t consulta = enlace->consulta;
        enlace->consulta = 0;
        if (dsm_directorio_recibir_info(&s->directorio, s->reserva_remota, consulta,
                                        buf, n, NULL, &info) != 0) {
            int error = errno;
            fprintf(stderr, "nodo %u no pudo abrir reserva %" PRIu32 " del par %u: %s\n",
                    s->nodo->id, s->reserva_remota, par, strerror(error));
            return error == EPROTO ? -1 : 0;
        }
        printf("nodo %u reserva_remota %" PRIu32 " gpn %" PRIu64 " hogar %u\n",
               s->nodo->id, info.reserva_id, info.tramos[0].gpn_inicial, par);
        return 0;
    }

    return -1;
}

static int atender(struct servicio *s, int escucha)
{
    struct pollfd fds[DSM_CONFIG_MAX_NODOS + 1];
    fds[0] = (struct pollfd){.fd = escucha, .events = POLLIN};
    while (!detener) {
        for (uint16_t i = 0; i < s->cfg->cantidad_nodos; i++) {
            fds[i + 1] = (struct pollfd){.fd = s->pares[i].fd, .events = POLLIN};
        }
        if (poll(fds, 1u + s->cfg->cantidad_nodos, 100) < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            return -1;
        }
        for (uint16_t i = 0; i < s->cfg->cantidad_nodos && !detener; i++) {
            vencer_consulta(s, i);
            short eventos = fds[i + 1].revents;
            if (eventos && (!(eventos & POLLIN) || despachar(s, i) != 0)) {
                if (!detener) {
                    fprintf(stderr, "nodo %u desconectado del par %u\n", s->nodo->id, i);
                }
                cerrar_par(s, i);
            }
        }
        if (!detener && (fds[0].revents & POLLIN)) {
            aceptar_par(s, escucha);
        } else if (!detener && fds[0].revents) {
            fprintf(stderr, "nodo %u perdio el socket de escucha\n", s->nodo->id);
            return -1;
        }
        if (!detener) {
            conectar_par(s);
        }
    }
    return 0;
}

static int servir(const dsm_config_t *cfg, const dsm_nodo_cfg_t *nodo, uint32_t reserva_remota)
{
    struct sigaction accion = {.sa_handler = parar};
    sigemptyset(&accion.sa_mask);
    if (sigaction(SIGINT, &accion, NULL) != 0) {
        perror("sigaction");
        return 1;
    }

    struct servicio s = {.cfg = cfg, .nodo = nodo, .reserva_remota = reserva_remota};
    for (uint16_t i = 0; i < cfg->cantidad_nodos; i++) {
        s.pares[i].fd = -1;
    }
    if (dsm_directorio_init(&s.directorio, cfg, nodo->id) != 0) {
        fprintf(stderr, "no se pudo iniciar el directorio del nodo %u\n", nodo->id);
        return 1;
    }

    int escucha = -1;
    int rc = 1;
    dsm_pagina_desc_t local;
    if (dsm_directorio_reservar_pagina(&s.directorio, &local) != 0) {
        fprintf(stderr, "no se pudo crear la reserva local del nodo %u\n", nodo->id);
    } else if (dsm_escuchar(nodo->ipv4_host, nodo->puerto, &escucha) != 0 ||
               fcntl(escucha, F_SETFL, O_NONBLOCK) != 0) {
        perror("escuchar");
    } else {
        setvbuf(stdout, NULL, _IOLBF, 0);
        printf("nodo %u escuchando puerto %u reserva_local %" PRIu32 " gpn %" PRIu64 "\n",
               nodo->id, nodo->puerto, local.reserva, local.gpn);
        rc = atender(&s, escucha) != 0;
    }

    for (uint16_t i = 0; i < cfg->cantidad_nodos; i++) {
        if (s.pares[i].fd >= 0) {
            close(s.pares[i].fd);
        }
    }
    if (escucha >= 0) {
        close(escucha);
    }
    dsm_directorio_cerrar(&s.directorio);
    if (rc == 0) {
        printf("nodo %u servicio detenido\n", nodo->id);
    }
    return rc;
}

static void uso(const char *argv0)
{
    fprintf(stderr, "uso: %s <config> <id-nodo> [servir [id-reserva-remota]]\n", argv0);
}

int main(int argc, char **argv)
{
    if (argc < 3 || argc > 5 || (argc >= 4 && strcmp(argv[3], "servir") != 0)) {
        uso(argv[0]);
        return 1;
    }
    dsm_config_t cfg;
    if (dsm_config_cargar(argv[1], &cfg) != 0) {
        return 1;
    }
    errno = 0;
    char *fin = NULL;
    unsigned long id = strtoul(argv[2], &fin, 10);
    if (errno != 0 || fin == argv[2] || *fin != '\0' || id > 65535ul) {
        fprintf(stderr, "id de nodo invalido\n");
        return 1;
    }
    const dsm_nodo_cfg_t *nodo = dsm_config_nodo(&cfg, (uint16_t)id);
    if (nodo == NULL) {
        fprintf(stderr, "el nodo %lu no esta en la configuracion\n", id);
        return 1;
    }
    if (argc >= 4) {
        uint32_t reserva = 0;
        if (argc == 5) {
            errno = 0;
            unsigned long valor = strtoul(argv[4], &fin, 10);
            uint16_t creador = (uint16_t)(valor >> 16);
            if (errno || argv[4][0] < '0' || argv[4][0] > '9' || *fin != '\0' ||
                valor > UINT32_MAX || (valor & UINT16_MAX) == 0 || creador == nodo->id ||
                dsm_config_nodo(&cfg, creador) == NULL) {
                fprintf(stderr, "id de reserva remota invalido\n");
                return 1;
            }
            reserva = (uint32_t)valor;
        }
        return servir(&cfg, nodo, reserva);
    }
    printf("nodo %u\n", (unsigned)nodo->id);
    printf("puerto %u\n", (unsigned)nodo->puerto);
    printf("memoria %llu\n", (unsigned long long)cfg.memoria_inicial);
    return 0;
}
