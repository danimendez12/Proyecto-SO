#include "config.h"
#include "directory.h"
#include "network.h"
#include "pager.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char saludo[] = "Hola TEC";

struct pendiente {
    uint32_t id;
    uint8_t tipo;
    uint8_t *respuesta;
    size_t capacidad;
    size_t bytes;
    int lista;
    int error;
};

struct servicio {
    const dsm_config_t *cfg;
    uint16_t id;
    dsm_directorio_t directorio;
    dsm_red_t *red;
    pthread_mutex_t mutex;
    pthread_cond_t respuesta;
    struct pendiente pendientes[DSM_CONFIG_MAX_NODOS];
};

struct lectura {
    struct servicio *servicio;
    dsm_pagina_desc_t pagina;
    dsm_vistas_t *vista;
};

static int fallo(int error)
{
    errno = error;
    return -1;
}

static dsm_cabecera_t cabecera(struct servicio *s, uint8_t tipo, uint16_t destino)
{
    return (dsm_cabecera_t){
        .version = DSM_PROTO_VERSION,
        .tipo = tipo,
        .origen = s->id,
        .destino = destino,
        .id_msg = dsm_red_id(s->red),
    };
}

static int iniciar_pendientes(struct servicio *s)
{
    int error = pthread_mutex_init(&s->mutex, NULL);
    if (error != 0) {
        return fallo(error);
    }
    pthread_condattr_t attr;
    error = pthread_condattr_init(&attr);
    if (error == 0) {
        error = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
        if (error == 0) {
            error = pthread_cond_init(&s->respuesta, &attr);
        }
        pthread_condattr_destroy(&attr);
    }
    if (error != 0) {
        pthread_mutex_destroy(&s->mutex);
        return fallo(error);
    }
    return 0;
}

static int intercambiar(struct servicio *s, const dsm_cabecera_t *cab,
                         const uint8_t *envio, size_t n, uint8_t tipo,
                         uint8_t *respuesta, size_t capacidad, size_t *bytes)
{
    struct timespec plazo;
    if (clock_gettime(CLOCK_MONOTONIC, &plazo) != 0) {
        return -1;
    }
    uint32_t ms = s->cfg->rpc_timeout_ms;
    plazo.tv_sec += ms / 1000;
    plazo.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (plazo.tv_nsec >= 1000000000L) {
        plazo.tv_sec++;
        plazo.tv_nsec -= 1000000000L;
    }

    struct pendiente *p = &s->pendientes[cab->destino];
    pthread_mutex_lock(&s->mutex);
    if (p->id != 0) {
        pthread_mutex_unlock(&s->mutex);
        return fallo(EBUSY);
    }
    *p = (struct pendiente){
        .id = cab->id_msg, .tipo = tipo, .respuesta = respuesta, .capacidad = capacidad,
    };
    pthread_mutex_unlock(&s->mutex);

    int error = 0;
    if (dsm_red_enviar(s->red, cab->destino, envio, n) != 0) {
        error = errno;
    } else if (cab->tipo == DSM_TIPO_PAGE_REQUEST) {
        printf("nodo %u PAGE_REQUEST enviado hogar %u bytes %zu id %" PRIu32 "\n",
               s->id, cab->destino, n, cab->id_msg);
    }

    pthread_mutex_lock(&s->mutex);
    while (error == 0 && !p->lista) {
        error = pthread_cond_timedwait(&s->respuesta, &s->mutex, &plazo);
    }
    if (error == 0) {
        error = p->error;
        *bytes = p->bytes;
    }
    *p = (struct pendiente){0};
    pthread_mutex_unlock(&s->mutex);
    return error == 0 ? 0 : fallo(error);
}

static void entregar_respuesta(struct servicio *s, uint16_t par, const dsm_cabecera_t *cab,
                                const uint8_t *buf, size_t n)
{
    pthread_mutex_lock(&s->mutex);
    struct pendiente *p = &s->pendientes[par];
    if (p->id != 0 && p->id == cab->id_corr && !p->lista) {
        if (cab->tipo != p->tipo || n > p->capacidad) {
            p->error = EPROTO;
        } else {
            memcpy(p->respuesta, buf, n);
            p->bytes = n;
        }
        p->lista = 1;
        pthread_cond_broadcast(&s->respuesta);
    }
    pthread_mutex_unlock(&s->mutex);
}

