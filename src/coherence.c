#include "coherence.h"

#include "network.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>

struct pendiente {
    struct pendiente *siguiente;
    uint16_t par;
    uint32_t id;
    uint8_t tipo;
    struct timespec plazo;
    dsm_pagina_desc_t pagina;
    uint32_t proceso;
    dsm_acceso_t acceso;
    uint8_t *respuesta;
    size_t capacidad;
    size_t bytes;
    int lista;
    int error;
};

struct resolucion_local {
    uint64_t gpn;
    struct resolucion_local *siguiente;
};

struct solicitud_capacidad {
    struct solicitud_capacidad *siguiente;
    uint16_t par;
    size_t bytes;
    uint8_t trama[];
};

struct dsm_coherencia {
    const dsm_config_t *cfg;
    uint16_t nodo;
    dsm_directorio_t *directorio;
    dsm_red_t *red;
    pthread_mutex_t mutex;
    pthread_cond_t respuesta;
    struct pendiente *pendientes;
    struct resolucion_local *resolviendo;
    /* Las operaciones locales de capacidad no se ejecutan en el lector TCP.
     * Los cierres de copias usan la cola FIFO del hogar del directorio. */
    struct solicitud_capacidad *capacidad_inicio;
    struct solicitud_capacidad *capacidad_fin;
    size_t capacidad_cantidad;
    pthread_t capacidad_hilo;
    int capacidad_activa;
    uint64_t token;
    int cerrando;
};

static int fallo(int error)
{
    errno = error;
    return -1;
}

static int vigente(const struct timespec *plazo)
{
    struct timespec ahora;
    return clock_gettime(CLOCK_MONOTONIC, &ahora) == 0 &&
           (ahora.tv_sec < plazo->tv_sec ||
            (ahora.tv_sec == plazo->tv_sec && ahora.tv_nsec < plazo->tv_nsec));
}

static int plazo_nuevo(dsm_coherencia_t *c, struct timespec *plazo)
{
    if (clock_gettime(CLOCK_MONOTONIC, plazo) != 0) {
        return -1;
    }
    plazo->tv_sec += c->cfg->rpc_timeout_ms / 1000u;
    plazo->tv_nsec += (long)(c->cfg->rpc_timeout_ms % 1000u) * 1000000L;
    if (plazo->tv_nsec >= 1000000000L) {
        plazo->tv_sec++;
        plazo->tv_nsec -= 1000000000L;
    }
    return 0;
}

static int error_estado(uint16_t estado)
{
    switch (estado) {
    case DSM_ESTADO_OK:
        return 0;
    case DSM_ESTADO_PLAZO:
        return ETIMEDOUT;
    case DSM_ESTADO_FUERA:
    case DSM_ESTADO_CIERRE:
        return ENOTCONN;
    case DSM_ESTADO_COLA:
    case DSM_ESTADO_CONFLICTO:
        return EBUSY;
    case DSM_ESTADO_RANGO:
        return ENOENT;
    case DSM_ESTADO_SIN_TRAMO:
        return ENOSPC;
    default:
        return EPROTO;
    }
}

static uint8_t estado_local(const dsm_coherencia_t *c, const dsm_pagina_desc_t *pagina)
{
    if (!(pagina->mascara_copias & (UINT64_C(1) << c->nodo))) {
        return DSM_MSI_I;
    }
    return pagina->msi;
}

static uint32_t nuevo_id(void *ctx)
{
    return dsm_red_id(((dsm_coherencia_t *)ctx)->red);
}

static dsm_cabecera_t cabecera(dsm_coherencia_t *c, uint8_t tipo, uint16_t destino)
{
    return (dsm_cabecera_t){
        .version = DSM_PROTO_VERSION,
        .tipo = tipo,
        .origen = c->nodo,
        .destino = destino,
        .id_msg = nuevo_id(c),
    };
}

static int despachar(void *ctx, uint16_t par, const dsm_cabecera_t *cab,
                     const uint8_t *buf, size_t n);

static int enviar(void *ctx, uint16_t destino, const uint8_t *buf, size_t n)
{
    dsm_coherencia_t *c = ctx;
    if (destino == c->nodo) {
        dsm_cabecera_t cab;
        if (dsm_cabecera_decodificar(buf, n, &cab) != 0) {
            return fallo(EPROTO);
        }
        return despachar(c, c->nodo, &cab, buf, n);
    }
    return dsm_red_enviar(c->red, destino, buf, n);
}

/* Una respuesta PAGE_DATA se instala en el lector TCP antes de entregar al
 * mismo enlace la siguiente invalidacion. El paginador solo espera el resultado.
 */
static int aplicar_respuesta(dsm_coherencia_t *c, struct pendiente *p,
                             const uint8_t *buf, size_t n)
{
    dsm_cabecera_t cab;
    dsm_page_data_t datos;
    if (dsm_page_data_decodificar(buf, n, &cab, &datos) != 0) {
        return fallo(EPROTO);
    }
    if (cab.estado != DSM_ESTADO_OK) {
        return fallo(error_estado(cab.estado));
    }
    int escritura = p->acceso == DSM_FALLO_ESCRITURA;
    uint64_t bit = UINT64_C(1) << c->nodo;
    if (datos.gpn != p->pagina.gpn || datos.reserva_id != p->pagina.reserva ||
        datos.orden != p->pagina.orden || datos.hogar != p->pagina.hogar ||
        datos.proc_id != p->proceso || datos.version < p->pagina.version ||
        (escritura && datos.version <= p->pagina.version) ||
        datos.msi != (escritura ? DSM_MSI_M : DSM_MSI_S) ||
        datos.poseedor != (escritura ? c->nodo : DSM_POSEEDOR_NINGUNO) ||
        !(datos.mascara_copias & bit) ||
        (escritura && datos.mascara_copias != bit) ||
        (datos.page_nbytes != 0 && datos.page_nbytes != c->cfg->page_size) ||
        (!escritura && datos.page_nbytes != c->cfg->page_size) ||
        (p->pagina.msi == DSM_MSI_I && datos.page_nbytes != c->cfg->page_size)) {
        return fallo(EPROTO);
    }
    if (datos.hogar != c->nodo && dsm_directorio_aplicar_pagina(c->directorio, &datos) != 0) {
        return -1;
    }
    dsm_directorio_contar(c->directorio, DSM_CONTA_PAG_REC, 1);
    dsm_directorio_contar(c->directorio, DSM_CONTA_BYTES_REC, datos.page_nbytes);
    printf("nodo %u PAGE_DATA recibido hogar %u reserva %" PRIu32 " gpn %" PRIu64
           " bytes %" PRIu32 " trama %zu corr %" PRIu32 "\n",
           c->nodo, datos.hogar, datos.reserva_id, datos.gpn,
           datos.page_nbytes, n, cab.id_corr);
    printf("nodo %u reserva %" PRIu32 " gpn %" PRIu64 " MSI %s -> %s version %" PRIu32 "\n",
           c->nodo, datos.reserva_id, datos.gpn, p->pagina.msi == DSM_MSI_I ? "I" : "S",
           escritura ? "M" : "S", datos.version);
    return 0;
}

