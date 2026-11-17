#include "dsm.h"

#include "config.h"
#include "directory.h"
#include "pager.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

struct dsm_context {
    dsm_directorio_t directorio;
    dsm_pager_t *pager;
    dsm_pagina_desc_t pagina;
    dsm_node_id_t nodo;
    uint16_t cantidad_nodos;
    dsm_proc_id_t proceso;
    int publicada;
};

/* Serializa la API del unico contexto y su ciclo de vida. */
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static dsm_context_t *activo;

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

/* Las paginas locales se crean en M. Cualquier otro fallo es inesperado. */
static int rechazar_fallo(const dsm_pager_peticion_t *peticion, void *buf, size_t capacidad,
                          dsm_pager_concesion_t *concesion, void *datos)
{
    (void)peticion;
    (void)buf;
    (void)capacidad;
    (void)concesion;
    (void)datos;
    errno = ENOTSUP;
    return -1;
}

static int preparar_contexto(dsm_context_t *ctx, const dsm_config_t *cfg)
{
    if (dsm_directorio_init(&ctx->directorio, cfg, ctx->nodo) != 0) {
        return DSM_ERR_ESTADO;
    }
    if (dsm_directorio_reservar_pagina(&ctx->directorio, &ctx->pagina) != 0) {
        return DSM_ERR_NOMEM;
    }

    dsm_vistas_t *vistas;
    if (dsm_directorio_vista(&ctx->directorio, ctx->pagina.reserva, NULL, &vistas) != 0 ||
        dsm_vistas_proteger(vistas, PROT_NONE) != 0) {
        return DSM_ERR_ESTADO;
    }
    const dsm_pager_pagina_t pagina = {
        .reserva_id = ctx->pagina.reserva,
        .gpn = ctx->pagina.gpn,
        .version = ctx->pagina.version,
        .msi = ctx->pagina.msi,
    };
    if (dsm_pager_iniciar_msi(&ctx->pager, vistas, cfg->page_size, cfg->max_hilos, &pagina,
                              ctx->nodo, cfg->rpc_timeout_ms, rechazar_fallo, NULL) != 0) {
        return errno == ENOMEM ? DSM_ERR_NOMEM : DSM_ERR_ESTADO;
    }
    return DSM_OK;
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
    ctx->nodo = nodo;
    ctx->cantidad_nodos = cfg.cantidad_nodos;
    int rc = preparar_contexto(ctx, &cfg);
    if (rc != DSM_OK) {
        dsm_directorio_cerrar(&ctx->directorio);
        free(ctx);
        return salir(rc);
    }
    activo = ctx;
    *salida = ctx;
    return salir(DSM_OK);
}

/* Cierra el paginador antes de desmontar su vista prestada. */
static int cerrar_reserva(dsm_context_t *ctx)
{
    if (ctx->pager != NULL && dsm_pager_cerrar(ctx->pager) != 0) {
        return errno == EBUSY ? DSM_ERR_OCUPADA : DSM_ERR_ESTADO;
    }
    ctx->pager = NULL;
    dsm_directorio_cerrar(&ctx->directorio);
    ctx->pagina = (dsm_pagina_desc_t){0};
    ctx->publicada = 0;
    return DSM_OK;
}

int dsm_cerrar(dsm_context_t *ctx)
{
    int rc = entrar(ctx);
    if (rc != DSM_OK) {
        return rc;
    }
    rc = cerrar_reserva(ctx);
    if (rc == DSM_OK) {
        activo = NULL;
        free(ctx);
    }
    return salir(rc);
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
    ctx->proceso = ((uint32_t)ctx->nodo << 16) | 1;
    *salida = ctx->proceso;
    return salir(DSM_OK);
}

static int proceso_valido(const dsm_context_t *ctx, dsm_proc_id_t proceso)
{
    return proceso != 0 && proceso == ctx->proceso;
}

int dsm_hilo_registrar(dsm_context_t *ctx, dsm_proc_id_t proceso)
{
    int rc = entrar(ctx);
    if (rc != DSM_OK) {
        return rc;
    }
    if (!proceso_valido(ctx, proceso)) {
        return salir(DSM_ERR_INVAL);
    }
    if (ctx->pager == NULL) {
        return salir(DSM_ERR_ESTADO);
    }
    if (dsm_pager_registrar(ctx->pager) != 0) {
        return salir(errno == ENOSPC ? DSM_ERR_HILOS : DSM_ERR_ESTADO);
    }
    return salir(DSM_OK);
}

int dsm_hilo_desregistrar(dsm_context_t *ctx)
{
    int rc = entrar(ctx);
    if (rc != DSM_OK) {
        return rc;
    }
    if (ctx->pager == NULL || dsm_pager_desregistrar(ctx->pager) != 0) {
        return salir(DSM_ERR_ESTADO);
    }
    return salir(DSM_OK);
}

