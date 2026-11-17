#include "directory.h"

#include "network.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

int dsm_directorio_init(dsm_directorio_t *dir, const dsm_config_t *cfg, uint16_t nodo_id)
{
    if (dir == NULL) {
        return -1;
    }
    memset(dir, 0, sizeof *dir);
    if (cfg == NULL || cfg->cantidad_nodos > DSM_CONFIG_MAX_NODOS ||
        cfg->rpc_timeout_ms == 0 || cfg->page_size == 0) {
        return -1;
    }
    const dsm_nodo_cfg_t *nodo = dsm_config_nodo(cfg, nodo_id);
    if (nodo == NULL) {
        return -1;
    }
    for (uint16_t i = 0; i <= cfg->cantidad_nodos; i++) {
        uint64_t base = dsm_config_base(cfg, i);
        if (base == UINT64_MAX || base % cfg->page_size != 0 ||
            (i > 0 && base / cfg->page_size <= dir->limites_gpn[i - 1])) {
            return -1;
        }
        dir->limites_gpn[i] = base / cfg->page_size;
    }
    uint64_t paginas = nodo->memoria / cfg->page_size;
    if (paginas > UINT32_MAX || pthread_mutex_init(&dir->mutex, NULL) != 0) {
        return -1;
    }
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) != 0) {
        pthread_mutex_destroy(&dir->mutex);
        return -1;
    }
    int cola_lista = 0;
    int error = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (error == 0) {
        error = pthread_cond_init(&dir->cola_cv, &attr);
        cola_lista = error == 0;
    }
    if (error == 0) {
        error = pthread_cond_init(&dir->campana_cv, &attr);
    }
    pthread_condattr_destroy(&attr);
    if (error != 0) {
        if (cola_lista) {
            pthread_cond_destroy(&dir->cola_cv);
        }
        pthread_mutex_destroy(&dir->mutex);
        return -1;
    }
    dir->nodo_id = nodo_id;
    dir->cantidad_nodos = cfg->cantidad_nodos;
    dir->page_size = cfg->page_size;
    dir->rpc_timeout_ms = cfg->rpc_timeout_ms;
    dir->memoria_inicial = nodo->memoria;
    dir->paginas_totales = (uint32_t)paginas;
    dir->paginas_libres = dir->paginas_totales;
    dir->siguiente_local = 1;
    for (size_t i = 0; i < DSM_DIR_MAX_VISTAS; i++) {
        dir->vistas[i].fd = -1;
    }
    dir->inicializado = 1;
    return 0;
}

void dsm_directorio_cerrar(dsm_directorio_t *dir)
{
    if (dir == NULL || !dir->inicializado) {
        return;
    }
    dsm_directorio_desactivar(dir);
    for (size_t i = 0; i < dir->nvistas && i < DSM_DIR_MAX_VISTAS; i++) {
        dsm_vistas_cerrar(&dir->vistas[i]);
    }
    pthread_cond_destroy(&dir->campana_cv);
    pthread_cond_destroy(&dir->cola_cv);
    pthread_mutex_destroy(&dir->mutex);
    free(dir->campana_buf);
    memset(dir, 0, sizeof *dir);
}

/* Los lectores solo modifican el mutex de un directorio inicializado. */
static pthread_mutex_t *mutex_de(const dsm_directorio_t *dir)
{
    return (pthread_mutex_t *)&dir->mutex;
}

static int bloquear(const dsm_directorio_t *dir)
{
    return dir != NULL && dir->inicializado && pthread_mutex_lock(mutex_de(dir)) == 0;
}

static int id_reserva_valido(const dsm_directorio_t *dir, uint32_t reserva_id)
{
    return (reserva_id >> 16) < dir->cantidad_nodos && (uint16_t)reserva_id != 0;
}

static uint16_t hogar_del_gpn(const dsm_directorio_t *dir, uint64_t gpn)
{
    for (uint16_t hogar = 0; hogar < dir->cantidad_nodos; hogar++) {
        if (gpn < dir->limites_gpn[hogar + 1]) {
            return hogar;
        }
    }
    return dir->cantidad_nodos;
}

/* Se llama con el mutex del directorio tomado. */
static size_t indice_reserva(const dsm_directorio_t *dir, uint32_t reserva_id)
{
    for (size_t i = 0; i < dir->npaginas; i++) {
        if (dir->paginas[i].reserva == reserva_id) {
            return i;
        }
    }
    return dir->npaginas;
}

int dsm_directorio_reservar_pagina(dsm_directorio_t *dir, dsm_pagina_desc_t *salida)
{
    if (!bloquear(dir)) {
        return -1;
    }
    int resultado = -1;
    if (dir->paginas_libres == 0 || dir->tiene_manifiesto ||
        dir->npaginas >= DSM_DIR_MAX_PAGINAS || dir->nvistas >= DSM_DIR_MAX_VISTAS) {
        goto salir;
    }
    uint64_t indice = dir->paginas_totales - dir->paginas_libres;
    uint64_t gpn = dir->limites_gpn[dir->nodo_id] + indice;
    dsm_vistas_t *vista = &dir->vistas[dir->nvistas];
    if (dsm_vistas_crear(dir->page_size, vista) != 0) {
        goto salir;
    }
    if (dsm_vistas_proteger_pagina(vista, 0, dir->page_size, PROT_READ | PROT_WRITE) != 0) {
        int error = errno;
        dsm_vistas_cerrar(vista);
        errno = error;
        goto salir;
    }
    uint32_t reserva = ((uint32_t)dir->nodo_id << 16) | dir->siguiente_local;

    dsm_pagina_desc_t desc;
    memset(&desc, 0, sizeof desc);
    desc.gpn = gpn;
    desc.hogar = dir->nodo_id;
    desc.poseedor = dir->nodo_id;
    desc.reserva = reserva;
    desc.msi = DSM_MSI_M;
    desc.version = 0;
    desc.mascara_copias = UINT64_C(1) << dir->nodo_id;
    desc.ubicacion_local = vista->a_r;
    desc.orden = 0;

    memset(&dir->manifiesto, 0, sizeof dir->manifiesto);
    dir->manifiesto.reserva_id = reserva;
    dir->manifiesto.total_tramos = 1;
    dir->manifiesto.tamano_bytes = dir->page_size;
    dir->manifiesto.primer_tramo = 0;
    dir->manifiesto.cantidad_tramos = 1;
    dir->manifiesto.total_paginas = 1;
    dir->manifiesto.tramos[0].orden_inicial = 0;
    dir->manifiesto.tramos[0].cantidad = 1;
    dir->manifiesto.tramos[0].gpn_inicial = gpn;

    dir->paginas[dir->npaginas++] = desc;
    dir->nvistas++;
    dir->paginas_libres--;
    dir->reservas_globales++;
    dir->siguiente_local++;
    dir->tiene_manifiesto = 1;
    if (salida != NULL) {
        *salida = desc;
    }
    resultado = 0;

salir:
    pthread_mutex_unlock(&dir->mutex);
    return resultado;
}

int dsm_directorio_consultar(const dsm_directorio_t *dir, uint32_t reserva_id,
                             uint32_t primer_tramo, dsm_region_info_t *info, uint16_t *estado)
{
    if (info == NULL || estado == NULL || !bloquear(dir)) {
        return -1;
    }
    if (!dir->tiene_manifiesto || dir->manifiesto.reserva_id != reserva_id ||
        primer_tramo != dir->manifiesto.primer_tramo) {
        *estado = DSM_ESTADO_RANGO;
        memset(info, 0, sizeof *info);
    } else {
        *info = dir->manifiesto;
        *estado = DSM_ESTADO_OK;
    }
    pthread_mutex_unlock(mutex_de(dir));
    return 0;
}