static int recibir_respuesta(dsm_coherencia_t *c, uint16_t par,
                             const dsm_cabecera_t *cab, const uint8_t *buf, size_t n)
{
    pthread_mutex_lock(&c->mutex);
    struct pendiente *p = c->pendientes;
    while (p != NULL && (p->par != par || p->id != cab->id_corr)) {
        p = p->siguiente;
    }
    if (p == NULL || p->lista) {
        pthread_mutex_unlock(&c->mutex);
        /* Una concesion abandonada no puede convertirse en permiso local. */
        return cab->tipo == DSM_TIPO_PAGE_DATA ? fallo(EPROTO) : 0;
    }
    int error = 0;
    if (cab->tipo != p->tipo || n > p->capacidad) {
        error = EPROTO;
    } else if (!vigente(&p->plazo)) {
        error = ETIMEDOUT;
    } else if (cab->tipo == DSM_TIPO_PAGE_DATA &&
               aplicar_respuesta(c, p, buf, n) != 0) {
        error = errno;
    }
    if (error == 0 && p->respuesta != NULL) {
        memcpy(p->respuesta, buf, n);
        p->bytes = n;
    }
    p->error = error;
    p->lista = 1;
    pthread_cond_broadcast(&c->respuesta);
    pthread_mutex_unlock(&c->mutex);
    if (cab->tipo == DSM_TIPO_PAGE_DATA &&
        ((cab->estado == DSM_ESTADO_OK && error != 0) || error == EPROTO)) {
        return fallo(error);
    }
    return 0;
}

static void estado_par(void *ctx, uint16_t par, uint8_t estado)
{
    dsm_coherencia_t *c = ctx;
    if (estado == DSM_NODO_FUERA) {
        dsm_directorio_par_fuera(c->directorio, par);
        pthread_mutex_lock(&c->mutex);
        for (struct pendiente *p = c->pendientes; p != NULL; p = p->siguiente) {
            if (p->par == par && !p->lista) {
                p->error = ENOTCONN;
                p->lista = 1;
            }
        }
        pthread_cond_broadcast(&c->respuesta);
        pthread_mutex_unlock(&c->mutex);
    } else if (estado == DSM_NODO_EN_LINEA) {
        dsm_directorio_par_linea(c->directorio, par);
    }
}

static uint16_t estado_capacidad_error(int error)
{
    switch (error) {
    case ENOSPC:
    case ENOMEM:
    case EOVERFLOW: return DSM_ESTADO_SIN_TRAMO;
    case EBUSY: return DSM_ESTADO_CONFLICTO;
    case ETIMEDOUT: return DSM_ESTADO_PLAZO;
    case ENOTCONN:
    case ECONNRESET:
    case EPIPE: return DSM_ESTADO_FUERA;
    case ECANCELED: return DSM_ESTADO_CIERRE;
    case EINVAL:
    case ENOENT: return DSM_ESTADO_RANGO;
    default: return DSM_ESTADO_TRAMA;
    }
}

static int aplicar_capacidad(dsm_coherencia_t *c, uint8_t tipo, dsm_capacidad_t *msg)
{
    switch (tipo) {
    case DSM_TIPO_CAP_PREPARE:
        return dsm_directorio_preparar(c->directorio, msg->reserva_id, msg->token,
                                       msg->orden, msg->max_paginas, msg->max_tramos, &msg->info);
    case DSM_TIPO_CAP_COMMIT:
        return dsm_directorio_confirmar(c->directorio, msg->reserva_id, msg->token, &msg->info);
    case DSM_TIPO_CAP_CANCEL:
        return dsm_directorio_cancelar(c->directorio, msg->reserva_id, msg->token);
    case DSM_TIPO_CAP_RELEASE:
        return dsm_directorio_soltar(c->directorio, msg->reserva_id, msg->token);
    default:
        return fallo(EPROTO);
    }
}

static int responder_capacidad(dsm_coherencia_t *c, uint16_t par,
                               const uint8_t *buf, size_t n, int rechazo)
{
    dsm_cabecera_t pedido;
    dsm_capacidad_t msg;
    if (dsm_capacidad_decodificar(buf, n, &pedido, &msg) != 0 ||
        pedido.origen != par || pedido.destino != c->nodo ||
        (msg.reserva_id >> 16) != par || (pedido.tipo & 1u)) {
        return fallo(EPROTO);
    }
    int error = rechazo;
    if (error == 0 && aplicar_capacidad(c, pedido.tipo, &msg) != 0) {
        error = errno;
    }
    dsm_cabecera_t cab = cabecera(c, (uint8_t)(pedido.tipo + 1u), par);
    cab.banderas = DSM_FLAG_RESPUESTA;
    cab.id_corr = pedido.id_msg;
    cab.estado = error == 0 ? DSM_ESTADO_OK : estado_capacidad_error(error);
    uint8_t respuesta[DSM_CAPACIDAD_MAX];
    size_t bytes;
    if (dsm_capacidad_codificar(respuesta, sizeof respuesta, &cab, &msg, &bytes) != 0) {
        return fallo(EPROTO);
    }
    return enviar(c, par, respuesta, bytes);
}

