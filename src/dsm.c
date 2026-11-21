#include "dsm.h"

#include "coherence.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct vinculacion {
    dsm_reserva_id_t id;
    void *base;
    size_t bytes;
    int retirando;
    int provisional;
    uint32_t aportantes;
    uint32_t copias_por_cerrar;
    struct vinculacion *siguiente;
};

/* Intervalos utilizados por dsm_leer/dsm_escribir. Los punteros directos
 * que utilice la aplicacion se sincronizan en la propia aplicacion. */
struct copia {
    struct vinculacion *reserva;
    size_t inicio, fin;
    int escritura;
    struct copia *siguiente;
};

struct dsm_context {
    dsm_config_t cfg;
    dsm_directorio_t directorio;
    dsm_coherencia_t *coherencia;
    dsm_pager_t *pager;
    dsm_node_id_t nodo;
    dsm_proc_id_t proceso;
    size_t trabajadores;
    struct vinculacion *reservas;
    struct copia *copias;
};

/* Los callbacks de red y paginacion nunca toman el mutex del API. */
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fin_copia = PTHREAD_COND_INITIALIZER;
static dsm_context_t *activo;
static _Thread_local int registrado;

static int salir(int rc)
{
    pthread_mutex_unlock(&mutex);
    return rc;
}

static int entrar(dsm_context_t *ctx)
{
    if (ctx == NULL) {
        return DSM_ERR_INVAL;
    }
    pthread_mutex_lock(&mutex);
    return ctx == activo ? DSM_OK : salir(DSM_ERR_CERRADO);
}

static int error_api(int error)
{
    switch (error) {
    case EINVAL:
    case ENOENT: return DSM_ERR_INVAL;
    case ENOMEM:
    case ENOSPC:
    case EOVERFLOW: return DSM_ERR_NOMEM;
    case ENOTCONN:
    case ECONNRESET:
    case EPIPE: return DSM_ERR_NODO;
    case ETIMEDOUT: return DSM_ERR_TIEMPO;
    case EBUSY: return DSM_ERR_OCUPADA;
    case EPROTO: return DSM_ERR_PROTO;
    default: return DSM_ERR_ESTADO;
    }
}

static struct vinculacion **posicion(dsm_context_t *ctx, uint32_t id)
{
    struct vinculacion **pos = &ctx->reservas;
    while (*pos != NULL && (*pos)->id != id) {
        pos = &(*pos)->siguiente;
    }
    return pos;
}

static void quitar(struct vinculacion **pos)
{
    struct vinculacion *v = *pos;
    *pos = v->siguiente;
    free(v);
}

int dsm_iniciar(const char *ruta_config, dsm_node_id_t nodo, dsm_context_t **salida)
{
    if (ruta_config == NULL || salida == NULL) {
        return DSM_ERR_INVAL;
    }
    dsm_config_t cfg;
    if (dsm_config_cargar(ruta_config, &cfg) != 0 || dsm_config_nodo(&cfg, nodo) == NULL) {
        return DSM_ERR_CONFIG;
    }
    pthread_mutex_lock(&mutex);
    if (activo != NULL) {
        return salir(DSM_ERR_ESTADO);
    }
    dsm_context_t *ctx = calloc(1, sizeof *ctx);
    if (ctx == NULL) {
        return salir(DSM_ERR_NOMEM);
    }
    ctx->cfg = cfg;
    ctx->nodo = nodo;
    if (dsm_directorio_init(&ctx->directorio, &cfg, nodo) != 0 ||
        dsm_coherencia_iniciar(&ctx->coherencia, &ctx->cfg, nodo, &ctx->directorio) != 0 ||
        dsm_pager_iniciar_vacio(&ctx->pager, cfg.page_size, cfg.max_hilos, nodo,
                                cfg.rpc_timeout_ms, dsm_coherencia_resolver, ctx->coherencia) != 0) {
        int rc = error_api(errno);
        dsm_coherencia_cerrar(ctx->coherencia);
        dsm_directorio_cerrar(&ctx->directorio);
        free(ctx);
        return salir(rc);
    }
    activo = ctx;
    *salida = ctx;
    return salir(DSM_OK);
}

int dsm_proc_abrir(dsm_context_t *ctx, dsm_proc_id_t *salida)
{
    int rc = entrar(ctx);
    if (rc != DSM_OK) {
        return rc;
    }
    if (salida == NULL) {
        return salir(DSM_ERR_INVAL);
    }
    if (ctx->proceso != 0) {
        return salir(DSM_ERR_ESTADO);
    }
    ctx->proceso = ((uint32_t)ctx->nodo << 16) | 1u;
    *salida = ctx->proceso;
    return salir(DSM_OK);
}

static int registrar(dsm_context_t *ctx)
{
    for (struct vinculacion *v = ctx->reservas; v != NULL; v = v->siguiente) {
        if (v->retirando && v->base != NULL) {
            return DSM_ERR_OCUPADA;
        }
    }
    if (dsm_pager_registrar(ctx->pager) != 0) {
        return errno == ENOSPC ? DSM_ERR_HILOS : DSM_ERR_ESTADO;
    }
    registrado = 1;
    ctx->trabajadores++;
    return DSM_OK;
}

