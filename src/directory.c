#include "directory.h"

#include "network.h"

#include <string.h>

int dsm_directorio_init(dsm_directorio_t *dir, const dsm_config_t *cfg, uint16_t nodo_id)
{
    if (dir == NULL || cfg == NULL || dsm_config_nodo(cfg, nodo_id) == NULL) {
        return -1;
    }
    if (cfg->page_size == 0 || (cfg->memoria_inicial % cfg->page_size) != 0) {
        return -1;
    }
    memset(dir, 0, sizeof *dir);
    dir->nodo_id = nodo_id;
    dir->page_size = cfg->page_size;
    dir->memoria_inicial = cfg->memoria_inicial;
    dir->paginas_totales = (uint32_t)(cfg->memoria_inicial / cfg->page_size);
    dir->paginas_libres = dir->paginas_totales;
    dir->siguiente_local = 1;
    for (size_t i = 0; i < DSM_DIR_MAX_VISTAS; i++) {
        dir->vistas[i].fd = -1;
    }
    return 0;
}

void dsm_directorio_cerrar(dsm_directorio_t *dir)
{
    if (dir == NULL) {
        return;
    }
    for (size_t i = 0; i < dir->nvistas && i < DSM_DIR_MAX_VISTAS; i++) {
        dsm_vistas_cerrar(&dir->vistas[i]);
    }
    memset(dir, 0, sizeof *dir);
}

static uint16_t hogar_de(const dsm_directorio_t *dir, uint64_t gpn)
{
    uint64_t por_nodo = dir->memoria_inicial / dir->page_size;
    return (uint16_t)(gpn / por_nodo);
}

int dsm_directorio_reservar_pagina(dsm_directorio_t *dir, dsm_pagina_desc_t *salida)
{
    if (dir == NULL || dir->paginas_libres == 0 || dir->tiene_manifiesto ||
        dir->npaginas >= DSM_DIR_MAX_PAGINAS || dir->nvistas >= DSM_DIR_MAX_VISTAS) {
        return -1;
    }
    uint64_t por_nodo = dir->memoria_inicial / dir->page_size;
    uint64_t indice = dir->paginas_totales - dir->paginas_libres;
    uint64_t gpn = (uint64_t)dir->nodo_id * por_nodo + indice;
    if (dsm_vistas_crear(dir->page_size, &dir->vistas[dir->nvistas]) != 0) {
        return -1;
    }
    dsm_vistas_t *vista = &dir->vistas[dir->nvistas];
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
    return 0;
}

int dsm_directorio_consultar(const dsm_directorio_t *dir, uint32_t reserva_id,
                             uint32_t primer_tramo, dsm_region_info_t *info, uint16_t *estado)
{
    if (dir == NULL || info == NULL || estado == NULL) {
        return -1;
    }
    if (!dir->tiene_manifiesto || dir->manifiesto.reserva_id != reserva_id ||
        primer_tramo != dir->manifiesto.primer_tramo) {
        *estado = DSM_ESTADO_RANGO;
        memset(info, 0, sizeof *info);
        return 0;
    }
    *info = dir->manifiesto;
    *estado = DSM_ESTADO_OK;
    return 0;
}