int dsm_directorio_instalar(dsm_directorio_t *dir, const dsm_region_info_t *info)
{
    if (info == NULL || !bloquear(dir)) {
        errno = EINVAL;
        return -1;
    }
    int resultado = -1;
    const dsm_tramo_t *tramo = &info->tramos[0];
    uint16_t creador = (uint16_t)(info->reserva_id >> 16);
    uint16_t hogar = hogar_del_gpn(dir, tramo->gpn_inicial);
    if (!id_reserva_valido(dir, info->reserva_id) || info->total_tramos != 1 ||
        info->primer_tramo != 0 || info->cantidad_tramos != 1 || info->total_paginas != 1 ||
        info->tamano_bytes != dir->page_size || tramo->orden_inicial != 0 ||
        tramo->cantidad != 1 || hogar >= dir->cantidad_nodos) {
        errno = EPROTO;
        goto salir;
    }
    for (size_t i = 0; i < dir->npaginas; i++) {
        const dsm_pagina_desc_t *pagina = &dir->paginas[i];
        if (pagina->reserva == info->reserva_id && pagina->gpn == tramo->gpn_inicial) {
            resultado = 0;
            goto salir;
        }
        if (pagina->reserva == info->reserva_id || pagina->gpn == tramo->gpn_inicial) {
            errno = EPROTO;
            goto salir;
        }
    }
    if (creador == dir->nodo_id || hogar == dir->nodo_id) {
        errno = EPROTO;
        goto salir;
    }
    if (dir->npaginas >= DSM_DIR_MAX_PAGINAS || dir->nvistas >= DSM_DIR_MAX_VISTAS) {
        errno = ENOSPC;
        goto salir;
    }
    dsm_vistas_t *vista = &dir->vistas[dir->nvistas];
    if (dsm_vistas_crear(dir->page_size, vista) != 0) {
        goto salir;
    }
    dir->paginas[dir->npaginas++] = (dsm_pagina_desc_t){
        .gpn = tramo->gpn_inicial,
        .hogar = hogar,
        .poseedor = DSM_POSEEDOR_NINGUNO,
        .reserva = info->reserva_id,
        .msi = DSM_MSI_I,
        .ubicacion_local = vista->a_r,
    };
    dir->nvistas++;
    resultado = 0;

salir:
    pthread_mutex_unlock(&dir->mutex);
    return resultado;
}

int dsm_directorio_codificar_info(dsm_directorio_t *dir, const dsm_cabecera_t *pedido,
                                  const dsm_region_lookup_t *consulta, uint32_t id_msg,
                                  uint8_t *buf, size_t cap, size_t *n)
{
    if (dir == NULL || !dir->inicializado || pedido == NULL || consulta == NULL ||
        id_msg == 0 || buf == NULL || n == NULL || cap < DSM_CABECERA_BYTES) {
        errno = EINVAL;
        return -1;
    }
    if (pedido->origen >= dir->cantidad_nodos || pedido->origen == dir->nodo_id ||
        pedido->version != DSM_PROTO_VERSION || pedido->tipo != DSM_TIPO_REGION_LOOKUP ||
        pedido->banderas != 0 || pedido->destino != dir->nodo_id ||
        pedido->id_msg == 0 || pedido->id_corr != 0 || pedido->estado != DSM_ESTADO_OK) {
        errno = EPROTO;
        return -1;
    }
    dsm_region_info_t info;
    uint16_t estado = DSM_ESTADO_RANGO;
    if (dsm_directorio_consultar(dir, consulta->reserva_id, consulta->primer_tramo, &info,
                                 &estado) != 0) {
        return -1;
    }
    dsm_cabecera_t cab = {
        .version = DSM_PROTO_VERSION,
        .tipo = DSM_TIPO_REGION_INFO,
        .banderas = DSM_FLAG_RESPUESTA,
        .longitud = DSM_CABECERA_BYTES,
        .origen = dir->nodo_id,
        .destino = pedido->origen,
        .id_msg = id_msg,
        .id_corr = pedido->id_msg,
        .estado = estado,
    };
    if (estado == DSM_ESTADO_OK) {
        return dsm_region_info_codificar(buf, cap, &cab, &info, n);
    }
    if (dsm_cabecera_codificar(buf, &cab) != 0) {
        return -1;
    }
    *n = DSM_CABECERA_BYTES;
    return 0;
}