static void *servir_capacidad(void *ctx)
{
    dsm_coherencia_t *c = ctx;
    for (;;) {
        pthread_mutex_lock(&c->mutex);
        while (!c->cerrando && c->capacidad_inicio == NULL) {
            pthread_cond_wait(&c->respuesta, &c->mutex);
        }
        if (c->cerrando) {
            pthread_mutex_unlock(&c->mutex);
            return NULL;
        }
        struct solicitud_capacidad *s = c->capacidad_inicio;
        c->capacidad_inicio = s->siguiente;
        if (c->capacidad_inicio == NULL) {
            c->capacidad_fin = NULL;
        }
        c->capacidad_cantidad--;
        pthread_mutex_unlock(&c->mutex);
        if (responder_capacidad(c, s->par, s->trama, s->bytes, 0) != 0) {
            fprintf(stderr, "nodo %u capacidad: %s\n", c->nodo, strerror(errno));
        }
        free(s);
    }
}

static int encolar_capacidad(dsm_coherencia_t *c, uint16_t par, const uint8_t *buf, size_t n)
{
    dsm_cabecera_t cab;
    dsm_capacidad_t msg;
    if (dsm_capacidad_decodificar(buf, n, &cab, &msg) != 0 ||
        (msg.reserva_id >> 16) != par || (cab.tipo & 1u)) {
        return fallo(EPROTO);
    }
    struct solicitud_capacidad *s = malloc(sizeof *s + n);
    if (s == NULL) {
        return responder_capacidad(c, par, buf, n, ENOMEM);
    }
    *s = (struct solicitud_capacidad){.par = par, .bytes = n};
    memcpy(s->trama, buf, n);
    pthread_mutex_lock(&c->mutex);
    if (c->cerrando || c->capacidad_cantidad == DSM_DIR_COLA) {
        int error = c->cerrando ? ECANCELED : EBUSY;
        pthread_mutex_unlock(&c->mutex);
        free(s);
        return responder_capacidad(c, par, buf, n, error);
    }
    if (c->capacidad_fin == NULL) {
        c->capacidad_inicio = s;
    } else {
        c->capacidad_fin->siguiente = s;
    }
    c->capacidad_fin = s;
    c->capacidad_cantidad++;
    pthread_cond_broadcast(&c->respuesta);
    pthread_mutex_unlock(&c->mutex);
    return 0;
}

static int enviar_proto_error(dsm_coherencia_t *c, uint16_t par, const dsm_cabecera_t *causa)
{
    dsm_cabecera_t cab = cabecera(c, DSM_TIPO_PROTO_ERROR, par);
    dsm_proto_error_t msg;
    memset(&msg, 0, sizeof msg);
    msg.codigo = DSM_ESTADO_TIPO;
    msg.id_causante = causa->id_msg;
    snprintf(msg.texto, sizeof msg.texto, "tipo desconocido");
    uint8_t buf[DSM_PROTO_ERROR_BYTES];
    size_t n = 0;
    if (dsm_proto_error_codificar(buf, sizeof buf, &cab, &msg, &n) != 0) {
        return 0;
    }
    fprintf(stderr, "nodo %u PROTO_ERROR par %u tipo %u id %" PRIu32 "\n",
            c->nodo, par, causa->tipo, causa->id_msg);
    (void)enviar(c, par, buf, n);
    return 0;
}

static int despachar(void *ctx, uint16_t par, const dsm_cabecera_t *cab,
                     const uint8_t *buf, size_t n)
{
    dsm_coherencia_t *c = ctx;
    if (par >= c->cfg->cantidad_nodos || cab->origen != par || cab->destino != c->nodo) {
        return fallo(EPROTO);
    }
    if (cab->banderas == DSM_FLAG_RESPUESTA && cab->id_corr != 0) {
        if (dsm_directorio_completar(c->directorio, par, buf, n) == 0) {
            return 0;
        }
        return recibir_respuesta(c, par, cab, buf, n);
    }
    if (cab->tipo == DSM_TIPO_PROTO_ERROR) {
        dsm_cabecera_t copia;
        dsm_proto_error_t err;
        if (dsm_proto_error_decodificar(buf, n, &copia, &err) == 0) {
            fprintf(stderr, "nodo %u PROTO_ERROR recibido par %u codigo %u id %" PRIu32 " %s\n",
                    c->nodo, par, err.codigo, err.id_causante, err.texto);
        }
        return 0;
    }
    if (cab->banderas != 0 || cab->id_corr != 0 || cab->estado != DSM_ESTADO_OK) {
        return fallo(EPROTO);
    }
    if (cab->tipo >= DSM_TIPO_CAP_PREPARE && cab->tipo <= DSM_TIPO_CAP_RELEASE_ACK) {
        return encolar_capacidad(c, par, buf, n);
    }
    if (cab->tipo == DSM_TIPO_PAGE_REQUEST || cab->tipo == DSM_TIPO_INVALIDATE ||
        cab->tipo == DSM_TIPO_PAGE_DOWNGRADE || cab->tipo == DSM_TIPO_REGION_CLOSE ||
        cab->tipo == DSM_TIPO_REGION_LOOKUP || cab->tipo == DSM_TIPO_REGION_COPY_CLOSE ||
        cab->tipo == DSM_TIPO_STATS_REQUEST) {
        return dsm_directorio_ofrecer(c->directorio, par, buf, n);
    }
    return enviar_proto_error(c, par, cab);
}