static int desregistrar(dsm_context_t *ctx)
{
    if (!registrado || dsm_pager_desregistrar(ctx->pager) != 0) {
        return DSM_ERR_ESTADO;
    }
    registrado = 0;
    ctx->trabajadores--;
    return DSM_OK;
}

int dsm_hilo_registrar(dsm_context_t *ctx, dsm_proc_id_t proceso)
{
    int rc = entrar(ctx);
    if (rc != DSM_OK) {
        return rc;
    }
    return salir(proceso != 0 && proceso == ctx->proceso ? registrar(ctx) : DSM_ERR_INVAL);
}

int dsm_hilo_desregistrar(dsm_context_t *ctx)
{
    int rc = entrar(ctx);
    return rc == DSM_OK ? salir(desregistrar(ctx)) : rc;
}

/* El manifiesto y el respaldo ya estan validados por el directorio. */
static int asociar(dsm_context_t *ctx, struct vinculacion *v, dsm_vistas_t *vista)
{
    if (dsm_coherencia_asociar(ctx->coherencia, ctx->pager, v->id, vista, &v->bytes) != 0) {
        return error_api(errno);
    }
    v->base = vista->a_r;
    return DSM_OK;
}

/* Una respuesta perdida deja al participante en la lista hasta que confirme
 * la cancelacion. Cada hogar puede devolver capacidad independientemente. */
static int cancelar(dsm_context_t *ctx, struct vinculacion **pos)
{
    struct vinculacion *v = *pos;
    int resultado = DSM_OK;
    if (v->base != NULL) {
        if (dsm_pager_retirar_vista(ctx->pager, v->id) != 0) {
            return error_api(errno);
        }
        v->base = NULL;
    }
    for (uint16_t hogar = 0; hogar < ctx->cfg.cantidad_nodos; hogar++) {
        uint32_t bit = UINT32_C(1) << hogar;
        if (!(v->aportantes & bit)) {
            continue;
        }
        if (dsm_coherencia_cancelar(ctx->coherencia, hogar, v->id) != 0) {
            resultado = error_api(errno);
        } else {
            v->aportantes &= ~bit;
        }
    }
    if (v->aportantes == 0) {
        quitar(pos);
    }
    return resultado;
}

static void reintentar_cancelaciones(dsm_context_t *ctx)
{
    struct vinculacion **pos = &ctx->reservas;
    while (*pos != NULL) {
        if ((*pos)->provisional && cancelar(ctx, pos) == DSM_OK) {
            continue;
        }
        pos = &(*pos)->siguiente;
    }
}

static int preparar_mapa(dsm_context_t *ctx, struct vinculacion *v, dsm_region_info_t *mapa)
{
    uint32_t orden = 0;
    int inalcanzable = 0;
    /* Primero el creador, despues los demas IDs en orden ascendente. */
    for (uint16_t paso = 0; paso < ctx->cfg.cantidad_nodos && orden < mapa->total_paginas; paso++) {
        uint16_t hogar = paso == 0 ? ctx->nodo : paso - 1;
        if (paso != 0 && hogar >= ctx->nodo) {
            hogar++;
        }
        if (hogar != ctx->nodo && dsm_coherencia_esperar(ctx->coherencia, hogar) != 0) {
            inalcanzable = 1;
            continue;
        }
        uint32_t bit = UINT32_C(1) << hogar;
        v->aportantes |= bit;
        dsm_region_info_t parcial;
        uint32_t max_tramos = DSM_MAX_TRAMOS - mapa->cantidad_tramos;
        if (max_tramos > DSM_TRAMOS_POR_HOGAR) {
            max_tramos = DSM_TRAMOS_POR_HOGAR;
        }
        if (dsm_coherencia_preparar(ctx->coherencia, hogar, v->id, orden,
                                   mapa->total_paginas - orden, max_tramos, &parcial) != 0) {
            if (errno == ENOSPC) {
                if (hogar != ctx->nodo) {
                    v->aportantes &= ~bit;
                }
                continue;
            }
            return error_api(errno);
        }
        if (parcial.reserva_id != v->id || parcial.cantidad_tramos == 0 ||
            parcial.cantidad_tramos > max_tramos) {
            return DSM_ERR_PROTO;
        }
        uint64_t base = ctx->directorio.limites_gpn[hogar];
        uint64_t fin = ctx->directorio.limites_gpn[hogar + 1];
        for (uint32_t i = 0; i < parcial.cantidad_tramos; i++) {
            const dsm_tramo_t *tramo = &parcial.tramos[i];
            if (tramo->orden_inicial != orden || tramo->cantidad == 0 ||
                tramo->cantidad > mapa->total_paginas - orden ||
                tramo->gpn_inicial < base || tramo->gpn_inicial >= fin ||
                tramo->cantidad > fin - tramo->gpn_inicial) {
                return DSM_ERR_PROTO;
            }
            mapa->tramos[mapa->cantidad_tramos++] = *tramo;
            orden += tramo->cantidad;
        }
    }
    mapa->total_tramos = mapa->cantidad_tramos;
    if (orden != mapa->total_paginas) {
        return inalcanzable ? DSM_ERR_NODO : DSM_ERR_NOMEM;
    }
    return DSM_OK;
}

