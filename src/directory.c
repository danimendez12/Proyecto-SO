#include "directory.h"

#include "network.h"

#include <errno.h>
#include <string.h>
#include <sys/mman.h>

int dsm_directorio_init(dsm_directorio_t *dir, const dsm_config_t *cfg, uint16_t nodo_id)
{
    if (dir == NULL) {
        return -1;
    }
    memset(dir, 0, sizeof *dir);
    if (cfg == NULL || cfg->cantidad_nodos > DSM_CONFIG_MAX_NODOS ||
        dsm_config_nodo(cfg, nodo_id) == NULL || cfg->rpc_timeout_ms == 0 ||
        cfg->page_size == 0 || cfg->memoria_inicial == 0 ||
        cfg->memoria_inicial % cfg->page_size != 0 ||
        cfg->memoria_inicial / cfg->page_size > UINT32_MAX ||
        pthread_mutex_init(&dir->mutex, NULL) != 0) {
        return -1;
    }
    dir->nodo_id = nodo_id;
    dir->cantidad_nodos = cfg->cantidad_nodos;
    dir->page_size = cfg->page_size;
    dir->rpc_timeout_ms = cfg->rpc_timeout_ms;
    dir->memoria_inicial = cfg->memoria_inicial;
    dir->paginas_totales = (uint32_t)(cfg->memoria_inicial / cfg->page_size);
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
    for (size_t i = 0; i < dir->nvistas && i < DSM_DIR_MAX_VISTAS; i++) {
        dsm_vistas_cerrar(&dir->vistas[i]);
    }
    pthread_mutex_destroy(&dir->mutex);
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
    uint64_t gpn = (uint64_t)dir->nodo_id * dir->paginas_totales + indice;
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
    /* Por ahora el creador solo reserva una pagina de su propio hogar. */
    if (!id_reserva_valido(dir, info->reserva_id) || info->total_tramos != 1 ||
        info->primer_tramo != 0 || info->cantidad_tramos != 1 || info->total_paginas != 1 ||
        info->tamano_bytes != dir->page_size || tramo->orden_inicial != 0 ||
        tramo->cantidad != 1 || tramo->gpn_inicial / dir->paginas_totales != creador) {
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
    if (creador == dir->nodo_id) {
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
        .hogar = creador,
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

int dsm_directorio_responder(dsm_directorio_t *dir, int fd, uint16_t peer,
                             const dsm_cabecera_t *pedido,
                             const dsm_region_lookup_t *consulta, uint32_t *seq)
{
    if (dir == NULL || !dir->inicializado || pedido == NULL || consulta == NULL ||
        seq == NULL || *seq == 0) {
        errno = EINVAL;
        return -1;
    }
    if (peer >= dir->cantidad_nodos || peer == dir->nodo_id ||
        pedido->version != DSM_PROTO_VERSION || pedido->tipo != DSM_TIPO_REGION_LOOKUP ||
        pedido->banderas != 0 || pedido->origen != peer || pedido->destino != dir->nodo_id ||
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
    dsm_cabecera_t cab;
    memset(&cab, 0, sizeof cab);
    cab.version = DSM_PROTO_VERSION;
    cab.tipo = DSM_TIPO_REGION_INFO;
    cab.banderas = DSM_FLAG_RESPUESTA;
    cab.origen = dir->nodo_id;
    cab.destino = pedido->origen;
    cab.id_msg = (*seq)++;
    cab.id_corr = pedido->id_msg;
    cab.estado = estado;
    uint8_t buf[512];
    size_t n = 0;
    if (estado == DSM_ESTADO_OK) {
        if (dsm_region_info_codificar(buf, sizeof buf, &cab, &info, &n) != 0) {
            return -1;
        }
    } else {
        cab.longitud = DSM_CABECERA_BYTES;
        if (dsm_cabecera_codificar(buf, &cab) != 0) {
            return -1;
        }
        n = DSM_CABECERA_BYTES;
    }
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
    void *gestor = NULL;
    for (size_t i = 0; i < dir->npaginas; i++) {
        if (dir->paginas[i].reserva == reserva_id) {
            gestor = dir->vistas[i].c_r;
            break;
        }
    }
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