static int intercambiar(dsm_coherencia_t *c, uint16_t par, const uint8_t *buf, size_t n,
                         struct pendiente *peticion)
{
    if (!vigente(&peticion->plazo)) {
        return fallo(ETIMEDOUT);
    }
    pthread_mutex_lock(&c->mutex);
    if (c->cerrando) {
        pthread_mutex_unlock(&c->mutex);
        return fallo(ECANCELED);
    }
    struct pendiente *p = peticion;
    p->par = par;
    p->siguiente = c->pendientes;
    c->pendientes = p;
    pthread_mutex_unlock(&c->mutex);

    int error = 0;
    if (enviar(c, par, buf, n) != 0) {
        error = errno;
    } else if (peticion->tipo == DSM_TIPO_PAGE_DATA) {
        printf("nodo %u PAGE_REQUEST enviado hogar %u bytes %zu id %" PRIu32 "\n",
               c->nodo, par, n, peticion->id);
    } else if (peticion->tipo == DSM_TIPO_INVALIDATE_ACK) {
        dsm_directorio_contar(c->directorio, DSM_CONTA_INV_ENV, 1);
        printf("nodo %u INVALIDATE enviado par %u gpn %" PRIu64 " id %" PRIu32 "\n",
               c->nodo, par, peticion->pagina.gpn, peticion->id);
    }
    pthread_mutex_lock(&c->mutex);
    while (error == 0 && !p->lista) {
        error = pthread_cond_timedwait(&c->respuesta, &c->mutex, &peticion->plazo);
    }
    /* El lector valida el plazo antes de instalar y publica bajo este mutex.
     * Una instalacion ya confirmada prevalece al timeout de la espera del mutex.
     */
    if (error == ETIMEDOUT && p->lista) {
        error = 0;
    }
    if (error == 0) {
        error = p->error;
    }
    struct pendiente **pos = &c->pendientes;
    while (*pos != p) {
        pos = &(*pos)->siguiente;
    }
    *pos = p->siguiente;
    pthread_mutex_unlock(&c->mutex);
    return error == 0 ? 0 : fallo(error);
}

static const char *nombre_capacidad(uint8_t tipo)
{
    switch (tipo) {
    case DSM_TIPO_CAP_PREPARE: return "CAP_PREPARE";
    case DSM_TIPO_CAP_COMMIT: return "CAP_COMMIT";
    case DSM_TIPO_CAP_CANCEL: return "CAP_CANCEL";
    default: return "CAP_RELEASE";
    }
}

static int operar_capacidad(dsm_coherencia_t *c, uint16_t hogar,
                            uint8_t tipo, dsm_capacidad_t *msg)
{
    if (c == NULL || msg == NULL || hogar >= c->cfg->cantidad_nodos ||
        (uint16_t)msg->reserva_id == 0 || (msg->reserva_id >> 16) != c->nodo) {
        return fallo(EINVAL);
    }
    msg->token = c->token;
    dsm_cabecera_t cab = cabecera(c, tipo, hogar);
    uint8_t envio[DSM_CAPACIDAD_MAX], respuesta[DSM_CAPACIDAD_MAX];
    size_t n;
    if (dsm_capacidad_codificar(envio, sizeof envio, &cab, msg, &n) != 0) {
        return fallo(EINVAL);
    }
    printf("nodo %u %s hogar %u reserva %" PRIu32 " orden %" PRIu32
           " max_paginas %" PRIu32 " token %" PRIu64 "\n", c->nodo,
           nombre_capacidad(tipo), hogar, msg->reserva_id, msg->orden, msg->max_paginas, msg->token);
    int rc;
    if (hogar == c->nodo) {
        rc = aplicar_capacidad(c, tipo, msg);
    } else {
        struct pendiente p = {
            .id = cab.id_msg, .tipo = (uint8_t)(tipo + 1u),
            .respuesta = respuesta, .capacidad = sizeof respuesta,
        };
        if (plazo_nuevo(c, &p.plazo) != 0 ||
            intercambiar(c, hogar, envio, n, &p) != 0) {
            rc = -1;
        } else {
            dsm_capacidad_t confirmacion;
            dsm_cabecera_t recibida;
            if (dsm_capacidad_decodificar(respuesta, p.bytes, &recibida, &confirmacion) != 0 ||
                confirmacion.reserva_id != msg->reserva_id || confirmacion.token != msg->token ||
                confirmacion.orden != msg->orden || confirmacion.max_paginas != msg->max_paginas ||
                confirmacion.max_tramos != msg->max_tramos) {
                rc = fallo(EPROTO);
            } else if (recibida.estado != DSM_ESTADO_OK) {
                rc = fallo(error_estado(recibida.estado));
            } else {
                *msg = confirmacion;
                rc = 0;
            }
        }
    }
    int error = rc == 0 ? 0 : errno;
    printf("nodo %u %s_ACK hogar %u reserva %" PRIu32 " paginas %" PRIu32
           " tramos %" PRIu32 " resultado %d\n", c->nodo, nombre_capacidad(tipo), hogar,
           msg->reserva_id, rc == 0 && tipo == DSM_TIPO_CAP_PREPARE ? msg->info.total_paginas : 0,
           rc == 0 && tipo == DSM_TIPO_CAP_PREPARE ? msg->info.cantidad_tramos : 0, error);
    return rc == 0 ? 0 : fallo(error);
}

int dsm_coherencia_preparar(dsm_coherencia_t *c, uint16_t hogar, uint32_t id,
                            uint32_t orden, uint32_t max_pag, uint32_t max_tramos,
                            dsm_region_info_t *parcial)
{
    if (parcial == NULL) {
        return fallo(EINVAL);
    }
    dsm_capacidad_t msg = {
        .reserva_id = id, .orden = orden, .max_paginas = max_pag, .max_tramos = max_tramos,
    };
    if (operar_capacidad(c, hogar, DSM_TIPO_CAP_PREPARE, &msg) != 0) {
        return -1;
    }
    *parcial = msg.info;
    return 0;
}