int dsm_directorio_instalar(dsm_directorio_t *dir, const dsm_region_info_t *info)
{
    if (dir == NULL || info == NULL) {
        return -1;
    }
    if (info->cantidad_tramos == 0 || info->cantidad_tramos > DSM_MAX_TRAMOS ||
        info->total_paginas == 0 || info->primer_tramo > info->total_tramos ||
        info->cantidad_tramos > info->total_tramos - info->primer_tramo) {
        return -1;
    }
    uint64_t paginas = 0;
    for (uint32_t i = 0; i < info->cantidad_tramos; i++) {
        paginas += info->tramos[i].cantidad;
    }
    if (dir->page_size == 0 || paginas != info->total_paginas ||
        paginas > UINT64_MAX / dir->page_size ||
        info->tamano_bytes != paginas * dir->page_size) {
        return -1;
    }
    if (dir->npaginas + (size_t)paginas > DSM_DIR_MAX_PAGINAS ||
        dir->nvistas >= DSM_DIR_MAX_VISTAS) {
        return -1;
    }

    uint32_t libres = dir->paginas_libres;
    uint32_t globales = dir->reservas_globales;
    uint16_t siguiente = dir->siguiente_local;
    int manifiesto = dir->tiene_manifiesto;

    if (dsm_vistas_crear((size_t)info->tamano_bytes, &dir->vistas[dir->nvistas]) != 0) {
        return -1;
    }
    dsm_vistas_t *vista = &dir->vistas[dir->nvistas];
    size_t puesto = 0;
    for (uint32_t t = 0; t < info->cantidad_tramos; t++) {
        for (uint32_t k = 0; k < info->tramos[t].cantidad; k++) {
            dsm_pagina_desc_t desc;
            memset(&desc, 0, sizeof desc);
            desc.gpn = info->tramos[t].gpn_inicial + k;
            desc.hogar = hogar_de(dir, desc.gpn);
            desc.poseedor = DSM_POSEEDOR_NINGUNO;
            desc.reserva = info->reserva_id;
            desc.msi = DSM_MSI_I;
            desc.version = 0;
            desc.orden = info->tramos[t].orden_inicial + k;
            desc.ubicacion_local = (uint8_t *)vista->a_r + (puesto * dir->page_size);
            dir->paginas[dir->npaginas + puesto] = desc;
            puesto++;
        }
    }
    dir->npaginas += puesto;
    dir->nvistas++;
    dir->paginas_libres = libres;
    dir->reservas_globales = globales;
    dir->siguiente_local = siguiente;
    dir->tiene_manifiesto = manifiesto;
    return 0;
}

static int enviar_info(int fd, dsm_directorio_t *dir, const dsm_cabecera_t *pedido,
                       const dsm_region_lookup_t *consulta, uint32_t *seq)
{
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
    return send_all(fd, buf, n);
}

int dsm_directorio_publicar(dsm_directorio_t *dir, int fd, uint32_t *seq)
{
    if (dir == NULL || seq == NULL || *seq == 0) {
        return -1;
    }
    uint8_t buf[512];
    size_t n = 0;
    dsm_cabecera_t cab;
    dsm_region_lookup_t consulta;
    if (dsm_recibir_trama(fd, buf, sizeof buf, &n) != 0) {
        return -1;
    }
    if (dsm_region_lookup_decodificar(buf, n, &cab, &consulta) != 0) {
        return -1;
    }
    if ((cab.banderas & DSM_FLAG_RESPUESTA) != 0 || cab.destino != dir->nodo_id) {
        return -1;
    }
    return enviar_info(fd, dir, &cab, &consulta, seq);
}

int dsm_directorio_traer(dsm_directorio_t *dir, int fd, uint32_t reserva_id, uint32_t *seq,
                         dsm_cabecera_t *cab_out, dsm_region_info_t *info_out)
{
    if (dir == NULL || seq == NULL || *seq == 0) {
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
    cab.destino = (uint16_t)(reserva_id >> 16);
    cab.id_msg = (*seq)++;
    cab.id_corr = 0;
    cab.estado = DSM_ESTADO_OK;
    uint32_t id_lookup = cab.id_msg;
    uint8_t buf[512];
    size_t n = 0;
    if (dsm_region_lookup_codificar(buf, sizeof buf, &cab, &consulta, &n) != 0 ||
        send_all(fd, buf, n) != 0) {
        return -1;
    }
    if (dsm_recibir_trama(fd, buf, sizeof buf, &n) != 0) {
        return -1;
    }
    dsm_cabecera_t resp;
    dsm_region_info_t info;
    if (dsm_region_info_decodificar(buf, n, &resp, &info) != 0) {
        return -1;
    }
    if (resp.estado != DSM_ESTADO_OK || (resp.banderas & DSM_FLAG_RESPUESTA) == 0 ||
        resp.id_corr != id_lookup || resp.origen != cab.destino) {
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

void *dsm_directorio_c_r(const dsm_directorio_t *dir)
{
    if (dir == NULL || dir->nvistas == 0) {
        return NULL;
    }
    return dir->vistas[0].c_r;
}