static int responder_pagina(struct servicio *s, uint16_t par, const uint8_t *buf, size_t n)
{
    dsm_cabecera_t pedido_cab;
    dsm_page_request_t pedido;
    if (dsm_page_request_decodificar(buf, n, &pedido_cab, &pedido) != 0 ||
        (pedido.proc_id >> 16) != par || (uint16_t)pedido.proc_id == 0) {
        return fallo(EPROTO);
    }
    printf("nodo %u PAGE_REQUEST recibido par %u gpn %" PRIu64 " bytes %zu id %" PRIu32 "\n",
           s->id, par, pedido.gpn, n, pedido_cab.id_msg);

    dsm_cabecera_t cab = cabecera(s, DSM_TIPO_PAGE_DATA, par);
    cab.banderas = DSM_FLAG_RESPUESTA;
    cab.id_corr = pedido_cab.id_msg;
    dsm_page_data_t datos;
    int cambio = 0;
    if (dsm_directorio_servir_pagina(&s->directorio, par, &pedido, &datos, &cambio) != 0) {
        cab.estado = DSM_ESTADO_RANGO;
    } else if (cambio) {
        printf("nodo %u reserva %" PRIu32 " gpn %" PRIu64 " MSI M -> S version %" PRIu32 "\n",
               s->id, datos.reserva_id, datos.gpn, datos.version);
    }

    uint8_t respuesta[DSM_PAGE_DATA_BASE + s->cfg->page_size];
    size_t bytes;
    if (dsm_page_data_codificar(respuesta, sizeof respuesta, &cab,
                               cab.estado == DSM_ESTADO_OK ? &datos : NULL, &bytes) != 0 ||
        dsm_red_enviar(s->red, par, respuesta, bytes) != 0) {
        return -1;
    }
    if (cab.estado == DSM_ESTADO_OK) {
        printf("nodo %u PAGE_DATA enviado par %u reserva %" PRIu32 " gpn %" PRIu64
               " bytes %" PRIu32 " trama %zu corr %" PRIu32 "\n",
               s->id, par, datos.reserva_id, datos.gpn, datos.page_nbytes, bytes, cab.id_corr);
    }
    return 0;
}

static int despachar(void *ctx, uint16_t par, const dsm_cabecera_t *cab,
                      const uint8_t *buf, size_t n)
{
    struct servicio *s = ctx;
    if (cab->origen != par || cab->destino != s->id) {
        return fallo(EPROTO);
    }
    if (cab->banderas == DSM_FLAG_RESPUESTA && cab->id_corr != 0) {
        entregar_respuesta(s, par, cab, buf, n);
        return 0;
    }
    if (cab->banderas != 0 || cab->id_corr != 0 || cab->estado != DSM_ESTADO_OK) {
        return fallo(EPROTO);
    }
    if (cab->tipo == DSM_TIPO_PAGE_REQUEST) {
        return responder_pagina(s, par, buf, n);
    }
    if (cab->tipo == DSM_TIPO_REGION_LOOKUP) {
        dsm_cabecera_t pedido;
        dsm_region_lookup_t consulta;
        uint8_t respuesta[512];
        size_t bytes;
        if (dsm_region_lookup_decodificar(buf, n, &pedido, &consulta) != 0 ||
            dsm_directorio_codificar_info(&s->directorio, &pedido, &consulta,
                                           dsm_red_id(s->red), respuesta, sizeof respuesta,
                                           &bytes) != 0) {
            return -1;
        }
        return dsm_red_enviar(s->red, par, respuesta, bytes);
    }
    return fallo(EPROTO);
}