int dsm_coherencia_confirmar(dsm_coherencia_t *c, uint16_t hogar, uint32_t id,
                             const dsm_region_info_t *manifiesto)
{
    if (manifiesto == NULL || manifiesto->reserva_id != id) {
        return fallo(EINVAL);
    }
    dsm_capacidad_t msg = {.reserva_id = id, .info = *manifiesto};
    return operar_capacidad(c, hogar, DSM_TIPO_CAP_COMMIT, &msg);
}

int dsm_coherencia_cancelar(dsm_coherencia_t *c, uint16_t hogar, uint32_t id)
{
    dsm_capacidad_t msg = {.reserva_id = id};
    return operar_capacidad(c, hogar, DSM_TIPO_CAP_CANCEL, &msg);
}

int dsm_coherencia_soltar(dsm_coherencia_t *c, uint16_t hogar, uint32_t id)
{
    dsm_capacidad_t msg = {.reserva_id = id};
    return operar_capacidad(c, hogar, DSM_TIPO_CAP_RELEASE, &msg);
}

int dsm_coherencia_esperar(dsm_coherencia_t *c, uint16_t par)
{
    if (c == NULL || par >= c->cfg->cantidad_nodos) {
        return fallo(EINVAL);
    }
    return par == c->nodo ? 0 : dsm_red_esperar(c->red, par);
}

int dsm_coherencia_abrir(dsm_coherencia_t *c, uint32_t reserva,
                         dsm_pagina_desc_t *pagina, dsm_vistas_t **vista)
{
    if (c == NULL || pagina == NULL || vista == NULL || (uint16_t)reserva == 0 ||
        (reserva >> 16) >= c->cfg->cantidad_nodos) {
        return fallo(EINVAL);
    }
    uint16_t creador = (uint16_t)(reserva >> 16);
    if (creador == c->nodo) {
        return dsm_directorio_vista(c->directorio, reserva, pagina, vista);
    }
    dsm_cabecera_t cab = cabecera(c, DSM_TIPO_REGION_LOOKUP, creador);
    dsm_region_lookup_t consulta = {.reserva_id = reserva};
    uint8_t envio[40], respuesta[DSM_REGION_INFO_MAX];
    size_t n;
    struct pendiente p = {
        .id = cab.id_msg, .tipo = DSM_TIPO_REGION_INFO,
        .respuesta = respuesta, .capacidad = sizeof respuesta,
    };
    if (plazo_nuevo(c, &p.plazo) != 0 ||
        dsm_region_lookup_codificar(envio, sizeof envio, &cab, &consulta, &n) != 0 ||
        intercambiar(c, creador, envio, n, &p) != 0 ||
        dsm_directorio_recibir_info(c->directorio, reserva, cab.id_msg,
                                    respuesta, p.bytes, NULL, NULL) != 0 ||
        dsm_directorio_vista(c->directorio, reserva, pagina, vista) != 0) {
        return -1;
    }
    printf("nodo %u reserva_remota %" PRIu32 " gpn %" PRIu64 " hogar %u MSI I\n",
           c->nodo, reserva, pagina->gpn, pagina->hogar);
    return 0;
}

int dsm_coherencia_asociar(dsm_coherencia_t *c, dsm_pager_t *pager, uint32_t id,
                          dsm_vistas_t *vista, size_t *bytes_logicos)
{
    dsm_region_info_t info;
    if (dsm_directorio_info(c->directorio, id, &info) != 0) {
        return -1;
    }
    if (vista->bytes / c->cfg->page_size != info.total_paginas) {
        return fallo(EPROTO);
    }
    dsm_pager_pagina_t *paginas = calloc(info.total_paginas, sizeof *paginas);
    if (paginas == NULL) {
        return -1;
    }
    /* El directorio ya valido la cobertura del manifiesto. */
    for (size_t t = 0; t < info.cantidad_tramos; t++) {
        const dsm_tramo_t *tramo = &info.tramos[t];
        for (uint32_t j = 0; j < tramo->cantidad; j++) {
            paginas[tramo->orden_inicial + j] = (dsm_pager_pagina_t){
                .reserva_id = id, .gpn = tramo->gpn_inicial + j,
            };
        }
    }
    int rc = dsm_pager_agregar_vista(pager, vista, paginas);
    free(paginas);
    if (rc == 0 && bytes_logicos != NULL) {
        *bytes_logicos = (size_t)info.tamano_bytes;
    }
    return rc;
}

static uint16_t hogar_de_gpn(const dsm_coherencia_t *c, uint64_t gpn)
{
    for (uint16_t hogar = 0; hogar < c->cfg->cantidad_nodos; hogar++) {
        uint64_t limite = dsm_config_base(c->cfg, (uint16_t)(hogar + 1u));
        if (limite != UINT64_MAX && gpn < limite / c->cfg->page_size) {
            return hogar;
        }
    }
    return c->cfg->cantidad_nodos;
}