int dsm_directorio_responder(dsm_directorio_t *dir, int fd, uint16_t peer,
                             const dsm_cabecera_t *pedido,
                             const dsm_region_lookup_t *consulta, uint32_t *seq)
{
    if (pedido == NULL || seq == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (pedido->origen != peer) {
        errno = EPROTO;
        return -1;
    }
    uint8_t buf[512];
    size_t n;
    if (dsm_directorio_codificar_info(dir, pedido, consulta, *seq, buf, sizeof buf, &n) != 0) {
        return -1;
    }
    (*seq)++;
    return dsm_enviar_trama(fd, buf, n, dir->rpc_timeout_ms);
}

int dsm_directorio_publicar(dsm_directorio_t *dir, int fd, uint32_t *seq)
{
    if (dir == NULL || !dir->inicializado || seq == NULL || *seq == 0) {
        errno = EINVAL;
        return -1;
    }
    uint8_t buf[512];
    size_t n = 0;
    dsm_cabecera_t cab;
    dsm_region_lookup_t consulta;
    if (dsm_recibir_trama_plazo(fd, buf, sizeof buf, &n, dir->rpc_timeout_ms) != 0) {
        return -1;
    }
    if (dsm_region_lookup_decodificar(buf, n, &cab, &consulta) != 0) {
        errno = EPROTO;
        return -1;
    }
    return dsm_directorio_responder(dir, fd, cab.origen, &cab, &consulta, seq);
}

int dsm_directorio_solicitar(const dsm_directorio_t *dir, int fd, uint32_t reserva_id,
                            uint32_t *seq, uint32_t *id_lookup)
{
    uint16_t creador = (uint16_t)(reserva_id >> 16);
    if (dir == NULL || !dir->inicializado || seq == NULL || *seq == 0 || id_lookup == NULL ||
        creador == dir->nodo_id || !id_reserva_valido(dir, reserva_id)) {
        errno = EINVAL;
        return -1;
    }
    dsm_region_lookup_t consulta;
    consulta.reserva_id = reserva_id;
    consulta.primer_tramo = 0;
    dsm_cabecera_t cab;
    memset(&cab, 0, sizeof cab);
    cab.version = DSM_PROTO_VERSION;
    cab.tipo = DSM_TIPO_REGION_LOOKUP;
    cab.banderas = 0;
    cab.origen = dir->nodo_id;
    cab.destino = creador;
    cab.id_msg = (*seq)++;
    cab.id_corr = 0;
    cab.estado = DSM_ESTADO_OK;
    uint8_t buf[40];
    size_t n = 0;
    if (dsm_region_lookup_codificar(buf, sizeof buf, &cab, &consulta, &n) != 0) {
        errno = EPROTO;
        return -1;
    }
    if (dsm_enviar_trama(fd, buf, n, dir->rpc_timeout_ms) != 0) {
        return -1;
    }
    *id_lookup = cab.id_msg;
    return 0;
}

int dsm_directorio_recibir_info(dsm_directorio_t *dir, uint32_t reserva_id,
                                uint32_t id_lookup, const uint8_t *buf, size_t n,
                                dsm_cabecera_t *cab_out, dsm_region_info_t *info_out)
{
    uint16_t creador = (uint16_t)(reserva_id >> 16);
    if (dir == NULL || !dir->inicializado || id_lookup == 0 ||
        creador == dir->nodo_id || !id_reserva_valido(dir, reserva_id)) {
        errno = EINVAL;
        return -1;
    }
    dsm_cabecera_t resp;
    dsm_region_info_t info;
    if (dsm_region_info_decodificar(buf, n, &resp, &info) != 0 ||
        resp.banderas != DSM_FLAG_RESPUESTA || resp.id_corr != id_lookup ||
        resp.origen != creador || resp.destino != dir->nodo_id) {
        errno = EPROTO;
        return -1;
    }
    if (resp.estado != DSM_ESTADO_OK) {
        errno = resp.estado == DSM_ESTADO_RANGO ? ENOENT : EPROTO;
        return -1;
    }
    if (info.reserva_id != reserva_id) {
        errno = EPROTO;
        return -1;
    }
    if (dsm_directorio_instalar(dir, &info) != 0) {
        return -1;
    }
    if (cab_out != NULL) {
        *cab_out = resp;
    }
    if (info_out != NULL) {
        *info_out = info;
    }
    return 0;
}

int dsm_directorio_traer(dsm_directorio_t *dir, int fd, uint32_t reserva_id, uint32_t *seq,
                         dsm_cabecera_t *cab_out, dsm_region_info_t *info_out)
{
    uint32_t id_lookup;
    uint8_t buf[512];
    size_t n;
    if (dsm_directorio_solicitar(dir, fd, reserva_id, seq, &id_lookup) != 0 ||
        dsm_recibir_trama_plazo(fd, buf, sizeof buf, &n, dir->rpc_timeout_ms) != 0) {
        return -1;
    }
    return dsm_directorio_recibir_info(dir, reserva_id, id_lookup, buf, n, cab_out, info_out);
}

void *dsm_directorio_c_r_reserva(const dsm_directorio_t *dir, uint32_t reserva_id)
{
    if (!bloquear(dir)) {
        return NULL;
    }
    size_t i = indice_reserva(dir, reserva_id);
    void *gestor = i < dir->npaginas ? dir->vistas[i].c_r : NULL;
    pthread_mutex_unlock(mutex_de(dir));
    return gestor;
}

void *dsm_directorio_c_r(const dsm_directorio_t *dir)
{
    if (!bloquear(dir)) {
        return NULL;
    }
    void *gestor = dir->nvistas == 1 ? dir->vistas[0].c_r : NULL;
    pthread_mutex_unlock(mutex_de(dir));
    return gestor;
}

int dsm_directorio_vista(dsm_directorio_t *dir, uint32_t reserva_id,
                         dsm_pagina_desc_t *desc, dsm_vistas_t **vista)
{
    if ((desc == NULL && vista == NULL) || !bloquear(dir)) {
        errno = EINVAL;
        return -1;
    }
    size_t i = indice_reserva(dir, reserva_id);
    if (i == dir->npaginas) {
        pthread_mutex_unlock(&dir->mutex);
        errno = ENOENT;
        return -1;
    }
    if (desc != NULL) {
        *desc = dir->paginas[i];
    }
    if (vista != NULL) {
        *vista = &dir->vistas[i];
    }
    pthread_mutex_unlock(&dir->mutex);
    return 0;
}

int dsm_directorio_servir_pagina(dsm_directorio_t *dir, uint16_t solicitante,
                                 const dsm_page_request_t *pedido, dsm_page_data_t *datos,
                                 void *imagen, size_t imagen_cap, int *cambio)
{
    if (pedido == NULL || datos == NULL || cambio == NULL || !bloquear(dir)) {
        errno = EINVAL;
        return -1;
    }
    int resultado = -1;
    if (solicitante >= dir->cantidad_nodos || solicitante == dir->nodo_id ||
        pedido->acceso != DSM_ACCESO_LECTURA || pedido->msi != DSM_MSI_I ||
        hogar_del_gpn(dir, pedido->gpn) != dir->nodo_id) {
        errno = EPROTO;
        goto salir;
    }
    size_t i = 0;
    while (i < dir->npaginas && dir->paginas[i].gpn != pedido->gpn) {
        i++;
    }
    if (i == dir->npaginas) {
        errno = ENOENT;
        goto salir;
    }
    dsm_pagina_desc_t *pagina = &dir->paginas[i];
    dsm_vistas_t *vista = &dir->vistas[i];
    if (pagina->msi != DSM_MSI_M && pagina->msi != DSM_MSI_S) {
        errno = EPROTO;
        goto salir;
    }
    if (imagen != NULL && imagen_cap < dir->page_size) {
        errno = EINVAL;
        goto salir;
    }
    int era_modified = pagina->msi == DSM_MSI_M;
    if (era_modified) {
        if (pagina->version == UINT32_MAX) {
            errno = EOVERFLOW;
            goto salir;
        }
        if (dsm_vistas_proteger_pagina(vista, 0, dir->page_size, PROT_READ) != 0) {
            goto salir;
        }
        pagina->msi = DSM_MSI_S;
        pagina->poseedor = DSM_POSEEDOR_NINGUNO;
        pagina->version++;
    }
    pagina->mascara_copias |= UINT64_C(1) << solicitante;
    *datos = (dsm_page_data_t){
        .gpn = pagina->gpn,
        .proc_id = pedido->proc_id,
        .reserva_id = pagina->reserva,
        .orden = pagina->orden,
        .hogar = pagina->hogar,
        .poseedor = pagina->poseedor,
        .msi = pagina->msi,
        .permiso = DSM_PAGE_PERMISO_LEER,
        .mascara_copias = pagina->mascara_copias,
        .page_nbytes = dir->page_size,
        .version = pagina->version,
        .datos = vista->c_r,
    };
    if (imagen != NULL) {
        memcpy(imagen, vista->c_r, dir->page_size);
        datos->datos = imagen;
    }
    *cambio = era_modified;
    resultado = 0;

salir:
    pthread_mutex_unlock(&dir->mutex);
    return resultado;
}

int dsm_directorio_compartir_pagina(dsm_directorio_t *dir, uint32_t reserva_id,
                                     const dsm_page_data_t *datos)
{
    if (datos == NULL || !bloquear(dir)) {
        errno = EINVAL;
        return -1;
    }
    int resultado = -1;
    size_t i = indice_reserva(dir, reserva_id);
    if (i == dir->npaginas) {
        errno = ENOENT;
        goto salir;
    }
    dsm_pagina_desc_t *pagina = &dir->paginas[i];
    uint64_t bits_esperados = (UINT64_C(1) << pagina->hogar) | (UINT64_C(1) << dir->nodo_id);
    uint64_t bits_validos = (UINT64_C(1) << dir->cantidad_nodos) - 1;
    if (pagina->msi != DSM_MSI_I || datos->msi != DSM_MSI_S ||
        datos->reserva_id != reserva_id || datos->gpn != pagina->gpn ||
        datos->orden != pagina->orden || datos->hogar != pagina->hogar ||
        datos->poseedor != DSM_POSEEDOR_NINGUNO || datos->version <= pagina->version ||
        datos->permiso != DSM_PAGE_PERMISO_LEER || datos->banderas != 0 ||
        datos->page_nbytes != dir->page_size || datos->datos == NULL ||
        (datos->mascara_copias & bits_esperados) != bits_esperados ||
        (datos->mascara_copias & ~bits_validos) != 0) {
        errno = EPROTO;
        goto salir;
    }
    memcpy(dir->vistas[i].c_r, datos->datos, dir->page_size);
    pagina->poseedor = datos->poseedor;
    pagina->msi = DSM_MSI_S;
    pagina->version = datos->version;
    pagina->mascara_copias = datos->mascara_copias;
    resultado = 0;

salir:
    pthread_mutex_unlock(&dir->mutex);
    return resultado;
}

static size_t indice_gpn(const dsm_directorio_t *dir, uint64_t gpn)
{
    for (size_t i = 0; i < dir->npaginas; i++) {
        if (dir->paginas[i].gpn == gpn) {
            return i;
        }
    }
    return dir->npaginas;
}

static int bit_de(uint64_t mascara, uint16_t id)
{
    return id < 64 && (mascara & (UINT64_C(1) << id)) != 0;
}

static int proteger(dsm_directorio_t *dir, size_t indice, int prot)
{
    return dsm_vistas_proteger_pagina(&dir->vistas[indice], 0, dir->page_size, prot);
}

static dsm_cabecera_t cabecera_de(const dsm_directorio_t *dir, uint8_t tipo, uint16_t destino,
                                  uint32_t id)
{
    dsm_cabecera_t cab;
    memset(&cab, 0, sizeof cab);
    cab.version = DSM_PROTO_VERSION;
    cab.tipo = tipo;
    cab.origen = dir->nodo_id;
    cab.destino = destino;
    cab.id_msg = id;
    cab.estado = DSM_ESTADO_OK;
    return cab;
}

static int enviar_fuera(dsm_directorio_t *dir, uint16_t destino, const uint8_t *buf, size_t n)
{
    if (dir->enviar == NULL) {
        errno = EINVAL;
        return -1;
    }
    return dir->enviar(dir->envio_ctx, destino, buf, n);
}

static uint16_t estado_errno(int error)
{
    if (error == ETIMEDOUT) {
        return DSM_ESTADO_PLAZO;
    }
    if (error == ENOTCONN || error == ECONNRESET || error == EPIPE) {
        return DSM_ESTADO_FUERA;
    }
    if (error == EAGAIN) {
        return DSM_ESTADO_COLA;
    }
    if (error == EPROTO) {
        return DSM_ESTADO_CONFLICTO;
    }
    if (error == EPERM) {
        return DSM_ESTADO_NO_HOGAR;
    }
    return DSM_ESTADO_RANGO;
}

static int enviar_pagina(dsm_directorio_t *dir, uint16_t destino, uint32_t id_corr, uint16_t estado,
                         const dsm_page_data_t *datos)
{
    uint32_t id = dir->nuevo_id(dir->envio_ctx);
    if (id == 0) {
        errno = EINVAL;
        return -1;
    }
    dsm_cabecera_t cab = cabecera_de(dir, DSM_TIPO_PAGE_DATA, destino, id);
    cab.banderas = DSM_FLAG_RESPUESTA;
    cab.id_corr = id_corr;
    cab.estado = estado;
    size_t cap = dir->campana_cap;
    uint8_t *buf = malloc(cap);
    if (buf == NULL) {
        return -1;
    }
    size_t n = 0;
    int rc = dsm_page_data_codificar(buf, cap, &cab, estado == DSM_ESTADO_OK ? datos : NULL, &n);
    if (rc == 0) {
        rc = enviar_fuera(dir, destino, buf, n);
    }
    if (rc == 0 && estado == DSM_ESTADO_OK && datos != NULL) {
        printf("nodo %u PAGE_DATA enviado par %u reserva %" PRIu32 " gpn %" PRIu64
               " bytes %" PRIu32 " trama %zu corr %" PRIu32 "\n",
               dir->nodo_id, destino, datos->reserva_id, datos->gpn, datos->page_nbytes, n,
               id_corr);
    }
    free(buf);
    return rc;
}

static void campana_abrir(dsm_directorio_t *dir, uint16_t par, uint32_t id)
{
    pthread_mutex_lock(&dir->mutex);
    dir->campana_espera = 1;
    dir->campana_par = par;
    dir->campana_id = id;
    dir->campana_error = 0;
    dir->tiene_datos = 0;
    dir->tiene_ack = 0;
    dir->datos_n = 0;
    pthread_mutex_unlock(&dir->mutex);
}

static void campana_cerrar(dsm_directorio_t *dir)
{
    pthread_mutex_lock(&dir->mutex);
    dir->campana_espera = 0;
    pthread_mutex_unlock(&dir->mutex);
}

static int campana_esperar(dsm_directorio_t *dir, int quiere_datos, int quiere_ack, uint8_t *ack,
                           uint8_t *datos, size_t *datos_n)
{
    struct timespec plazo;
    if (clock_gettime(CLOCK_MONOTONIC, &plazo) != 0) {
        return -1;
    }
    uint32_t ms = dir->rpc_timeout_ms;
    plazo.tv_sec += ms / 1000u;
    plazo.tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (plazo.tv_nsec >= 1000000000L) {
        plazo.tv_sec++;
        plazo.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&dir->mutex);
    int error = 0;
    while (error == 0 && dir->campana_error == 0 &&
           ((quiere_datos && !dir->tiene_datos) || (quiere_ack && !dir->tiene_ack))) {
        error = pthread_cond_timedwait(&dir->campana_cv, &dir->mutex, &plazo);
    }
    if (error == 0) {
        error = dir->campana_error;
    }
    if (error == 0) {
        if (quiere_ack && ack != NULL) {
            memcpy(ack, dir->ack_trama, DSM_CONTROL_BYTES);
        }
        if (quiere_datos && datos != NULL && datos_n != NULL) {
            if (dir->datos_n > dir->campana_cap) {
                error = EPROTO;
            } else {
                memcpy(datos, dir->campana_buf, dir->datos_n);
                *datos_n = dir->datos_n;
            }
        }
    }
    pthread_mutex_unlock(&dir->mutex);
    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}

static void pausa_poseedor(const dsm_directorio_t *dir)
{
    if (dir->retardo_ms != 0) {
        usleep(dir->retardo_ms * 1000u);
    }
}

static int atender_invalidate(dsm_directorio_t *dir, uint16_t par, const uint8_t *trama, size_t n)
{
    dsm_cabecera_t pedido;
    dsm_invalidate_t msg;
    if (dsm_invalidate_decodificar(trama, n, &pedido, &msg) != 0 || pedido.origen != par) {
        errno = EPROTO;
        return -1;
    }
    pausa_poseedor(dir);

    uint16_t resultado = DSM_INVALIDA_YA_I;
    uint16_t estado = DSM_ESTADO_OK;
    pthread_mutex_lock(&dir->mutex);
    size_t i = indice_gpn(dir, msg.gpn);
    if (i < dir->npaginas && dir->paginas[i].msi == DSM_MSI_M &&
        dir->paginas[i].poseedor == dir->nodo_id) {
        estado = DSM_ESTADO_CONFLICTO;
    } else if (i < dir->npaginas && dir->paginas[i].msi != DSM_MSI_I) {
        if (proteger(dir, i, PROT_NONE) != 0) {
            int error = errno;
            pthread_mutex_unlock(&dir->mutex);
            errno = error;
            return -1;
        }
        dir->paginas[i].msi = DSM_MSI_I;
        dir->paginas[i].poseedor = DSM_POSEEDOR_NINGUNO;
        dir->paginas[i].mascara_copias &= ~(UINT64_C(1) << dir->nodo_id);
        resultado = DSM_INVALIDA_HECHO;
    }
    pthread_mutex_unlock(&dir->mutex);

    uint32_t id = dir->nuevo_id(dir->envio_ctx);
    dsm_cabecera_t cab = cabecera_de(dir, DSM_TIPO_INVALIDATE_ACK, par, id);
    cab.banderas = DSM_FLAG_RESPUESTA;
    cab.id_corr = pedido.id_msg;
    cab.estado = estado;
    dsm_invalidate_ack_t ack = {
        .gpn = msg.gpn, .id_solicitud = msg.id_solicitud, .resultado = resultado,
    };
    uint8_t buf[DSM_CONTROL_BYTES];
    size_t out = 0;
    if (id == 0 || dsm_invalidate_ack_codificar(buf, sizeof buf, &cab, &ack, &out) != 0) {
        errno = EPROTO;
        return -1;
    }
    return enviar_fuera(dir, par, buf, out);
}

static int atender_downgrade(dsm_directorio_t *dir, uint16_t par, const uint8_t *trama, size_t n)
{
    dsm_cabecera_t pedido;
    dsm_downgrade_t msg;
    if (dsm_downgrade_decodificar(trama, n, &pedido, &msg) != 0 || pedido.origen != par) {
        errno = EPROTO;
        return -1;
    }
    pausa_poseedor(dir);

    uint8_t *imagen = malloc(dir->page_size);
    if (imagen == NULL) {
        return -1;
    }
    dsm_page_data_t datos;
    memset(&datos, 0, sizeof datos);
    int tenia = 0;
    uint16_t resultado = DSM_DOWNGRADE_NO_M;
    pthread_mutex_lock(&dir->mutex);
    size_t i = indice_gpn(dir, msg.gpn);
    dsm_pagina_desc_t *pagina = i < dir->npaginas ? &dir->paginas[i] : NULL;
    if (pagina != NULL && pagina->msi == DSM_MSI_M && pagina->poseedor == dir->nodo_id &&
        pagina->hogar == par && msg.solicitante < dir->cantidad_nodos) {
        if (msg.motivo == DSM_MOTIVO_A_S && pagina->version == UINT32_MAX) {
            pthread_mutex_unlock(&dir->mutex);
            free(imagen);
            errno = EOVERFLOW;
            return -1;
        }
        int prot = msg.motivo == DSM_MOTIVO_A_S ? PROT_READ : PROT_NONE;
        if (proteger(dir, i, prot) != 0) {
            int error = errno;
            pthread_mutex_unlock(&dir->mutex);
            free(imagen);
            errno = error;
            return -1;
        }
        memcpy(imagen, dir->vistas[i].c_r, dir->page_size);
        if (msg.motivo == DSM_MOTIVO_A_S) {
            pagina->version++;
            pagina->msi = DSM_MSI_S;
            pagina->poseedor = DSM_POSEEDOR_NINGUNO;
            pagina->mascara_copias =
                (UINT64_C(1) << pagina->hogar) | (UINT64_C(1) << dir->nodo_id) |
                (UINT64_C(1) << msg.solicitante);
        } else {
            pagina->msi = DSM_MSI_I;
            pagina->poseedor = DSM_POSEEDOR_NINGUNO;
            pagina->mascara_copias = 0;
        }
        datos = (dsm_page_data_t){
            .gpn = pagina->gpn,
            .proc_id = (uint32_t)msg.solicitante << 16,
            .reserva_id = pagina->reserva,
            .orden = pagina->orden,
            .hogar = pagina->hogar,
            .poseedor = DSM_POSEEDOR_NINGUNO,
            .msi = DSM_MSI_S,
            .permiso = DSM_PAGE_PERMISO_LEER,
            .mascara_copias = (UINT64_C(1) << pagina->hogar) | (UINT64_C(1) << dir->nodo_id) |
                              (UINT64_C(1) << msg.solicitante),
            .page_nbytes = dir->page_size,
            .version = pagina->version,
            .datos = imagen,
        };
        if (msg.motivo == DSM_MOTIVO_A_I) {
            datos.msi = DSM_MSI_S;
            datos.mascara_copias = (UINT64_C(1) << pagina->hogar) | (UINT64_C(1) << msg.solicitante);
        }
        tenia = 1;
        resultado = DSM_DOWNGRADE_HECHO;
    }
    pthread_mutex_unlock(&dir->mutex);

    int rc = 0;
    if (tenia) {
        uint32_t id = dir->nuevo_id(dir->envio_ctx);
        dsm_cabecera_t cab = cabecera_de(dir, DSM_TIPO_PAGE_DATA, par, id);
        cab.banderas = DSM_FLAG_RESPUESTA;
        cab.id_corr = pedido.id_msg;
        size_t cap = (size_t)dir->page_size + DSM_PAGE_DATA_BASE;
        uint8_t *buf = malloc(cap);
        size_t out = 0;
        if (buf == NULL || id == 0 ||
            dsm_page_data_codificar(buf, cap, &cab, &datos, &out) != 0 ||
            enviar_fuera(dir, par, buf, out) != 0) {
            rc = -1;
        }
        free(buf);
    }
    uint32_t id_ack = dir->nuevo_id(dir->envio_ctx);
    dsm_cabecera_t cab_ack = cabecera_de(dir, DSM_TIPO_PAGE_DOWNGRADE_ACK, par, id_ack);
    cab_ack.banderas = DSM_FLAG_RESPUESTA;
    cab_ack.id_corr = pedido.id_msg;
    dsm_downgrade_ack_t ack = {
        .gpn = msg.gpn, .id_solicitud = msg.id_solicitud, .resultado = resultado,
    };
    uint8_t raw[DSM_CONTROL_BYTES];
    size_t out = 0;
    if (rc == 0 &&
        (id_ack == 0 || dsm_downgrade_ack_codificar(raw, sizeof raw, &cab_ack, &ack, &out) != 0 ||
         enviar_fuera(dir, par, raw, out) != 0)) {
        rc = -1;
    }
    if (rc == 0 && tenia && msg.motivo == DSM_MOTIVO_A_S && msg.solicitante != dir->nodo_id &&
        msg.solicitante != par) {
        uint32_t id = dir->nuevo_id(dir->envio_ctx);
        dsm_cabecera_t cab = cabecera_de(dir, DSM_TIPO_PAGE_DATA, msg.solicitante, id);
        cab.banderas = DSM_FLAG_RESPUESTA;
        cab.id_corr = msg.id_solicitud;
        size_t cap = (size_t)dir->page_size + DSM_PAGE_DATA_BASE;
        uint8_t *buf = malloc(cap);
        size_t out_cli = 0;
        if (buf == NULL || id == 0 ||
            dsm_page_data_codificar(buf, cap, &cab, &datos, &out_cli) != 0 ||
            enviar_fuera(dir, msg.solicitante, buf, out_cli) != 0) {
            rc = -1;
        }
        free(buf);
    }
    free(imagen);
    return rc;
}

static int pedir_bajada(dsm_directorio_t *dir, uint16_t poseedor, uint64_t gpn, uint32_t id_solicitud,
                        uint16_t solicitante, uint8_t motivo, uint32_t version_esperada,
                        uint8_t *imagen, uint32_t *version_out)
{
    uint32_t id = dir->nuevo_id(dir->envio_ctx);
    if (id == 0) {
        errno = EINVAL;
        return -1;
    }
    campana_abrir(dir, poseedor, id);
    dsm_cabecera_t cab = cabecera_de(dir, DSM_TIPO_PAGE_DOWNGRADE, poseedor, id);
    dsm_downgrade_t msg = {
        .gpn = gpn, .id_solicitud = id_solicitud, .solicitante = solicitante, .motivo = motivo,
    };
    uint8_t envio[DSM_CONTROL_BYTES];
    size_t n = 0;
    if (dsm_downgrade_codificar(envio, sizeof envio, &cab, &msg, &n) != 0 ||
        enviar_fuera(dir, poseedor, envio, n) != 0) {
        campana_cerrar(dir);
        return -1;
    }
    uint8_t ack_raw[DSM_CONTROL_BYTES];
    uint8_t *recibo = malloc(dir->campana_cap);
    size_t recibo_n = 0;
    int rc = -1;
    if (recibo != NULL &&
        campana_esperar(dir, 1, 1, ack_raw, recibo, &recibo_n) == 0) {
        dsm_cabecera_t cab_ack;
        dsm_downgrade_ack_t ack;
        dsm_cabecera_t cab_datos;
        dsm_page_data_t datos;
        if (dsm_downgrade_ack_decodificar(ack_raw, DSM_CONTROL_BYTES, &cab_ack, &ack) == 0 &&
            cab_ack.estado == DSM_ESTADO_OK && cab_ack.id_corr == id && ack.gpn == gpn &&
            ack.resultado == DSM_DOWNGRADE_HECHO &&
            dsm_page_data_decodificar(recibo, recibo_n, &cab_datos, &datos) == 0 &&
            cab_datos.estado == DSM_ESTADO_OK && cab_datos.id_corr == id && datos.gpn == gpn &&
            datos.version == version_esperada && datos.page_nbytes == dir->page_size &&
            datos.datos != NULL) {
            memcpy(imagen, datos.datos, dir->page_size);
            *version_out = datos.version;
            rc = 0;
        } else {
            errno = EPROTO;
        }
    }
    free(recibo);
    campana_cerrar(dir);
    return rc;
}

static int pedir_invalidacion(dsm_directorio_t *dir, uint16_t destino, uint64_t gpn,
                              uint32_t id_solicitud, uint16_t escritor)
{
    uint32_t id = dir->nuevo_id(dir->envio_ctx);
    if (id == 0) {
        errno = EINVAL;
        return -1;
    }
    campana_abrir(dir, destino, id);
    dsm_cabecera_t cab = cabecera_de(dir, DSM_TIPO_INVALIDATE, destino, id);
    dsm_invalidate_t msg = {.gpn = gpn, .id_solicitud = id_solicitud, .escritor = escritor};
    uint8_t envio[DSM_CONTROL_BYTES];
    size_t n = 0;
    int rc = -1;
    uint8_t ack_raw[DSM_CONTROL_BYTES];
    if (dsm_invalidate_codificar(envio, sizeof envio, &cab, &msg, &n) == 0 &&
        enviar_fuera(dir, destino, envio, n) == 0 &&
        campana_esperar(dir, 0, 1, ack_raw, NULL, NULL) == 0) {
        dsm_cabecera_t cab_ack;
        dsm_invalidate_ack_t ack;
        if (dsm_invalidate_ack_decodificar(ack_raw, DSM_CONTROL_BYTES, &cab_ack, &ack) == 0 &&
            cab_ack.estado == DSM_ESTADO_OK && cab_ack.id_corr == id && ack.gpn == gpn &&
            ack.resultado <= DSM_INVALIDA_YA_I) {
            rc = 0;
        } else {
            errno = EPROTO;
        }
    }
    campana_cerrar(dir);
    return rc;
}

static void adoptar_bytes(dsm_directorio_t *dir, uint64_t gpn, const uint8_t *imagen)
{
    pthread_mutex_lock(&dir->mutex);
    size_t i = indice_gpn(dir, gpn);
    if (i < dir->npaginas && proteger(dir, i, PROT_READ | PROT_WRITE) == 0) {
        memcpy(dir->vistas[i].c_r, imagen, dir->page_size);
        dir->paginas[i].msi = DSM_MSI_M;
        dir->paginas[i].poseedor = dir->nodo_id;
        dir->paginas[i].mascara_copias = UINT64_C(1) << dir->nodo_id;
        dir->paginas[i].perdida = 0;
    }
    pthread_mutex_unlock(&dir->mutex);
}

static int publicar_lectura_local(dsm_directorio_t *dir, uint16_t par, const dsm_cabecera_t *pedido_cab,
                                  const dsm_page_request_t *pedido)
{
    printf("nodo %u PAGE_REQUEST recibido par %u gpn %" PRIu64 " bytes %u id %" PRIu32 "\n",
           dir->nodo_id, par, pedido->gpn, DSM_PAGE_REQUEST_BYTES, pedido_cab->id_msg);
    dsm_page_data_t datos;
    int cambio = 0;
    uint8_t *imagen = malloc(dir->page_size);
    if (imagen == NULL) {
        return -1;
    }
    int rc = dsm_directorio_servir_pagina(dir, par, pedido, &datos, imagen, dir->page_size, &cambio);
    if (rc != 0) {
        int error = errno;
        free(imagen);
        return enviar_pagina(dir, par, pedido_cab->id_msg, estado_errno(error), NULL) == 0 ? 0 : -1;
    }
    if (cambio) {
        printf("nodo %u reserva %" PRIu32 " gpn %" PRIu64 " MSI M -> S version %" PRIu32 "\n",
               dir->nodo_id, datos.reserva_id, datos.gpn, datos.version);
    }
    rc = enviar_pagina(dir, par, pedido_cab->id_msg, DSM_ESTADO_OK, &datos);
    free(imagen);
    return rc;
}

static int instalar_bajada(dsm_directorio_t *dir, uint64_t gpn, uint16_t solicitante, uint16_t poseedor,
                           const uint8_t *imagen, uint32_t version)
{
    pthread_mutex_lock(&dir->mutex);
    size_t i = indice_gpn(dir, gpn);
    int rc = -1;
    if (i < dir->npaginas && proteger(dir, i, PROT_READ) == 0) {
        memcpy(dir->vistas[i].c_r, imagen, dir->page_size);
        dsm_pagina_desc_t *pagina = &dir->paginas[i];
        pagina->msi = DSM_MSI_S;
        pagina->poseedor = DSM_POSEEDOR_NINGUNO;
        pagina->version = version;
        pagina->mascara_copias = (UINT64_C(1) << dir->nodo_id) | (UINT64_C(1) << poseedor) |
                                 (UINT64_C(1) << solicitante);
        pagina->perdida = 0;
        printf("nodo %u reserva %" PRIu32 " gpn %" PRIu64 " MSI M -> S version %" PRIu32 "\n",
               dir->nodo_id, pagina->reserva, pagina->gpn, pagina->version);
        rc = 0;
    }
    pthread_mutex_unlock(&dir->mutex);
    return rc;
}

static int atender_lectura(dsm_directorio_t *dir, uint16_t par, const dsm_cabecera_t *pedido_cab,
                           const dsm_page_request_t *pedido)
{
    int modo = 0;
    uint16_t poseedor = 0;
    uint32_t version = 0;
    pthread_mutex_lock(&dir->mutex);
    size_t i = indice_gpn(dir, pedido->gpn);
    if (hogar_del_gpn(dir, pedido->gpn) != dir->nodo_id) {
        modo = DSM_ESTADO_NO_HOGAR;
    } else if (i == dir->npaginas) {
        modo = DSM_ESTADO_RANGO;
    } else if (dir->paginas[i].perdida) {
        modo = DSM_ESTADO_FUERA;
    } else if (dir->paginas[i].msi == DSM_MSI_M && dir->paginas[i].poseedor == dir->nodo_id) {
        modo = 0;
    } else if (dir->paginas[i].msi == DSM_MSI_S && bit_de(dir->paginas[i].mascara_copias, dir->nodo_id)) {
        modo = 0;
    } else if (dir->paginas[i].msi == DSM_MSI_M && dir->paginas[i].poseedor != dir->nodo_id) {
        modo = 1;
        poseedor = dir->paginas[i].poseedor;
        version = dir->paginas[i].version;
        if ((dir->fuera & (1u << poseedor)) != 0) {
            modo = DSM_ESTADO_FUERA;
        }
    } else {
        modo = DSM_ESTADO_NO_POSEEDOR;
    }
    pthread_mutex_unlock(&dir->mutex);

    if (modo == DSM_ESTADO_FUERA || modo == DSM_ESTADO_NO_HOGAR || modo == DSM_ESTADO_RANGO ||
        modo == DSM_ESTADO_NO_POSEEDOR) {
        return enviar_pagina(dir, par, pedido_cab->id_msg, (uint16_t)modo, NULL);
    }
    if (modo == 0) {
        return publicar_lectura_local(dir, par, pedido_cab, pedido);
    }

    printf("nodo %u PAGE_REQUEST recibido par %u gpn %" PRIu64 " bytes %u id %" PRIu32 "\n",
           dir->nodo_id, par, pedido->gpn, DSM_PAGE_REQUEST_BYTES, pedido_cab->id_msg);
    uint8_t *imagen = malloc(dir->page_size);
    uint32_t version_nueva = 0;
    if (imagen == NULL) {
        return -1;
    }
    int rc = pedir_bajada(dir, poseedor, pedido->gpn, pedido_cab->id_msg, par, DSM_MOTIVO_A_S,
                          version + 1u, imagen, &version_nueva);
    if (rc == 0) {
        rc = instalar_bajada(dir, pedido->gpn, par, poseedor, imagen, version_nueva);
    } else {
        enviar_pagina(dir, par, pedido_cab->id_msg, estado_errno(errno), NULL);
    }
    free(imagen);
    return rc == 0 ? 0 : -1;
}

static int conceder_escritura(dsm_directorio_t *dir, uint16_t par, const dsm_cabecera_t *pedido_cab,
                              const dsm_page_request_t *pedido)
{
    printf("nodo %u PAGE_REQUEST escritura par %u gpn %" PRIu64 " id %" PRIu32 "\n",
           dir->nodo_id, par, pedido->gpn, pedido_cab->id_msg);

    uint16_t fallo = 0;
    uint32_t version = 0;
    uint64_t mascara = 0;
    uint16_t poseedor = DSM_POSEEDOR_NINGUNO;
    uint8_t msi = DSM_MSI_I;
    int ya_exclusivo = 0;
    pthread_mutex_lock(&dir->mutex);
    size_t indice = indice_gpn(dir, pedido->gpn);
    if (hogar_del_gpn(dir, pedido->gpn) != dir->nodo_id) {
        fallo = DSM_ESTADO_NO_HOGAR;
    } else if (indice == dir->npaginas) {
        fallo = DSM_ESTADO_RANGO;
    } else if (dir->paginas[indice].perdida) {
        fallo = DSM_ESTADO_FUERA;
    } else if (dir->paginas[indice].msi == DSM_MSI_M && dir->paginas[indice].poseedor == par) {
        ya_exclusivo = 1;
        version = dir->paginas[indice].version;
    } else {
        version = dir->paginas[indice].version;
        mascara = dir->paginas[indice].mascara_copias;
        poseedor = dir->paginas[indice].poseedor;
        msi = dir->paginas[indice].msi;
        if (version == UINT32_MAX) {
            fallo = DSM_ESTADO_RANGO;
        }
    }
    uint32_t reserva = fallo == 0 && indice < dir->npaginas ? dir->paginas[indice].reserva : 0;
    uint32_t orden = fallo == 0 && indice < dir->npaginas ? dir->paginas[indice].orden : 0;
    pthread_mutex_unlock(&dir->mutex);
    if (fallo != 0) {
        return enviar_pagina(dir, par, pedido_cab->id_msg, fallo, NULL);
    }

    uint8_t *imagen = malloc(dir->page_size);
    if (imagen == NULL) {
        return -1;
    }
    int con_carga = 1;
    int rescatada = 0;
    uint32_t version_datos = version;
    if (ya_exclusivo) {
        con_carga = 0;
    } else if (msi == DSM_MSI_M && poseedor != dir->nodo_id) {
        if ((dir->fuera & (1u << poseedor)) != 0) {
            free(imagen);
            return enviar_pagina(dir, par, pedido_cab->id_msg, DSM_ESTADO_FUERA, NULL);
        }
        if (pedir_bajada(dir, poseedor, pedido->gpn, pedido_cab->id_msg, par, DSM_MOTIVO_A_I, version,
                         imagen, &version_datos) != 0) {
            int error = errno;
            free(imagen);
            return enviar_pagina(dir, par, pedido_cab->id_msg, estado_errno(error), NULL);
        }
        rescatada = 1;
        con_carga = 1;
        mascara &= ~(UINT64_C(1) << poseedor);
    } else if (bit_de(mascara, par) && msi == DSM_MSI_S) {
        con_carga = 0;
    } else {
        pthread_mutex_lock(&dir->mutex);
        size_t i = indice_gpn(dir, pedido->gpn);
        if (i == dir->npaginas) {
            pthread_mutex_unlock(&dir->mutex);
            free(imagen);
            return enviar_pagina(dir, par, pedido_cab->id_msg, DSM_ESTADO_RANGO, NULL);
        }
        memcpy(imagen, dir->vistas[i].c_r, dir->page_size);
        pthread_mutex_unlock(&dir->mutex);
        con_carga = 1;
    }

    if (!ya_exclusivo) {
        for (uint16_t nodo = 0; nodo < dir->cantidad_nodos; nodo++) {
            if (nodo == par || nodo == dir->nodo_id || nodo == poseedor || !bit_de(mascara, nodo)) {
                continue;
            }
            if ((dir->fuera & (1u << nodo)) != 0) {
                continue;
            }
            if (pedir_invalidacion(dir, nodo, pedido->gpn, pedido_cab->id_msg, par) != 0) {
                int error = errno;
                if (rescatada) {
                    adoptar_bytes(dir, pedido->gpn, imagen);
                }
                free(imagen);
                return enviar_pagina(dir, par, pedido_cab->id_msg, estado_errno(error), NULL);
            }
        }
    }

    dsm_page_data_t datos;
    memset(&datos, 0, sizeof datos);
    pthread_mutex_lock(&dir->mutex);
    size_t i = indice_gpn(dir, pedido->gpn);
    int rc = -1;
    if (i < dir->npaginas && dir->paginas[i].version == version) {
        dsm_pagina_desc_t *pagina = &dir->paginas[i];
        if (!ya_exclusivo) {
            if (rescatada) {
                if (proteger(dir, i, PROT_READ | PROT_WRITE) == 0) {
                    memcpy(dir->vistas[i].c_r, imagen, dir->page_size);
                }
            }
            pagina->version++;
            pagina->msi = DSM_MSI_M;
            pagina->poseedor = par;
            pagina->mascara_copias = UINT64_C(1) << par;
            pagina->perdida = 0;
            if (par != dir->nodo_id) {
                proteger(dir, i, PROT_NONE);
            }
        }
        datos = (dsm_page_data_t){
            .gpn = pagina->gpn,
            .proc_id = pedido->proc_id,
            .reserva_id = reserva,
            .orden = orden,
            .hogar = dir->nodo_id,
            .poseedor = par,
            .msi = DSM_MSI_M,
            .permiso = DSM_PAGE_PERMISO_ESCRIBIR,
            .banderas = con_carga ? 0 : DSM_PAGE_DATA_SIN_CARGA,
            .mascara_copias = UINT64_C(1) << par,
            .page_nbytes = con_carga ? dir->page_size : 0,
            .version = pagina->version,
            .datos = con_carga ? imagen : NULL,
        };
        printf("nodo %u reserva %" PRIu32 " gpn %" PRIu64 " MSI -> M poseedor %u version %" PRIu32
               "\n",
               dir->nodo_id, pagina->reserva, pagina->gpn, par, pagina->version);
        rc = 0;
    } else {
        errno = EPROTO;
    }
    pthread_mutex_unlock(&dir->mutex);
    if (rc == 0) {
        rc = enviar_pagina(dir, par, pedido_cab->id_msg, DSM_ESTADO_OK, &datos);
    } else if (rescatada) {
        adoptar_bytes(dir, pedido->gpn, imagen);
        enviar_pagina(dir, par, pedido_cab->id_msg, DSM_ESTADO_CONFLICTO, NULL);
    }
    free(imagen);
    return rc;
}

static int atender_pedido(dsm_directorio_t *dir, uint16_t par, const uint8_t *trama, size_t n)
{
    dsm_cabecera_t cab;
    dsm_page_request_t pedido;
    if (n != DSM_PAGE_REQUEST_BYTES || dsm_page_request_decodificar(trama, n, &cab, &pedido) != 0 ||
        cab.origen != par || (pedido.proc_id >> 16) != par || (uint16_t)pedido.proc_id == 0) {
        errno = EPROTO;
        return -1;
    }
    if (pedido.acceso == DSM_ACCESO_ESCRITURA) {
        return conceder_escritura(dir, par, &cab, &pedido);
    }
    return atender_lectura(dir, par, &cab, &pedido);
}

static int procesar_item(dsm_directorio_t *dir, uint16_t par, const uint8_t *trama, size_t n)
{
    dsm_cabecera_t cab;
    if (dsm_cabecera_decodificar(trama, n < DSM_CABECERA_BYTES ? n : DSM_CABECERA_BYTES, &cab) != 0) {
        errno = EPROTO;
        return -1;
    }
    if (cab.tipo == DSM_TIPO_PAGE_REQUEST) {
        return atender_pedido(dir, par, trama, n);
    }
    if (cab.tipo == DSM_TIPO_INVALIDATE) {
        return atender_invalidate(dir, par, trama, n);
    }
    if (cab.tipo == DSM_TIPO_PAGE_DOWNGRADE) {
        return atender_downgrade(dir, par, trama, n);
    }
    errno = EPROTO;
    return -1;
}

static void *servicio_coherencia(void *arg)
{
    dsm_directorio_t *dir = arg;
    for (;;) {
        pthread_mutex_lock(&dir->mutex);
        while (!dir->servicio_parar && dir->cola_n == 0) {
            pthread_cond_wait(&dir->cola_cv, &dir->mutex);
        }
        if (dir->servicio_parar && dir->cola_n == 0) {
            pthread_mutex_unlock(&dir->mutex);
            return NULL;
        }
        uint16_t par = dir->cola[dir->cola_ini].par;
        uint16_t n = dir->cola[dir->cola_ini].n;
        uint8_t trama[64];
        memcpy(trama, dir->cola[dir->cola_ini].trama, n);
        dir->cola_ini = (dir->cola_ini + 1u) % DSM_DIR_COLA;
        dir->cola_n--;
        pthread_mutex_unlock(&dir->mutex);
        if (procesar_item(dir, par, trama, n) != 0) {
            fprintf(stderr, "nodo %u coherencia: %s\n", dir->nodo_id, strerror(errno));
        }
    }
}

int dsm_directorio_activar(dsm_directorio_t *dir, dsm_directorio_envio_t enviar,
                           dsm_directorio_id_t nuevo_id, void *ctx)
{
    if (dir == NULL || !dir->inicializado || enviar == NULL || nuevo_id == NULL) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&dir->mutex);
    if (dir->servicio_activo) {
        pthread_mutex_unlock(&dir->mutex);
        errno = EALREADY;
        return -1;
    }
    dir->campana_cap = (size_t)dir->page_size + DSM_PAGE_DATA_BASE;
    dir->campana_buf = malloc(dir->campana_cap);
    if (dir->campana_buf == NULL) {
        pthread_mutex_unlock(&dir->mutex);
        return -1;
    }
    dir->enviar = enviar;
    dir->nuevo_id = nuevo_id;
    dir->envio_ctx = ctx;
    dir->servicio_parar = 0;
    dir->cola_ini = 0;
    dir->cola_n = 0;
    dir->campana_espera = 0;
    int error = pthread_create(&dir->servicio_hilo, NULL, servicio_coherencia, dir);
    if (error != 0) {
        free(dir->campana_buf);
        dir->campana_buf = NULL;
        pthread_mutex_unlock(&dir->mutex);
        errno = error;
        return -1;
    }
    dir->servicio_activo = 1;
    pthread_mutex_unlock(&dir->mutex);
    return 0;
}