static int abrir_reserva(struct servicio *s, uint32_t reserva, struct lectura *lectura)
{
    uint16_t creador = (uint16_t)(reserva >> 16);
    dsm_cabecera_t cab = cabecera(s, DSM_TIPO_REGION_LOOKUP, creador);
    dsm_region_lookup_t consulta = {.reserva_id = reserva};
    uint8_t envio[40], respuesta[512];
    size_t n, bytes;
    if (dsm_region_lookup_codificar(envio, sizeof envio, &cab, &consulta, &n) != 0) {
        return fallo(EPROTO);
    }
    if (intercambiar(s, &cab, envio, n, DSM_TIPO_REGION_INFO,
                     respuesta, sizeof respuesta, &bytes) != 0 ||
        dsm_directorio_recibir_info(&s->directorio, reserva, cab.id_msg,
                                    respuesta, bytes, NULL, NULL) != 0 ||
        dsm_directorio_vista(&s->directorio, reserva, &lectura->pagina, &lectura->vista) != 0) {
        return -1;
    }
    if (lectura->pagina.msi != DSM_MSI_I) {
        return fallo(EPROTO);
    }
    lectura->servicio = s;
    printf("nodo %u reserva_remota %" PRIu32 " gpn %" PRIu64 " hogar %u MSI I\n",
           s->id, reserva, lectura->pagina.gpn, lectura->pagina.hogar);
    return 0;
}

static int traer_pagina(void *pagina_cr, size_t indice, size_t bytes, void *arg)
{
    struct lectura *lectura = arg;
    struct servicio *s = lectura->servicio;
    const dsm_pagina_desc_t *pagina = &lectura->pagina;
    if (indice != 0 || pagina_cr != lectura->vista->c_r || bytes != s->cfg->page_size) {
        return fallo(EINVAL);
    }

    dsm_cabecera_t cab = cabecera(s, DSM_TIPO_PAGE_REQUEST, pagina->hogar);
    dsm_page_request_t pedido = {
        .gpn = pagina->gpn, .proc_id = ((uint32_t)s->id << 16) | 1u, .msi = DSM_MSI_I,
    };
    uint8_t envio[48], respuesta[DSM_PAGE_DATA_BASE + bytes];
    size_t n, recibidos;
    if (dsm_page_request_codificar(envio, sizeof envio, &cab, &pedido, &n) != 0) {
        return fallo(EPROTO);
    }
    if (intercambiar(s, &cab, envio, n, DSM_TIPO_PAGE_DATA,
                     respuesta, sizeof respuesta, &recibidos) != 0) {
        perror("PAGE_REQUEST");
        return -1;
    }
    dsm_cabecera_t recibida;
    dsm_page_data_t datos;
    if (dsm_page_data_decodificar(respuesta, recibidos, &recibida, &datos) != 0 ||
        recibida.estado != DSM_ESTADO_OK || recibida.id_corr != cab.id_msg ||
        recibida.origen != pagina->hogar || recibida.destino != s->id ||
        datos.proc_id != pedido.proc_id ||
        dsm_directorio_compartir_pagina(&s->directorio, pagina->reserva, &datos) != 0) {
        fprintf(stderr, "PAGE_DATA invalido para la reserva %" PRIu32 "\n", pagina->reserva);
        return fallo(EPROTO);
    }
    printf("nodo %u PAGE_DATA recibido hogar %u reserva %" PRIu32 " gpn %" PRIu64
           " bytes %" PRIu32 " trama %zu corr %" PRIu32 "\n",
           s->id, datos.hogar, datos.reserva_id, datos.gpn,
           datos.page_nbytes, recibidos, recibida.id_corr);
    printf("nodo %u reserva %" PRIu32 " gpn %" PRIu64 " MSI I -> S version %" PRIu32 "\n",
           s->id, datos.reserva_id, datos.gpn, datos.version);
    return 0;
}

static int leer_reserva(struct servicio *s, uint32_t reserva)
{
    struct lectura lectura;
    if (abrir_reserva(s, reserva, &lectura) != 0) {
        return -1;
    }
    dsm_pager_t *pager;
    if (dsm_pager_iniciar(&pager, lectura.vista, s->cfg->page_size, 1,
                          traer_pagina, &lectura) != 0) {
        return -1;
    }
    int error = 0;
    if (dsm_pager_registrar(pager) != 0) {
        error = errno;
    } else {
        const volatile char *ar = lectura.vista->a_r;
        for (int intento = 0; intento < 2; intento++) {
            char texto[sizeof saludo + 1] = {0};
            for (size_t i = 0; i < sizeof saludo; i++) {
                texto[i] = ar[i];
            }
            printf("nodo %u %s reserva %" PRIu32 " gpn %" PRIu64 " resultado %s\n",
                   s->id, intento == 0 ? "lectura" : "segunda lectura",
                   reserva, lectura.pagina.gpn, texto);
            if (memcmp(texto, saludo, sizeof saludo) != 0) {
                error = EPROTO;
                break;
            }
            if (intento == 0) {
                /* El creador escribe el saludo sobre una pagina inicializada en cero. */
                for (size_t i = sizeof saludo; i < lectura.vista->bytes; i++) {
                    if (ar[i] != 0) {
                        error = EPROTO;
                        break;
                    }
                }
                if (error != 0) {
                    break;
                }
                printf("nodo %u imagen verificada %zu bytes\n", s->id, lectura.vista->bytes);
            }
        }
        if (dsm_pager_desregistrar(pager) != 0) {
            perror("dsm_pager_desregistrar");
            exit(EXIT_FAILURE);
        }
    }
    if (dsm_pager_cerrar(pager) != 0) {
        perror("dsm_pager_cerrar");
        exit(EXIT_FAILURE);
    }
    return error == 0 ? 0 : fallo(error);
}