static int cerrar_enlace(dsm_coherencia_t *c, uint16_t destino, uint32_t reserva,
                         uint8_t tipo, const struct timespec *plazo)
{
    dsm_cabecera_t cab = cabecera(c, tipo, destino);
    uint8_t envio[DSM_REGION_CIERRE_BYTES], respuesta[DSM_REGION_CIERRE_BYTES];
    struct pendiente p = {
        .id = cab.id_msg, .tipo = (uint8_t)(tipo + 1u), .plazo = *plazo,
        .respuesta = respuesta, .capacidad = sizeof respuesta,
    };
    size_t n;
    if (dsm_region_cierre_codificar(envio, sizeof envio, &cab, reserva, &n) != 0) {
        return fallo(EPROTO);
    }
    if (tipo == DSM_TIPO_REGION_COPY_CLOSE) {
        printf("nodo %u REGION_COPY_CLOSE enviado hogar %u reserva %" PRIu32 " id %" PRIu32 "\n",
               c->nodo, destino, reserva, cab.id_msg);
    }
    if (intercambiar(c, destino, envio, n, &p) != 0) {
        return -1;
    }
    uint32_t confirmada;
    if (dsm_region_cierre_decodificar(respuesta, p.bytes, &cab, &confirmada) != 0 ||
        confirmada != reserva) {
        return fallo(EPROTO);
    }
    int error = error_estado(cab.estado);
    if (tipo == DSM_TIPO_REGION_COPY_CLOSE) {
        printf("nodo %u REGION_COPY_CLOSE_ACK hogar %u reserva %" PRIu32 " resultado %d\n",
               c->nodo, destino, reserva, error);
    }
    return error == 0 ? 0 : fallo(error);
}

int dsm_coherencia_cerrar_copia(dsm_coherencia_t *c, uint16_t hogar, uint32_t reserva)
{
    if (c == NULL || hogar >= c->cfg->cantidad_nodos || (uint16_t)reserva == 0 ||
        (reserva >> 16) >= c->cfg->cantidad_nodos) {
        return fallo(EINVAL);
    }
    struct timespec plazo;
    if (plazo_nuevo(c, &plazo) != 0) {
        return -1;
    }
    /* Tambien self atraviesa la cola del hogar. El ACK es una barrera respecto
     * de todas las solicitudes PAGE_REQUEST previamente enviadas al hogar. */
    return cerrar_enlace(c, hogar, reserva, DSM_TIPO_REGION_COPY_CLOSE, &plazo);
}

int dsm_coherencia_desvincular(dsm_coherencia_t *c, uint32_t reserva)
{
    uint16_t creador = (uint16_t)(reserva >> 16);
    if (c == NULL || (uint16_t)reserva == 0 || creador == c->nodo ||
        creador >= c->cfg->cantidad_nodos) {
        return fallo(EINVAL);
    }
    dsm_region_info_t info;
    struct timespec plazo;
    memset(&info, 0, sizeof info);
    if (dsm_directorio_info(c->directorio, reserva, &info) != 0 && errno != ENOENT) {
        return -1;
    }
    if (plazo_nuevo(c, &plazo) != 0) {
        return -1;
    }
    uint64_t hogares = 0;
    for (uint32_t t = 0; t < info.cantidad_tramos; t++) {
        uint16_t hogar = hogar_de_gpn(c, info.tramos[t].gpn_inicial);
        if (hogar >= c->cfg->cantidad_nodos) {
            return fallo(EPROTO);
        }
        hogares |= UINT64_C(1) << hogar;
    }
    /* El cliente conserva su pin en el creador si cualquier hogar falla. Cada
     * cierre confirmado es idempotente y permite reintentar el conjunto. */
    for (uint16_t hogar = 0; hogar < c->cfg->cantidad_nodos; hogar++) {
        if ((hogares & (UINT64_C(1) << hogar)) &&
            cerrar_enlace(c, hogar, reserva, DSM_TIPO_REGION_COPY_CLOSE, &plazo) != 0 &&
            errno != ENOENT) {
            return -1;
        }
    }
    /* Una consulta que perdio su respuesta puede haber creado un pin sin
     * mapa local. Solo el creador confirma que ese pin ya no existe. */
    int rc = cerrar_enlace(c, creador, reserva, DSM_TIPO_REGION_CLOSE, &plazo);
    return rc == 0 || errno == ENOENT ? 0 : -1;
}

static int invalidar_hogar(dsm_coherencia_t *c, const dsm_cabecera_t *peticion,
                           const dsm_pagina_desc_t *pagina, const struct timespec *plazo)
{
    dsm_cabecera_t cab = cabecera(c, DSM_TIPO_INVALIDATE, pagina->hogar);
    dsm_invalidate_t invalidar = {
        .gpn = pagina->gpn, .id_solicitud = peticion->id_msg, .escritor = c->nodo,
    };
    uint8_t envio[DSM_CONTROL_BYTES], respuesta[DSM_CONTROL_BYTES];
    size_t n;
    struct pendiente p = {
        .id = cab.id_msg, .tipo = DSM_TIPO_INVALIDATE_ACK, .plazo = *plazo,
        .pagina = *pagina, .respuesta = respuesta, .capacidad = sizeof respuesta,
    };
    if (dsm_invalidate_codificar(envio, sizeof envio, &cab, &invalidar, &n) != 0) {
        return fallo(EPROTO);
    }
    if (intercambiar(c, pagina->hogar, envio, n, &p) != 0) {
        return -1;
    }
    dsm_cabecera_t recibida;
    dsm_invalidate_ack_t ack;
    if (dsm_invalidate_ack_decodificar(respuesta, p.bytes, &recibida, &ack) != 0 ||
        ack.gpn != pagina->gpn || ack.id_solicitud != peticion->id_msg) {
        return fallo(EPROTO);
    }
    printf("nodo %u INVALIDATE_ACK recibido par %u gpn %" PRIu64 " corr %" PRIu32 "\n",
           c->nodo, pagina->hogar, ack.gpn, recibida.id_corr);
    return recibida.estado == DSM_ESTADO_OK || recibida.estado == DSM_ESTADO_CONFLICTO ?
           0 : fallo(error_estado(recibida.estado));
}