int dsm_reservar(dsm_context_t *ctx, dsm_proc_id_t proceso, size_t bytes,
                 dsm_reserva_id_t *id, void **base)
{
    int rc = entrar(ctx);
    if (rc != DSM_OK) {
        return rc;
    }
    if (!proceso_valido(ctx, proceso) || bytes == 0 || id == NULL || base == NULL) {
        return salir(DSM_ERR_INVAL);
    }
    if (ctx->publicada || ctx->pager == NULL || bytes > ctx->directorio.page_size) {
        return salir(DSM_ERR_NOMEM);
    }
    ctx->publicada = 1;
    *id = ctx->pagina.reserva;
    *base = ctx->pagina.ubicacion_local;
    return salir(DSM_OK);
}

/* Distingue el ID local publicado de una operacion que requiere red. */
static int buscar_reserva(const dsm_context_t *ctx, dsm_reserva_id_t id)
{
    uint32_t creador = id >> 16;
    if ((uint16_t)id == 0 || creador >= ctx->cantidad_nodos) {
        return DSM_ERR_INVAL;
    }
    if (creador != ctx->nodo) {
        return DSM_ERR_NODO;
    }
    return ctx->publicada && id == ctx->pagina.reserva ? DSM_OK : DSM_ERR_INVAL;
}

int dsm_abrir_reserva(dsm_context_t *ctx, dsm_reserva_id_t id, void **base, size_t *bytes)
{
    int rc = entrar(ctx);
    if (rc != DSM_OK) {
        return rc;
    }
    if (base == NULL || bytes == NULL) {
        return salir(DSM_ERR_INVAL);
    }
    rc = buscar_reserva(ctx, id);
    if (rc == DSM_OK) {
        *base = ctx->pagina.ubicacion_local;
        *bytes = ctx->directorio.page_size;
    }
    return salir(rc);
}

int dsm_desvincular_reserva(dsm_context_t *ctx, dsm_reserva_id_t id)
{
    int rc = entrar(ctx);
    if (rc != DSM_OK) {
        return rc;
    }
    rc = buscar_reserva(ctx, id);
    /* Desvincular corresponde a una vista remota. El creador usa liberar. */
    return salir(rc == DSM_OK ? DSM_ERR_ESTADO : rc);
}

int dsm_liberar(dsm_context_t *ctx, dsm_reserva_id_t id)
{
    int rc = entrar(ctx);
    if (rc != DSM_OK) {
        return rc;
    }
    rc = buscar_reserva(ctx, id);
    if (rc != DSM_OK) {
        return salir(rc == DSM_ERR_NODO ? DSM_ERR_ESTADO : rc);
    }
    return salir(cerrar_reserva(ctx));
}

static int validar_rango(const dsm_context_t *ctx, dsm_reserva_id_t id, size_t offset, size_t bytes)
{
    int rc = buscar_reserva(ctx, id);
    if (rc != DSM_OK) {
        return rc;
    }
    size_t capacidad = ctx->directorio.page_size;
    return offset <= capacidad && bytes <= capacidad - offset ? DSM_OK : DSM_ERR_INVAL;
}

int dsm_leer(dsm_context_t *ctx, dsm_reserva_id_t id, size_t offset,
              void *destino, size_t bytes)
{
    int rc = entrar(ctx);
    if (rc != DSM_OK) {
        return rc;
    }
    if (destino == NULL && bytes != 0) {
        return salir(DSM_ERR_INVAL);
    }
    rc = validar_rango(ctx, id, offset, bytes);
    if (rc == DSM_OK && bytes != 0) {
        memmove(destino, (const unsigned char *)ctx->pagina.ubicacion_local + offset, bytes);
    }
    return salir(rc);
}

int dsm_escribir(dsm_context_t *ctx, dsm_reserva_id_t id, size_t offset,
                  const void *origen, size_t bytes)
{
    int rc = entrar(ctx);
    if (rc != DSM_OK) {
        return rc;
    }
    if (origen == NULL && bytes != 0) {
        return salir(DSM_ERR_INVAL);
    }
    rc = validar_rango(ctx, id, offset, bytes);
    if (rc == DSM_OK && bytes != 0) {
        memmove((unsigned char *)ctx->pagina.ubicacion_local + offset, origen, bytes);
    }
    return salir(rc);
}

int dsm_traducir(dsm_context_t *ctx, dsm_reserva_id_t id, size_t offset, void **salida)
{
    int rc = entrar(ctx);
    if (rc != DSM_OK) {
        return rc;
    }
    if (salida == NULL) {
        return salir(DSM_ERR_INVAL);
    }
    rc = validar_rango(ctx, id, offset, 1);
    if (rc == DSM_OK) {
        *salida = (unsigned char *)ctx->pagina.ubicacion_local + offset;
    }
    return salir(rc);
}