void dsm_directorio_desactivar(dsm_directorio_t *dir)
{
    if (dir == NULL || !dir->inicializado || !dir->servicio_activo) {
        return;
    }
    pthread_mutex_lock(&dir->mutex);
    dir->servicio_parar = 1;
    if (dir->campana_error == 0) {
        dir->campana_error = ECANCELED;
    }
    pthread_cond_broadcast(&dir->cola_cv);
    pthread_cond_broadcast(&dir->campana_cv);
    pthread_mutex_unlock(&dir->mutex);
    pthread_join(dir->servicio_hilo, NULL);
    pthread_mutex_lock(&dir->mutex);
    dir->servicio_activo = 0;
    dir->servicio_parar = 0;
    dir->cola_n = 0;
    dir->campana_espera = 0;
    free(dir->campana_buf);
    dir->campana_buf = NULL;
    pthread_mutex_unlock(&dir->mutex);
}

int dsm_directorio_ofrecer(dsm_directorio_t *dir, uint16_t par, const uint8_t *trama, size_t n)
{
    if (dir == NULL || !dir->inicializado || !dir->servicio_activo || trama == NULL ||
        n < DSM_CABECERA_BYTES || n > sizeof dir->cola[0].trama) {
        errno = EINVAL;
        return -1;
    }
    dsm_cabecera_t cab;
    if (dsm_cabecera_decodificar(trama, DSM_CABECERA_BYTES, &cab) != 0 || cab.origen != par ||
        cab.destino != dir->nodo_id) {
        errno = EPROTO;
        return -1;
    }
    if (cab.tipo != DSM_TIPO_PAGE_REQUEST && cab.tipo != DSM_TIPO_INVALIDATE &&
        cab.tipo != DSM_TIPO_PAGE_DOWNGRADE) {
        errno = EPROTO;
        return -1;
    }
    pthread_mutex_lock(&dir->mutex);
    if (dir->servicio_parar) {
        pthread_mutex_unlock(&dir->mutex);
        errno = ECANCELED;
        return -1;
    }
    if (dir->cola_n == DSM_DIR_COLA) {
        pthread_mutex_unlock(&dir->mutex);
        if (cab.tipo == DSM_TIPO_PAGE_REQUEST) {
            return enviar_pagina(dir, par, cab.id_msg, DSM_ESTADO_COLA, NULL);
        }
        errno = EAGAIN;
        return -1;
    }
    size_t slot = (dir->cola_ini + dir->cola_n) % DSM_DIR_COLA;
    dir->cola[slot].par = par;
    dir->cola[slot].n = (uint16_t)n;
    memcpy(dir->cola[slot].trama, trama, n);
    dir->cola_n++;
    pthread_cond_signal(&dir->cola_cv);
    pthread_mutex_unlock(&dir->mutex);
    return 0;
}

