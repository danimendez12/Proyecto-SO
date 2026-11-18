#include "coherence.h"

#include "network.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct pendiente {
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

struct dsm_coherencia {
    const dsm_config_t *cfg;
    uint16_t nodo;
    dsm_directorio_t *directorio;
    dsm_red_t *red;
    pthread_mutex_t mutex;
    pthread_cond_t respuesta;
    struct pendiente pendientes[DSM_CONFIG_MAX_NODOS];
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
        return EBUSY;
    case DSM_ESTADO_RANGO:
        return ENOENT;
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
    struct pendiente *p = &c->pendientes[par];
    if (p->id == 0 || p->id != cab->id_corr || p->lista) {
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
        if (c->pendientes[par].id != 0 && !c->pendientes[par].lista) {
            c->pendientes[par].error = ENOTCONN;
            c->pendientes[par].lista = 1;
            pthread_cond_broadcast(&c->respuesta);
        }
        pthread_mutex_unlock(&c->mutex);
    } else if (estado == DSM_NODO_EN_LINEA) {
        dsm_directorio_par_linea(c->directorio, par);
    }
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
    if (cab->banderas != 0 || cab->id_corr != 0 || cab->estado != DSM_ESTADO_OK) {
        return fallo(EPROTO);
    }
    if (cab->tipo == DSM_TIPO_PAGE_REQUEST || cab->tipo == DSM_TIPO_INVALIDATE ||
        cab->tipo == DSM_TIPO_PAGE_DOWNGRADE) {
        return dsm_directorio_ofrecer(c->directorio, par, buf, n);
    }
    if (cab->tipo == DSM_TIPO_REGION_LOOKUP) {
        dsm_cabecera_t pedido;
        dsm_region_lookup_t consulta;
        uint8_t respuesta[512];
        size_t bytes;
        if (dsm_region_lookup_decodificar(buf, n, &pedido, &consulta) != 0 ||
            dsm_directorio_codificar_info(c->directorio, &pedido, &consulta, nuevo_id(c),
                                         respuesta, sizeof respuesta, &bytes) != 0) {
            return fallo(EPROTO);
        }
        return enviar(c, par, respuesta, bytes);
    }
    return fallo(EPROTO);
}

static int intercambiar(dsm_coherencia_t *c, uint16_t par, const uint8_t *buf, size_t n,
                         struct pendiente *peticion)
{
    if (!vigente(&peticion->plazo)) {
        return fallo(ETIMEDOUT);
    }
    pthread_mutex_lock(&c->mutex);
    struct pendiente *p = &c->pendientes[par];
    if (c->cerrando || p->id != 0) {
        int error = c->cerrando ? ECANCELED : EBUSY;
        pthread_mutex_unlock(&c->mutex);
        return fallo(error);
    }
    *p = *peticion;
    pthread_mutex_unlock(&c->mutex);

    int error = 0;
    if (enviar(c, par, buf, n) != 0) {
        error = errno;
    } else if (peticion->tipo == DSM_TIPO_PAGE_DATA) {
        printf("nodo %u PAGE_REQUEST enviado hogar %u bytes %zu id %" PRIu32 "\n",
               c->nodo, par, n, peticion->id);
    } else if (peticion->tipo == DSM_TIPO_INVALIDATE_ACK) {
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
        peticion->bytes = p->bytes;
    }
    *p = (struct pendiente){0};
    pthread_mutex_unlock(&c->mutex);
    return error == 0 ? 0 : fallo(error);
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
    uint8_t envio[40], respuesta[512];
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

int dsm_coherencia_resolver(const dsm_pager_peticion_t *peticion, void *contexto)
{
    dsm_coherencia_t *c = contexto;
    if (c == NULL || peticion == NULL || peticion->solicitante != c->nodo ||
        (peticion->acceso != DSM_FALLO_LECTURA && peticion->acceso != DSM_FALLO_ESCRITURA)) {
        return fallo(EINVAL);
    }
    printf("nodo %u fallo SIGSEGV %s\n", c->nodo,
           peticion->acceso == DSM_FALLO_ESCRITURA ? "escritura" : "lectura");
    dsm_pagina_desc_t pagina;
    if (dsm_directorio_describir(c->directorio, peticion->pagina.gpn, &pagina) != 0 ||
        pagina.reserva != peticion->pagina.reserva_id || pagina.perdida) {
        return fallo(EPROTO);
    }
    pagina.msi = estado_local(c, &pagina);
    if (pagina.msi == DSM_MSI_M ||
        (pagina.msi == DSM_MSI_S && peticion->acceso == DSM_FALLO_LECTURA)) {
        return 0;
    }
    dsm_cabecera_t cab = cabecera(c, DSM_TIPO_PAGE_REQUEST, pagina.hogar);
    dsm_page_request_t pedido = {
        .gpn = pagina.gpn, .proc_id = ((uint32_t)c->nodo << 16) | 1u,
        .acceso = peticion->acceso == DSM_FALLO_ESCRITURA ?
                  DSM_ACCESO_ESCRITURA : DSM_ACCESO_LECTURA,
        .msi = pagina.msi,
    };
    uint8_t envio[DSM_PAGE_REQUEST_BYTES];
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
    *salida = c;
    return 0;

fallo_cond:
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
    for (size_t i = 0; i < DSM_CONFIG_MAX_NODOS; i++) {
        if (c->pendientes[i].id != 0) {
            c->pendientes[i].error = ECANCELED;
            c->pendientes[i].lista = 1;
        }
    }
    pthread_cond_broadcast(&c->respuesta);
    pthread_mutex_unlock(&c->mutex);
    dsm_directorio_desactivar(c->directorio);
    dsm_red_cerrar(c->red);
    pthread_cond_destroy(&c->respuesta);
    pthread_mutex_destroy(&c->mutex);
    free(c);
}