static int confirmar(dsm_context_t *ctx, struct vinculacion *v, const dsm_region_info_t *mapa)
{
    for (uint16_t hogar = 0; hogar < ctx->cfg.cantidad_nodos; hogar++) {
        if ((v->aportantes & (UINT32_C(1) << hogar)) &&
            dsm_coherencia_confirmar(ctx->coherencia, hogar, v->id, mapa) != 0) {
            return error_api(errno);
        }
    }
    dsm_pagina_desc_t primera;
    dsm_vistas_t *vista;
    if (dsm_directorio_vista(&ctx->directorio, v->id, &primera, &vista) != 0) {
        return error_api(errno);
    }
    int rc = asociar(ctx, v, vista);
    if (rc == DSM_OK && dsm_directorio_publicar_reserva(&ctx->directorio, v->id) != 0) {
        return error_api(errno);
    }
    return rc;
}

int dsm_reservar(dsm_context_t *ctx, dsm_proc_id_t proceso, size_t bytes,
                 dsm_reserva_id_t *id, void **base)
{
    int rc = entrar(ctx);
    if (rc != DSM_OK) {
        return rc;
    }
    if (proceso == 0 || proceso != ctx->proceso || bytes == 0 || id == NULL || base == NULL) {
        return salir(DSM_ERR_INVAL);
    }
    if (bytes > dsm_config_base(&ctx->cfg, ctx->cfg.cantidad_nodos)) {
        return salir(DSM_ERR_NOMEM);
    }
    reintentar_cancelaciones(ctx);
    struct vinculacion *v = calloc(1, sizeof *v);
    if (v == NULL) {
        return salir(DSM_ERR_NOMEM);
    }
    if (dsm_directorio_id_reserva(&ctx->directorio, &v->id) != 0) {
        rc = error_api(errno);
        free(v);
        return salir(rc);
    }
    v->provisional = 1;
    v->retirando = 1;
    v->siguiente = ctx->reservas;
    ctx->reservas = v;
    dsm_region_info_t mapa = {
        .reserva_id = v->id, .tamano_bytes = bytes,
        .total_paginas = (uint32_t)(1 + (bytes - 1) / ctx->cfg.page_size),
    };
    rc = preparar_mapa(ctx, v, &mapa);
    if (rc == DSM_OK) {
        rc = confirmar(ctx, v, &mapa);
    }
    if (rc != DSM_OK) {
        cancelar(ctx, &ctx->reservas);
        return salir(rc);
    }
    v->copias_por_cerrar = v->aportantes & ~(UINT32_C(1) << ctx->nodo);
    v->provisional = 0;
    v->retirando = 0;
    *base = v->base;
    *id = v->id;
    return salir(DSM_OK);
}

/* Un cierre ambiguo conserva el registro para reintentar con el mismo ID.
 * Ningun trabajador vuelve a usar una vista que se esta retirando. */
static int desvincular(dsm_context_t *ctx, struct vinculacion **pos)
{
    struct vinculacion *v = *pos;
    v->retirando = 1;
    if (dsm_coherencia_desvincular(ctx->coherencia, v->id) != 0) {
        return error_api(errno);
    }
    if (v->base != NULL && dsm_pager_retirar_vista(ctx->pager, v->id) != 0) {
        return error_api(errno);
    }
    v->base = NULL;
    if (dsm_directorio_desvincular(&ctx->directorio, v->id) != 0 && errno != ENOENT) {
        return error_api(errno);
    }
    quitar(pos);
    return DSM_OK;
}

int dsm_abrir_reserva(dsm_context_t *ctx, dsm_reserva_id_t id, void **base, size_t *bytes)
{
    int rc = entrar(ctx);
    if (rc != DSM_OK) {
        return rc;
    }
    uint16_t creador = (uint16_t)(id >> 16);
    if (base == NULL || bytes == NULL || (uint16_t)id == 0 || creador >= ctx->cfg.cantidad_nodos) {
        return salir(DSM_ERR_INVAL);
    }
    struct vinculacion **pos = posicion(ctx, id);
    if (*pos == NULL) {
        if (creador == ctx->nodo) {
            return salir(DSM_ERR_INVAL);
        }
        if (dsm_coherencia_esperar(ctx->coherencia, creador) != 0) {
            /* No se envio una solicitud a este nodo. */
            return salir(DSM_ERR_NODO);
        }
        struct vinculacion *v = calloc(1, sizeof *v);
        if (v == NULL) {
            return salir(DSM_ERR_NOMEM);
        }
        *pos = v;
        v->id = id;
        dsm_vistas_t *vista;
        dsm_pagina_desc_t primera;
        if (dsm_coherencia_abrir(ctx->coherencia, id, &primera, &vista) != 0) {
            int error = errno;
            if (error == ENOENT) {
                quitar(pos);
                return salir(DSM_ERR_INVAL);
            }
            rc = error_api(error);
        } else {
            rc = asociar(ctx, v, vista);
        }
        if (rc != DSM_OK) {
            desvincular(ctx, pos);
            return salir(rc);
        }
    }
    if ((*pos)->retirando) {
        return salir(DSM_ERR_OCUPADA);
    }
    *base = (*pos)->base;
    *bytes = (*pos)->bytes;
    return salir(DSM_OK);
}