static int resolver_sin_espera_local(const dsm_pager_peticion_t *peticion, void *contexto)
{
    dsm_coherencia_t *c = contexto;
    if (c == NULL || peticion == NULL || peticion->solicitante != c->nodo ||
        (peticion->acceso != DSM_FALLO_LECTURA && peticion->acceso != DSM_FALLO_ESCRITURA)) {
        return fallo(EINVAL);
    }
    dsm_pagina_desc_t pagina;
    if (dsm_directorio_describir(c->directorio, peticion->pagina.gpn, &pagina) != 0 ||
        pagina.reserva != peticion->pagina.reserva_id) {
        return fallo(EPROTO);
    }
    if (pagina.perdida) {
        return fallo(ENOTCONN);
    }
    pagina.msi = estado_local(c, &pagina);
    if (pagina.msi == DSM_MSI_M ||
        (pagina.msi == DSM_MSI_S && peticion->acceso == DSM_FALLO_LECTURA)) {
        return 0;
    }
    dsm_directorio_contar(c->directorio,
                          peticion->acceso == DSM_FALLO_ESCRITURA ? DSM_CONTA_ESCRITURA
                                                                  : DSM_CONTA_LECTURA,
                          1);
    /* REGION_LOOKUP solo establece que el creador está accesible.
     * El enlace con el hogar de esta página puede seguir en el alta. */
    if (pagina.hogar != c->nodo &&
        dsm_red_esperar_hasta(c->red, pagina.hogar, &peticion->plazo) != 0) {
        return -1;
    }
    dsm_cabecera_t cab = cabecera(c, DSM_TIPO_PAGE_REQUEST, pagina.hogar);
    dsm_page_request_t pedido = {
        .gpn = pagina.gpn, .proc_id = ((uint32_t)c->nodo << 16) | 1u,
        .acceso = peticion->acceso == DSM_FALLO_ESCRITURA ?
                  DSM_ACCESO_ESCRITURA : DSM_ACCESO_LECTURA,
        .msi = pagina.msi,
    };
    dsm_region_info_t info;
    if (dsm_directorio_info(c->directorio, pagina.reserva, &info) != 0) {
        return -1;
    }
    if (info.total_tramos > 1 || pagina.hogar != (pagina.reserva >> 16)) {
        pedido.reserva_id = pagina.reserva;
        pedido.orden = pagina.orden;
    }
    uint8_t envio[DSM_PAGE_REQUEST_MAP_BYTES];
    size_t n;
    if (pagina.msi == DSM_MSI_S && pedido.acceso == DSM_ACCESO_ESCRITURA &&
        pagina.hogar != c->nodo && invalidar_hogar(c, &cab, &pagina, &peticion->plazo) != 0) {
        return -1;
    }
    struct pendiente p = {
        .id = cab.id_msg, .tipo = DSM_TIPO_PAGE_DATA, .plazo = peticion->plazo,
        .pagina = pagina, .proceso = pedido.proc_id, .acceso = peticion->acceso,
        .capacidad = DSM_PAGE_DATA_BASE + c->cfg->page_size,
    };
    if (dsm_page_request_codificar(envio, sizeof envio, &cab, &pedido, &n) != 0) {
        return fallo(EPROTO);
    }
    return intercambiar(c, pagina.hogar, envio, n, &p);
}

static int resolviendo_pagina(const dsm_coherencia_t *c, uint64_t gpn)
{
    for (const struct resolucion_local *r = c->resolviendo; r != NULL; r = r->siguiente) {
        if (r->gpn == gpn) {
            return 1;
        }
    }
    return 0;
}

/* Directos y controlados comparten esta exclusion por GPN. El mutex solo
 * protege la lista y la espera local. Ningun mutex se conserva durante RPCs.
 */
static int resolver_peticion(const dsm_pager_peticion_t *peticion, void *contexto)
{
    dsm_coherencia_t *c = contexto;
    if (c == NULL || peticion == NULL) {
        return fallo(EINVAL);
    }
    pthread_mutex_lock(&c->mutex);
    int error = 0;
    while (!c->cerrando && error == 0 && resolviendo_pagina(c, peticion->pagina.gpn)) {
        error = pthread_cond_timedwait(&c->respuesta, &c->mutex, &peticion->plazo);
    }
    if (c->cerrando) {
        error = ECANCELED;
    } else if (!vigente(&peticion->plazo)) {
        error = ETIMEDOUT;
    }
    if (error != 0) {
        pthread_mutex_unlock(&c->mutex);
        return fallo(error);
    }
    struct resolucion_local r = {.gpn = peticion->pagina.gpn, .siguiente = c->resolviendo};
    c->resolviendo = &r;
    pthread_mutex_unlock(&c->mutex);

    int rc = resolver_sin_espera_local(peticion, c);
    error = errno;
    pthread_mutex_lock(&c->mutex);
    struct resolucion_local **pos = &c->resolviendo;
    while (*pos != &r) {
        pos = &(*pos)->siguiente;
    }
    *pos = r.siguiente;
    pthread_cond_broadcast(&c->respuesta);
    pthread_mutex_unlock(&c->mutex);
    return rc == 0 ? 0 : fallo(error);
}

int dsm_coherencia_resolver(const dsm_pager_peticion_t *peticion, void *contexto)
{
    dsm_coherencia_t *c = contexto;
    if (c == NULL || peticion == NULL) {
        return fallo(EINVAL);
    }
    printf("nodo %u fallo SIGSEGV %s\n", c->nodo,
           peticion->acceso == DSM_FALLO_ESCRITURA ? "escritura" : "lectura");
    return resolver_peticion(peticion, contexto);
}

int dsm_coherencia_copiar_controlado(dsm_coherencia_t *c, uint32_t reserva,
                                     uint64_t gpn, size_t offset, void *scratch, size_t bytes,
                                     int escritura, const struct timespec *plazo)
{
    if (c == NULL || plazo == NULL || scratch == NULL || (escritura != 0 && escritura != 1)) {
        return fallo(EINVAL);
    }
    dsm_pager_peticion_t peticion = {
        .pagina = {.reserva_id = reserva, .gpn = gpn},
        .solicitante = c->nodo,
        .acceso = escritura ? DSM_FALLO_ESCRITURA : DSM_FALLO_LECTURA,
        .plazo = *plazo,
    };
    for (;;) {
        if (dsm_directorio_copiar_controlado(c->directorio, reserva, gpn, offset,
                                             scratch, bytes, escritura) == 0) {
            return 0;
        }
        if (errno != EAGAIN) {
            return -1;
        }
        if (!vigente(plazo)) {
            return fallo(ETIMEDOUT);
        }
        if (resolver_peticion(&peticion, c) != 0) {
            return -1;
        }
    }
}