int dsm_directorio_completar(dsm_directorio_t *dir, uint16_t par, const uint8_t *trama, size_t n)
{
    dsm_cabecera_t cab;
    if (dir == NULL || !dir->inicializado || trama == NULL ||
        dsm_cabecera_decodificar(trama, n, &cab) != 0 || (cab.banderas & DSM_FLAG_RESPUESTA) == 0 ||
        cab.id_corr == 0) {
        return -1;
    }
    pthread_mutex_lock(&dir->mutex);
    if (!dir->campana_espera || par != dir->campana_par || cab.id_corr != dir->campana_id) {
        pthread_mutex_unlock(&dir->mutex);
        return -1;
    }
    if (cab.tipo == DSM_TIPO_PAGE_DATA) {
        if (n > dir->campana_cap) {
            dir->campana_error = EPROTO;
        } else if (!dir->tiene_datos) {
            memcpy(dir->campana_buf, trama, n);
            dir->datos_n = n;
            dir->tiene_datos = 1;
        }
    } else if (cab.tipo == DSM_TIPO_PAGE_DOWNGRADE_ACK || cab.tipo == DSM_TIPO_INVALIDATE_ACK) {
        if (n > sizeof dir->ack_trama) {
            dir->campana_error = EPROTO;
        } else if (!dir->tiene_ack) {
            memset(dir->ack_trama, 0, sizeof dir->ack_trama);
            memcpy(dir->ack_trama, trama, n);
            dir->tiene_ack = 1;
        }
    } else {
        pthread_mutex_unlock(&dir->mutex);
        return -1;
    }
    pthread_cond_signal(&dir->campana_cv);
    pthread_mutex_unlock(&dir->mutex);
    return 0;
}