int dsm_desvincular_reserva(dsm_context_t *ctx, dsm_reserva_id_t id)
{
    int rc = entrar(ctx);
    if (rc != DSM_OK) {
        return rc;
    }
    struct vinculacion **pos = posicion(ctx, id);
    if (*pos == NULL) {
        return salir(DSM_ERR_INVAL);
    }
    if ((id >> 16) == ctx->nodo) {
        return salir(DSM_ERR_ESTADO);
    }
    return salir(ctx->trabajadores == 0 ? desvincular(ctx, pos) : DSM_ERR_OCUPADA);
}

/* El creador cierra LOOKUP bajo el mutex del directorio antes de devolver
 * capacidad. Los bits se limpian tras cada ACK para poder reintentar. */
static int liberar(dsm_context_t *ctx, struct vinculacion **pos)
{
    struct vinculacion *v = *pos;
    if (v->provisional) {
        return cancelar(ctx, pos);
    }
    if (!v->retirando) {
        if (dsm_directorio_iniciar_retiro(&ctx->directorio, v->id) != 0) {
            return error_api(errno);
        }
        v->retirando = 1;
    }
    for (uint16_t hogar = 0; hogar < ctx->cfg.cantidad_nodos; hogar++) {
        uint32_t bit = UINT32_C(1) << hogar;
        if (v->copias_por_cerrar & bit) {
            if (dsm_coherencia_cerrar_copia(ctx->coherencia, hogar, v->id) != 0) {
                return error_api(errno);
            }
            v->copias_por_cerrar &= ~bit;
        }
    }
    if (v->base != NULL) {
        if (dsm_pager_retirar_vista(ctx->pager, v->id) != 0) {
            return error_api(errno);
        }
        v->base = NULL;
    }
    for (uint16_t hogar = 0; hogar < ctx->cfg.cantidad_nodos; hogar++) {
        uint32_t bit = UINT32_C(1) << hogar;
        if (hogar != ctx->nodo && (v->aportantes & bit)) {
            if (dsm_coherencia_soltar(ctx->coherencia, hogar, v->id) != 0) {
                return error_api(errno);
            }
            v->aportantes &= ~bit;
        }
    }
    if (dsm_coherencia_soltar(ctx->coherencia, ctx->nodo, v->id) != 0) {
        return error_api(errno);
    }
    quitar(pos);
    return DSM_OK;
}

int dsm_liberar(dsm_context_t *ctx, dsm_reserva_id_t id)
{
    int rc = entrar(ctx);
    if (rc != DSM_OK) {
        return rc;
    }
    struct vinculacion **pos = posicion(ctx, id);
    if (*pos == NULL || (id >> 16) != ctx->nodo) {
        return salir(*pos == NULL ? DSM_ERR_INVAL : DSM_ERR_ESTADO);
    }
    return salir(ctx->trabajadores == 0 ? liberar(ctx, pos) : DSM_ERR_OCUPADA);
}

int dsm_cerrar(dsm_context_t *ctx)
{
    int rc = entrar(ctx);
    if (rc != DSM_OK) {
        return rc;
    }
    if (ctx->trabajadores != 0) {
        return salir(DSM_ERR_OCUPADA);
    }
    struct vinculacion **pos = &ctx->reservas;
    while (*pos != NULL) {
        if (((*pos)->id >> 16) == ctx->nodo) {
            pos = &(*pos)->siguiente;
        } else if ((rc = desvincular(ctx, pos)) != DSM_OK) {
            return salir(rc);
        }
    }
    while (ctx->reservas != NULL) {
        rc = liberar(ctx, &ctx->reservas);
        if (rc != DSM_OK) {
            return salir(rc);
        }
    }
    if (dsm_directorio_preparar_cierre(&ctx->directorio) != 0 ||
        dsm_pager_cerrar(ctx->pager) != 0) {
        return salir(error_api(errno));
    }
    dsm_coherencia_cerrar(ctx->coherencia);
    dsm_directorio_cerrar(&ctx->directorio);
    activo = NULL;
    free(ctx);
    return salir(DSM_OK);
}

static int validar_rango(struct vinculacion *v, size_t offset, size_t bytes)
{
    if (v == NULL || offset > v->bytes || bytes > v->bytes - offset) {
        return DSM_ERR_INVAL;
    }
    return v->retirando ? DSM_ERR_OCUPADA : DSM_OK;
}