/* SIGINT se consume en el hilo principal y se mantiene bloqueada en los demas. */
static int esperar_lector(struct servicio *s, uint16_t par, const sigset_t *interrupcion)
{
    const struct timespec inmediato = {0};
    for (;;) {
        if (sigtimedwait(interrupcion, NULL, &inmediato) == SIGINT) {
            return 1;
        }
        if (dsm_red_esperar(s->red, par) == 0) {
            return sigtimedwait(interrupcion, NULL, &inmediato) == SIGINT;
        }
        if (errno != ETIMEDOUT) {
            return -1;
        }
    }
}

static int servir(const dsm_config_t *cfg, const dsm_nodo_cfg_t *nodo, uint32_t reserva)
{
    sigset_t interrupcion;
    sigemptyset(&interrupcion);
    sigaddset(&interrupcion, SIGINT);
    int error = pthread_sigmask(SIG_BLOCK, &interrupcion, NULL);
    struct sigaction accion = {.sa_handler = SIG_DFL};
    sigemptyset(&accion.sa_mask);
    if (error != 0 || sigaction(SIGINT, &accion, NULL) != 0) {
        fprintf(stderr, "no se pudo preparar SIGINT\n");
        return 1;
    }

    struct servicio s = {.cfg = cfg, .id = nodo->id};
    if (dsm_directorio_init(&s.directorio, cfg, nodo->id) != 0) {
        fprintf(stderr, "no se pudo iniciar el directorio del nodo %u\n", nodo->id);
        return 1;
    }
    int rc = 1;
    dsm_pagina_desc_t local;
    if (dsm_directorio_reservar_pagina(&s.directorio, &local) != 0) {
        fprintf(stderr, "no se pudo crear la reserva local del nodo %u\n", nodo->id);
        goto cerrar_directorio;
    }
    memcpy(local.ubicacion_local, saludo, sizeof saludo);
    if (iniciar_pendientes(&s) != 0) {
        perror("iniciar solicitudes");
        goto cerrar_directorio;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (dsm_red_iniciar(&s.red, cfg, nodo->id, despachar, &s) != 0) {
        perror("iniciar red");
        goto cerrar_pendientes;
    }
    printf("nodo %u escuchando puerto %u reserva_local %" PRIu32 " gpn %" PRIu64 " MSI M\n",
           nodo->id, nodo->puerto, local.reserva, local.gpn);

    rc = 0;
    int espera = reserva == 0 ? 0 : esperar_lector(&s, (uint16_t)(reserva >> 16), &interrupcion);
    if (espera <= 0) {
        if (espera < 0 || (reserva != 0 && leer_reserva(&s, reserva) != 0)) {
            perror("lectura remota");
            rc = 1;
        }
        int signo;
        error = sigwait(&interrupcion, &signo);
        if (error != 0) {
            fprintf(stderr, "sigwait: %s\n", strerror(error));
            rc = 1;
        }
    }
    dsm_red_cerrar(s.red);
    printf("nodo %u servicio detenido\n", nodo->id);

cerrar_pendientes:
    pthread_cond_destroy(&s.respuesta);
    pthread_mutex_destroy(&s.mutex);
cerrar_directorio:
    dsm_directorio_cerrar(&s.directorio);
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
    printf("memoria %llu\n", (unsigned long long)nodo->memoria);
    return 0;
}