void dsm_directorio_par_fuera(dsm_directorio_t *dir, uint16_t par)
{
    if (dir == NULL || !dir->inicializado || par >= dir->cantidad_nodos || par == dir->nodo_id) {
        return;
    }
    pthread_mutex_lock(&dir->mutex);
    dir->fuera |= 1u << par;
    for (size_t i = 0; i < dir->npaginas; i++) {
        dsm_pagina_desc_t *pagina = &dir->paginas[i];
        if (pagina->hogar != dir->nodo_id) {
            continue;
        }
        pagina->mascara_copias &= ~(UINT64_C(1) << par);
        if (pagina->poseedor == par && pagina->msi == DSM_MSI_M) {
            pagina->perdida = 1;
        }
    }
    if (dir->campana_espera && dir->campana_par == par && dir->campana_error == 0) {
        dir->campana_error = ENOTCONN;
        pthread_cond_signal(&dir->campana_cv);
    }
    pthread_mutex_unlock(&dir->mutex);
}

void dsm_directorio_par_linea(dsm_directorio_t *dir, uint16_t par)
{
    if (dir == NULL || !dir->inicializado || par >= 32) {
        return;
    }
    pthread_mutex_lock(&dir->mutex);
    dir->fuera &= ~(1u << par);
    pthread_mutex_unlock(&dir->mutex);
}