int dsm_ubicacion_pagina(dsm_context_t *ctx, dsm_reserva_id_t id, size_t orden,
                         dsm_node_id_t *hogar, uint64_t *gpn)
{
    int rc = entrar(ctx);
    if (rc != DSM_OK) {
        return rc;
    }
    struct vinculacion *v = *posicion(ctx, id);
    if (v == NULL || hogar == NULL || gpn == NULL || v->bytes == 0 ||
        orden > (v->bytes - 1) / ctx->cfg.page_size) {
        return salir(DSM_ERR_INVAL);
    }
    if (v->retirando) {
        return salir(DSM_ERR_OCUPADA);
    }
    dsm_pagina_desc_t pagina;
    if (dsm_directorio_pagina_reserva(&ctx->directorio, id, orden, &pagina) != 0) {
        return salir(error_api(errno));
    }
    *hogar = pagina.hogar;
    *gpn = pagina.gpn;
    return salir(DSM_OK);
}

static int hay_conflicto(const dsm_context_t *ctx, const struct copia *nueva)
{
    for (const struct copia *c = ctx->copias; c != NULL; c = c->siguiente) {
        if (c->reserva == nueva->reserva && (c->escritura || nueva->escritura) &&
            c->inicio < nueva->fin && nueva->inicio < c->fin) {
            return 1;
        }
    }
    return 0;
}

static int buscar_buffer(dsm_context_t *ctx, const void *buffer, size_t bytes,
                          struct vinculacion **vinculo, size_t *offset)
{
    uintptr_t direccion = (uintptr_t)buffer;
    if (bytes - 1 > UINTPTR_MAX - direccion) {
        return DSM_ERR_INVAL;
    }
    *vinculo = NULL;
    for (struct vinculacion *v = ctx->reservas; v != NULL; v = v->siguiente) {
        size_t asignados = v->bytes == 0 ? 0 : ((v->bytes - 1) / ctx->cfg.page_size + 1) * ctx->cfg.page_size;
        uintptr_t base = (uintptr_t)v->base;
        if (v->base != NULL && direccion >= base && direccion - base < asignados) {
            *offset = (size_t)(direccion - base);
            int rc = validar_rango(v, *offset, bytes);
            if (rc != DSM_OK) {
                return rc;
            }
            *vinculo = v;
            break;
        }
    }
    return DSM_OK;
}

static int copiar_pagina(dsm_context_t *ctx, struct vinculacion *v, size_t offset,
                          void *scratch, size_t bytes, int escritura, const struct timespec *plazo)
{
    dsm_pagina_desc_t pagina;
    if (dsm_directorio_pagina_reserva(&ctx->directorio, v->id,
                                       offset / ctx->cfg.page_size, &pagina) != 0 ||
        dsm_coherencia_copiar_controlado(ctx->coherencia, v->id, pagina.gpn,
                                          offset % ctx->cfg.page_size, scratch, bytes,
                                          escritura, plazo) != 0) {
        return error_api(errno);
    }
    return DSM_OK;
}

static int copiar_trozos(dsm_context_t *ctx, struct vinculacion *fuente, size_t offset_fuente,
                          const void *origen, struct vinculacion *receptor, size_t offset_receptor,
                          void *destino, size_t bytes, unsigned char *scratch)
{
    struct timespec plazo;
    if (clock_gettime(CLOCK_MONOTONIC, &plazo) != 0) {
        return error_api(errno);
    }
    plazo.tv_sec += ctx->cfg.rpc_timeout_ms / 1000u;
    plazo.tv_nsec += (long)(ctx->cfg.rpc_timeout_ms % 1000u) * 1000000L;
    if (plazo.tv_nsec >= 1000000000L) {
        plazo.tv_sec++;
        plazo.tv_nsec -= 1000000000L;
    }
    uintptr_t direccion_origen = (uintptr_t)origen;
    uintptr_t direccion_destino = (uintptr_t)destino;
    int hacia_atras = direccion_destino > direccion_origen &&
                      direccion_destino - direccion_origen < bytes;
    size_t hechos = 0;
    while (hechos < bytes) {
        size_t posicion = hacia_atras ? bytes - hechos - 1 : hechos;
        size_t cantidad = bytes - hechos;
        size_t pagina = ctx->cfg.page_size;
        if (fuente != NULL) {
            size_t dentro = (offset_fuente + posicion) % pagina;
            size_t limite = hacia_atras ? dentro + 1 : pagina - dentro;
            if (cantidad > limite) {
                cantidad = limite;
            }
        }
        if (receptor != NULL) {
            size_t dentro = (offset_receptor + posicion) % pagina;
            size_t limite = hacia_atras ? dentro + 1 : pagina - dentro;
            if (cantidad > limite) {
                cantidad = limite;
            }
        }
        size_t inicio = hacia_atras ? bytes - hechos - cantidad : hechos;
        int rc = DSM_OK;
        if (fuente != NULL) {
            rc = copiar_pagina(ctx, fuente, offset_fuente + inicio, scratch, cantidad, 0, &plazo);
        } else {
            memmove(scratch, (const unsigned char *)origen + inicio, cantidad);
        }
        if (rc != DSM_OK) {
            return rc;
        }
        if (receptor != NULL) {
            rc = copiar_pagina(ctx, receptor, offset_receptor + inicio, scratch, cantidad, 1, &plazo);
        } else {
            memmove((unsigned char *)destino + inicio, scratch, cantidad);
        }
        if (rc != DSM_OK) {
            return rc;
        }
        hechos += cantidad;
    }
    return DSM_OK;
}