int dsm_coherencia_stats(dsm_coherencia_t *c, uint16_t par, uint8_t seleccion,
                         dsm_stats_reply_t *msg)
{
    if (c == NULL || msg == NULL || par >= c->cfg->cantidad_nodos ||
        (seleccion != DSM_STATS_NODOS && seleccion != DSM_STATS_PAGINAS &&
         seleccion != DSM_STATS_AMBOS)) {
        return fallo(EINVAL);
    }
    memset(msg, 0, sizeof *msg);
    dsm_cabecera_t cab = cabecera(c, DSM_TIPO_STATS_REQUEST, par);
    dsm_stats_request_t pedido = {.seleccion = seleccion};
    uint8_t envio[40];
    size_t n = 0;
    uint8_t *respuesta = malloc(DSM_LONGITUD_MAX);
    if (respuesta == NULL) {
        return -1;
    }
    struct pendiente p = {
        .id = cab.id_msg,
        .tipo = DSM_TIPO_STATS_REPLY,
        .respuesta = respuesta,
        .capacidad = DSM_LONGITUD_MAX,
    };
    int rc = -1;
    if (plazo_nuevo(c, &p.plazo) == 0 &&
        dsm_stats_request_codificar(envio, sizeof envio, &cab, &pedido, &n) == 0 &&
        intercambiar(c, par, envio, n, &p) == 0) {
        dsm_cabecera_t recibida;
        if (dsm_stats_reply_decodificar(respuesta, p.bytes, &recibida, msg) == 0 &&
            recibida.estado == DSM_ESTADO_OK && recibida.id_corr == cab.id_msg &&
            msg->emisor == par) {
            rc = 0;
        } else {
            free(msg->paginas);
            memset(msg, 0, sizeof *msg);
            rc = fallo(EPROTO);
        }
    }
    free(respuesta);
    return rc;
}

static int token_sesion(uint64_t *token)
{
    do {
        size_t completos = 0;
        while (completos < sizeof *token) {
            ssize_t n = getrandom((uint8_t *)token + completos, sizeof *token - completos, 0);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                return n < 0 ? -1 : fallo(EIO);
            }
            completos += (size_t)n;
        }
    } while (*token == 0);
    return 0;
}

static void liberar_solicitudes_capacidad(dsm_coherencia_t *c)
{
    while (c->capacidad_inicio != NULL) {
        struct solicitud_capacidad *s = c->capacidad_inicio;
        c->capacidad_inicio = s->siguiente;
        free(s);
    }
}

int dsm_coherencia_iniciar(dsm_coherencia_t **salida, const dsm_config_t *cfg,
                           uint16_t nodo, dsm_directorio_t *directorio)
{
    if (salida == NULL) {
        return fallo(EINVAL);
    }
    *salida = NULL;
    if (cfg == NULL || directorio == NULL || !directorio->inicializado ||
        directorio->nodo_id != nodo || dsm_config_nodo(cfg, nodo) == NULL) {
        return fallo(EINVAL);
    }
    dsm_coherencia_t *c = calloc(1, sizeof *c);
    if (c == NULL) {
        return -1;
    }
    c->cfg = cfg;
    c->nodo = nodo;
    c->directorio = directorio;
    if (token_sesion(&c->token) != 0) {
        free(c);
        return -1;
    }
    int error = pthread_mutex_init(&c->mutex, NULL);
    if (error != 0) {
        free(c);
        return fallo(error);
    }
    pthread_condattr_t attr;
    error = pthread_condattr_init(&attr);
    if (error == 0) {
        error = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
        if (error == 0) {
            error = pthread_cond_init(&c->respuesta, &attr);
        }
        pthread_condattr_destroy(&attr);
    }
    if (error != 0) {
        pthread_mutex_destroy(&c->mutex);
        free(c);
        return fallo(error);
    }
    if (dsm_directorio_activar(directorio, enviar, nuevo_id, c) != 0) {
        error = errno;
        goto fallo_cond;
    }
    if (dsm_red_iniciar(&c->red, cfg, nodo, despachar, c) != 0) {
        error = errno;
        dsm_directorio_desactivar(directorio);
        goto fallo_cond;
    }
    dsm_red_observar(c->red, estado_par, c);
    error = pthread_create(&c->capacidad_hilo, NULL, servir_capacidad, c);
    if (error != 0) {
        dsm_red_cerrar(c->red);
        dsm_directorio_desactivar(directorio);
        goto fallo_cond;
    }
    c->capacidad_activa = 1;
    *salida = c;
    return 0;

fallo_cond:
    liberar_solicitudes_capacidad(c);
    pthread_cond_destroy(&c->respuesta);
    pthread_mutex_destroy(&c->mutex);
    free(c);
    return fallo(error);
}

void dsm_coherencia_cerrar(dsm_coherencia_t *c)
{
    if (c == NULL) {
        return;
    }
    pthread_mutex_lock(&c->mutex);
    c->cerrando = 1;
    for (struct pendiente *p = c->pendientes; p != NULL; p = p->siguiente) {
        p->error = ECANCELED;
        p->lista = 1;
    }
    pthread_cond_broadcast(&c->respuesta);
    pthread_mutex_unlock(&c->mutex);
    if (c->capacidad_activa) {
        pthread_join(c->capacidad_hilo, NULL);
    }
    dsm_directorio_desactivar(c->directorio);
    dsm_red_cerrar(c->red);
    liberar_solicitudes_capacidad(c);
    pthread_cond_destroy(&c->respuesta);
    pthread_mutex_destroy(&c->mutex);
    free(c);
}