void dsm_directorio_fijar_retardo(dsm_directorio_t *dir, uint32_t ms)
{
    if (dir == NULL || !dir->inicializado) {
        return;
    }
    pthread_mutex_lock(&dir->mutex);
    dir->retardo_ms = ms;
    pthread_mutex_unlock(&dir->mutex);
}

int dsm_directorio_aplicar_pagina(dsm_directorio_t *dir, const dsm_page_data_t *datos)
{
    if (datos == NULL || (datos->msi != DSM_MSI_S && datos->msi != DSM_MSI_M) || !bloquear(dir)) {
        errno = EINVAL;
        return -1;
    }
    int resultado = -1;
    size_t i = indice_gpn(dir, datos->gpn);
    if (i == dir->npaginas) {
        errno = ENOENT;
        goto salir;
    }
    dsm_pagina_desc_t *pagina = &dir->paginas[i];
    if (datos->reserva_id != pagina->reserva || datos->hogar != pagina->hogar ||
        datos->orden != pagina->orden || datos->gpn != pagina->gpn) {
        errno = EPROTO;
        goto salir;
    }
    int sin_carga = datos->page_nbytes == 0 && datos->banderas == DSM_PAGE_DATA_SIN_CARGA;
    if (datos->version < pagina->version) {
        errno = EPROTO;
        goto salir;
    }
    if (datos->msi == DSM_MSI_M) {
        if (sin_carga) {
            if (pagina->msi != DSM_MSI_S || datos->version != pagina->version + 1u ||
                datos->poseedor != dir->nodo_id || datos->permiso != DSM_PAGE_PERMISO_ESCRIBIR) {
                errno = EPROTO;
                goto salir;
            }
        } else if (datos->version == pagina->version && pagina->msi != DSM_MSI_I) {
            errno = EPROTO;
            goto salir;
        } else if (datos->page_nbytes != dir->page_size || datos->datos == NULL ||
                   datos->permiso != DSM_PAGE_PERMISO_ESCRIBIR) {
            errno = EPROTO;
            goto salir;
        } else if (proteger(dir, i, PROT_READ | PROT_WRITE) != 0) {
            goto salir;
        } else {
            memcpy(dir->vistas[i].c_r, datos->datos, dir->page_size);
        }
        if (sin_carga && proteger(dir, i, PROT_READ | PROT_WRITE) != 0) {
            goto salir;
        }
        pagina->msi = DSM_MSI_M;
        pagina->poseedor = dir->nodo_id;
        pagina->version = datos->version;
        pagina->mascara_copias = datos->mascara_copias;
        resultado = 0;
        goto salir;
    }
    if (sin_carga || datos->permiso != DSM_PAGE_PERMISO_LEER || datos->page_nbytes != dir->page_size ||
        datos->datos == NULL || datos->poseedor != DSM_POSEEDOR_NINGUNO) {
        errno = EPROTO;
        goto salir;
    }
    if (datos->version == pagina->version && pagina->msi == DSM_MSI_S) {
        if (memcmp(dir->vistas[i].c_r, datos->datos, dir->page_size) != 0) {
            errno = EPROTO;
            goto salir;
        }
        resultado = 0;
        goto salir;
    }
    if (datos->version == pagina->version && pagina->msi != DSM_MSI_I) {
        errno = EPROTO;
        goto salir;
    }
    if (proteger(dir, i, PROT_READ) != 0) {
        goto salir;
    }
    memcpy(dir->vistas[i].c_r, datos->datos, dir->page_size);
    pagina->msi = DSM_MSI_S;
    pagina->poseedor = DSM_POSEEDOR_NINGUNO;
    pagina->version = datos->version;
    pagina->mascara_copias = datos->mascara_copias;
    resultado = 0;

salir:
    pthread_mutex_unlock(&dir->mutex);
    return resultado;
}