static void retirar_copia(dsm_context_t *ctx, struct copia *copia)
{
    struct copia **actual = &ctx->copias;
    while (*actual != copia) {
        actual = &(*actual)->siguiente;
    }
    *actual = copia->siguiente;
}

static int copiar(dsm_context_t *ctx, dsm_reserva_id_t id, size_t offset,
                  void *destino, const void *origen, size_t bytes, int escritura)
{
    int rc = entrar(ctx);
    if (rc != DSM_OK) {
        return rc;
    }
    struct vinculacion *v = *posicion(ctx, id);
    rc = validar_rango(v, offset, bytes);
    if (rc != DSM_OK || bytes == 0) {
        return salir(rc);
    }
    if (escritura ? origen == NULL : destino == NULL) {
        return salir(DSM_ERR_INVAL);
    }
    struct vinculacion *buffer;
    size_t offset_buffer = 0;
    rc = buscar_buffer(ctx, escritura ? origen : destino, bytes, &buffer, &offset_buffer);
    if (rc != DSM_OK) {
        return salir(rc);
    }
    unsigned char *scratch = malloc(ctx->cfg.page_size);
    if (scratch == NULL) {
        return salir(DSM_ERR_NOMEM);
    }
    int temporal = !registrado;
    if (temporal && (rc = registrar(ctx)) != DSM_OK) {
        free(scratch);
        return salir(rc);
    }
    struct copia copia = {.reserva = v, .inicio = offset, .fin = offset + bytes,
                          .escritura = escritura};
    struct copia adicional = {.reserva = buffer, .inicio = offset_buffer,
                              .fin = offset_buffer + bytes, .escritura = !escritura};
    while (hay_conflicto(ctx, &copia) || (buffer != NULL && hay_conflicto(ctx, &adicional))) {
        pthread_cond_wait(&fin_copia, &mutex);
    }
    if (buffer != NULL) {
        adicional.siguiente = ctx->copias;
        ctx->copias = &adicional;
    }
    copia.siguiente = ctx->copias;
    ctx->copias = &copia;
    pthread_mutex_unlock(&mutex);

    /* El registro mantiene vivas ambas vistas durante un RPC. Solo scratch
     * cruza el mutex del directorio, y la copia conserva el orden de memmove. */
    unsigned char *direccion = (unsigned char *)v->base + offset;
    if (escritura) {
        rc = copiar_trozos(ctx, buffer, offset_buffer, origen, v, offset, direccion, bytes, scratch);
    } else {
        rc = copiar_trozos(ctx, v, offset, direccion, buffer, offset_buffer, destino, bytes, scratch);
    }
    free(scratch);
    pthread_mutex_lock(&mutex);
    retirar_copia(ctx, &copia);
    if (buffer != NULL) {
        retirar_copia(ctx, &adicional);
    }
    pthread_cond_broadcast(&fin_copia);
    int desregistro = temporal ? desregistrar(ctx) : DSM_OK;
    return salir(rc != DSM_OK ? rc : desregistro);
}

int dsm_leer(dsm_context_t *ctx, dsm_reserva_id_t id, size_t offset,
             void *destino, size_t bytes)
{
    if (bytes == 0) {
        return copiar(ctx, id, offset, destino, NULL, bytes, 0);
    }
    void *temporal = malloc(bytes);
    if (temporal == NULL) {
        return DSM_ERR_NOMEM;
    }
    int rc = copiar(ctx, id, offset, temporal, NULL, bytes, 0);
    if (rc == DSM_OK) {
        memcpy(destino, temporal, bytes);
    }
    free(temporal);
    return rc;
}

int dsm_escribir(dsm_context_t *ctx, dsm_reserva_id_t id, size_t offset,
                 const void *origen, size_t bytes)
{
    if (bytes == 0) {
        return copiar(ctx, id, offset, NULL, origen, bytes, 1);
    }
    void *previo = malloc(bytes);
    if (previo == NULL) {
        return DSM_ERR_NOMEM;
    }
    int rc = copiar(ctx, id, offset, previo, NULL, bytes, 0);
    if (rc == DSM_OK) {
        rc = copiar(ctx, id, offset, NULL, origen, bytes, 1);
        if (rc != DSM_OK) {
            int vuelta = copiar(ctx, id, offset, NULL, previo, bytes, 1);
            if (vuelta != DSM_OK) {
                rc = vuelta;
            }
        }
    }
    free(previo);
    return rc;
}

static int pedir_stats(dsm_context_t *ctx, uint16_t par, uint8_t seleccion, dsm_stats_reply_t *msg)
{
    if (dsm_coherencia_stats(ctx->coherencia, par, seleccion, msg) != 0) {
        return error_api(errno);
    }
    return DSM_OK;
}

/* Un par apagado no aporta páginas. La consulta lo anota como fuera y sigue. */
static int par_mudo(int rc)
{
    return rc == DSM_ERR_NODO || rc == DSM_ERR_TIEMPO;
}

static void anotar_fuera(dsm_nodo_info_t *info, const dsm_nodo_cfg_t *nodo)
{
    memset(info, 0, sizeof *info);
    info->id = nodo->id;
    info->estado = DSM_NODO_FUERA;
    info->ipv4_host = nodo->ipv4_host;
    info->puerto = nodo->puerto;
    info->memoria = nodo->memoria;
    memcpy(info->nombre, nodo->nombre, DSM_CONSULTA_NOMBRE - 1);
}

static void llenar_nodo(dsm_nodo_info_t *info, const dsm_ficha_t *ficha, const uint64_t *contadores)
{
    memset(info, 0, sizeof *info);
    info->id = ficha->id;
    info->estado = ficha->estado;
    info->ipv4_host = ficha->ipv4_host;
    info->puerto = ficha->puerto;
    info->memoria = ficha->memoria;
    info->base = ficha->base;
    info->libres = ficha->libres;
    info->totales = ficha->totales;
    info->aportados = ficha->totales >= ficha->libres ? ficha->totales - ficha->libres : 0;
    memcpy(info->nombre, ficha->nombre, DSM_CONSULTA_NOMBRE - 1);
    memcpy(info->contadores, contadores, sizeof info->contadores);
}

static int publicar(void *salida, size_t cap, size_t tam, size_t total, const void *origen,
                    size_t *cantidad)
{
    *cantidad = total;
    if (salida == NULL) {
        return cap == 0 ? DSM_OK : DSM_ERR_INVAL;
    }
    size_t n = total < cap ? total : cap;
    if (n != 0) {
        memcpy(salida, origen, n * tam);
    }
    return total <= cap ? DSM_OK : DSM_ERR_NOMEM;
}

int dsm_consultar_nodos(dsm_context_t *ctx, dsm_nodo_info_t *salida, size_t cap, size_t *cantidad)
{
    int rc = entrar(ctx);
    if (rc != DSM_OK) {
        return rc;
    }
    if (cantidad == NULL) {
        return salir(DSM_ERR_INVAL);
    }
    uint16_t nodos = ctx->cfg.cantidad_nodos;
    dsm_nodo_info_t *tabla = calloc(nodos == 0 ? 1 : nodos, sizeof *tabla);
    size_t total = 0;
    if (tabla == NULL) {
        return salir(DSM_ERR_NOMEM);
    }
    for (uint16_t id = 0; id < nodos; id++) {
        dsm_stats_reply_t msg;
        rc = pedir_stats(ctx, id, DSM_STATS_NODOS, &msg);
        if (par_mudo(rc)) {
            if (total + 1 > nodos) {
                dsm_nodo_info_t *nueva = realloc(tabla, (total + 1) * sizeof *tabla);
                if (nueva == NULL) {
                    free(tabla);
                    return salir(DSM_ERR_NOMEM);
                }
                tabla = nueva;
            }
            anotar_fuera(&tabla[total], dsm_config_nodo(&ctx->cfg, id));
            total++;
            continue;
        }
        if (rc != DSM_OK) {
            free(tabla);
            return salir(rc);
        }
        uint16_t n = msg.n;
        if (total + n > nodos) {
            dsm_nodo_info_t *nueva = realloc(tabla, (total + n) * sizeof *tabla);
            if (nueva == NULL) {
                free(msg.paginas);
                free(tabla);
                return salir(DSM_ERR_NOMEM);
            }
            tabla = nueva;
        }
        for (uint16_t i = 0; i < n; i++) {
            llenar_nodo(&tabla[total + i], &msg.fichas[i], msg.contadores);
        }
        total += n;
        free(msg.paginas);
    }
    rc = publicar(salida, cap, sizeof *salida, total, tabla, cantidad);
    free(tabla);
    return salir(rc);
}

int dsm_consultar_paginas(dsm_context_t *ctx, uint16_t hogar, dsm_pagina_info_t *salida,
                          size_t cap, size_t *cantidad)
{
    int rc = entrar(ctx);
    if (rc != DSM_OK) {
        return rc;
    }
    if (cantidad == NULL || (hogar != DSM_HOGAR_TODOS && hogar >= ctx->cfg.cantidad_nodos)) {
        return salir(DSM_ERR_INVAL);
    }
    uint16_t desde = hogar == DSM_HOGAR_TODOS ? 0 : hogar;
    uint16_t hasta = hogar == DSM_HOGAR_TODOS ? ctx->cfg.cantidad_nodos : (uint16_t)(hogar + 1u);
    dsm_pagina_info_t *tabla = NULL;
    size_t total = 0;
    for (uint16_t id = desde; id < hasta; id++) {
        dsm_stats_reply_t msg;
        rc = pedir_stats(ctx, id, DSM_STATS_AMBOS, &msg);
        if (par_mudo(rc)) {
            continue;
        }
        if (rc != DSM_OK) {
            free(tabla);
            return salir(rc);
        }
        uint32_t libres = msg.n > 0 ? msg.fichas[0].libres : 0;
        if (msg.p != 0) {
            dsm_pagina_info_t *nueva = realloc(tabla, (total + msg.p) * sizeof *tabla);
            if (nueva == NULL) {
                free(msg.paginas);
                free(tabla);
                return salir(DSM_ERR_NOMEM);
            }
            tabla = nueva;
            for (uint32_t i = 0; i < msg.p; i++) {
                const dsm_pagina_consulta_t *p = &msg.paginas[i];
                tabla[total + i] = (dsm_pagina_info_t){
                    .gpn = p->gpn,
                    .mascara = p->mascara,
                    .proc = p->proc,
                    .reserva = p->reserva,
                    .orden = p->orden,
                    .hogar = p->hogar,
                    .poseedor = p->poseedor,
                    .msi = p->msi,
                    .permiso = p->permiso,
                    .copias = p->copias,
                    .memoria_libre = libres,
                };
            }
            total += msg.p;
        }
        free(msg.paginas);
    }
    rc = publicar(salida, cap, sizeof *salida, total, tabla, cantidad);
    free(tabla);
    return salir(rc);
}