int dsm_directorio_copiar(dsm_directorio_t *dir, uint64_t gpn, void *imagen, size_t n,
                          dsm_pagina_desc_t *desc)
{
    if (dir == NULL || !dir->inicializado || imagen == NULL || n < dir->page_size) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&dir->mutex);
    size_t i = indice_gpn(dir, gpn);
    if (i == dir->npaginas) {
        pthread_mutex_unlock(&dir->mutex);
        errno = ENOENT;
        return -1;
    }
    dsm_pagina_desc_t pagina = dir->paginas[i];
    int local = !pagina.perdida &&
                (pagina.poseedor == dir->nodo_id ||
                 (pagina.msi == DSM_MSI_S && bit_de(pagina.mascara_copias, dir->nodo_id)) ||
                 (pagina.msi == DSM_MSI_M && pagina.poseedor == dir->nodo_id));
    if (local) {
        memcpy(imagen, dir->vistas[i].c_r, dir->page_size);
        pthread_mutex_unlock(&dir->mutex);
        if (desc != NULL) {
            *desc = pagina;
        }
        return 0;
    }
    int remota = pagina.msi == DSM_MSI_M && pagina.poseedor != dir->nodo_id &&
                 pagina.poseedor != DSM_POSEEDOR_NINGUNO && !pagina.perdida;
    uint16_t poseedor = pagina.poseedor;
    uint32_t version = pagina.version;
    pthread_mutex_unlock(&dir->mutex);
    if (!remota || dir->nuevo_id == NULL) {
        errno = pagina.perdida ? ENOTCONN : EPROTO;
        return -1;
    }
    uint8_t *tmp = malloc(dir->page_size);
    if (tmp == NULL) {
        return -1;
    }
    uint32_t version_nueva = 0;
    uint32_t id_solicitud = dir->nuevo_id(dir->envio_ctx);
    int rc = id_solicitud == 0 ? -1
                              : pedir_bajada(dir, poseedor, gpn, id_solicitud, dir->nodo_id,
                                             DSM_MOTIVO_A_S, version + 1u, tmp, &version_nueva);
    if (rc == 0) {
        rc = instalar_bajada(dir, gpn, dir->nodo_id, poseedor, tmp, version_nueva);
    }
    if (rc == 0) {
        memcpy(imagen, tmp, dir->page_size);
        if (desc != NULL) {
            rc = dsm_directorio_describir(dir, gpn, desc);
        }
    }
    free(tmp);
    return rc;
}

int dsm_directorio_describir(dsm_directorio_t *dir, uint64_t gpn, dsm_pagina_desc_t *desc)
{
    if (desc == NULL || !bloquear(dir)) {
        errno = EINVAL;
        return -1;
    }
    size_t i = indice_gpn(dir, gpn);
    if (i == dir->npaginas) {
        pthread_mutex_unlock(&dir->mutex);
        errno = ENOENT;
        return -1;
    }
    *desc = dir->paginas[i];
    pthread_mutex_unlock(&dir->mutex);
    return 0;
}