static char letra_msi(uint8_t msi)
{
    return msi == DSM_MSI_M ? 'M' : msi == DSM_MSI_S ? 'S' : 'I';
}

int dsm_imprimir(dsm_context_t *ctx, FILE *salida, int global)
{
    int rc = entrar(ctx);
    if (rc != DSM_OK) {
        return rc;
    }
    if (salida == NULL || (global != 0 && global != 1)) {
        return salir(DSM_ERR_INVAL);
    }
    uint16_t desde = global ? 0 : ctx->nodo;
    uint16_t hasta = global ? ctx->cfg.cantidad_nodos : (uint16_t)(ctx->nodo + 1u);
    flockfile(salida);
    for (uint16_t id = desde; id < hasta; id++) {
        dsm_stats_reply_t msg;
        rc = pedir_stats(ctx, id, DSM_STATS_AMBOS, &msg);
        if (par_mudo(rc)) {
            const dsm_nodo_cfg_t *nodo = dsm_config_nodo(&ctx->cfg, id);
            fprintf(salida, "nodo %u estado %u puerto %u aportados 0 libres 0 nombre %s\n",
                    nodo->id, DSM_NODO_FUERA, nodo->puerto, nodo->nombre);
            continue;
        }
        if (rc != DSM_OK) {
            funlockfile(salida);
            return salir(rc);
        }
        uint32_t libres = 0;
        for (uint16_t i = 0; i < msg.n; i++) {
            const dsm_ficha_t *f = &msg.fichas[i];
            char nombre[DSM_CONSULTA_NOMBRE];
            uint32_t aportados = f->totales >= f->libres ? f->totales - f->libres : 0;
            libres = f->libres;
            memcpy(nombre, f->nombre, sizeof nombre - 1);
            nombre[sizeof nombre - 1] = '\0';
            fprintf(salida,
                    "nodo %u estado %u puerto %u aportados %u libres %u nombre %s\n",
                    f->id, f->estado, f->puerto, aportados, f->libres, nombre);
        }
        fprintf(salida,
                "contadores nodo %u lectura %" PRIu64 " escritura %" PRIu64
                " inv_env %" PRIu64 " inv_rec %" PRIu64 " pag_env %" PRIu64
                " pag_rec %" PRIu64 " bytes_env %" PRIu64 " bytes_rec %" PRIu64 "\n",
                msg.emisor, msg.contadores[DSM_CONTA_LECTURA], msg.contadores[DSM_CONTA_ESCRITURA],
                msg.contadores[DSM_CONTA_INV_ENV], msg.contadores[DSM_CONTA_INV_REC],
                msg.contadores[DSM_CONTA_PAG_ENV], msg.contadores[DSM_CONTA_PAG_REC],
                msg.contadores[DSM_CONTA_BYTES_ENV], msg.contadores[DSM_CONTA_BYTES_REC]);
        for (uint32_t i = 0; i < msg.p; i++) {
            const dsm_pagina_consulta_t *p = &msg.paginas[i];
            fprintf(salida,
                    "pagina gpn %" PRIu64 " hogar %u poseedor %u msi %c reserva %" PRIu32
                    " orden %" PRIu32 " proceso %" PRIu32 " memoria_libre %u\n",
                    p->gpn, p->hogar, p->poseedor, letra_msi(p->msi), p->reserva, p->orden,
                    p->proc, libres);
        }
        free(msg.paginas);
    }
    funlockfile(salida);
    return salir(DSM_OK);
}

int dsm_traducir(dsm_context_t *ctx, dsm_reserva_id_t id, size_t offset, void **salida)
{
    int rc = entrar(ctx);
    if (rc != DSM_OK) {
        return rc;
    }
    struct vinculacion *v = *posicion(ctx, id);
    rc = validar_rango(v, offset, 1);
    if (salida != NULL && rc == DSM_OK) {
        *salida = (unsigned char *)v->base + offset;
    }
    return salir(salida == NULL ? DSM_ERR_INVAL : rc);
}
