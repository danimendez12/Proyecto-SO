#include "consola_consultas.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static int numero(const char *s, uint64_t maximo, uint64_t *valor)
{
    if (s == NULL || *s < '0' || *s > '9') return DSM_ERR_INVAL;
    char *fin;
    errno = 0;
    uintmax_t n = strtoumax(s, &fin, 10);
    if (errno || *fin || n > maximo) return DSM_ERR_INVAL;
    *valor = (uint64_t)n;
    return DSM_OK;
}

static const char *estado(uint8_t n)
{
    switch (n) {
    case DSM_DISP_VACIO: return "NO_VISTO";
    case DSM_DISP_UNION: return "UNION";
    case DSM_DISP_EN_LINEA: return "EN_LINEA";
    case DSM_DISP_SOSPECHA: return "SOSPECHA";
    case DSM_DISP_FUERA: return "FUERA";
    default: return "DESCONOCIDO";
    }
}

static void nodo_fila(FILE *f, const dsm_nodo_info_t *n)
{
    struct in_addr ip = {.s_addr = htonl(n->ipv4_host)};
    char texto[INET_ADDRSTRLEN] = "?", ocupadas[32] = "?", libres[32] = "?";
    (void)inet_ntop(AF_INET, &ip, texto, sizeof texto);
    if (n->respondio) {
        snprintf(ocupadas, sizeof ocupadas, "%" PRIu32, n->aportados);
        snprintf(libres, sizeof libres, "%" PRIu32, n->libres);
    }
    fprintf(f, "NODO id=%u nombre=%s ipv4=%s puerto=%u estado=%s respuesta=%s"
               " nominal_bytes=%" PRIu64 " totales=%" PRIu32 " ocupadas=%s libres=%s\n",
            n->id, n->nombre, texto, n->puerto, estado(n->estado), n->respondio ? "SI" : "NO",
            n->memoria, n->totales, ocupadas, libres);
}

static int tabla_nodos(dsm_context_t *ctx, const dsm_config_t *cfg,
                        dsm_nodo_info_t **tabla, size_t *cantidad)
{
    *tabla = calloc(cfg->cantidad_nodos, sizeof **tabla);
    if (*tabla == NULL) return DSM_ERR_NOMEM;
    int rc = dsm_consultar_nodos(ctx, *tabla, cfg->cantidad_nodos, cantidad);
    if (rc != DSM_OK) { free(*tabla); *tabla = NULL; }
    return rc;
}

static int memoria(FILE *f, const dsm_config_t *cfg, const dsm_nodo_info_t *n, size_t cantidad)
{
    uint64_t nominal = 0, activa = 0, libres = 0, totales = 0;
    size_t respondieron = 0, computadoras = 0;
    for (size_t i = 0; i < cfg->cantidad_nodos; i++) {
        size_t j;
        for (j = 0; j < i && cfg->nodos[j].ipv4_host != cfg->nodos[i].ipv4_host; j++) {}
        if (j == i) computadoras++;
    }
    for (size_t i = 0; i < cantidad; i++) {
        nominal += n[i].memoria;
        if (!n[i].respondio) continue;
        if (n[i].libres > n[i].totales) return DSM_ERR_PROTO;
        respondieron++;
        activa += n[i].memoria;
        libres += n[i].libres;
        totales += n[i].totales;
    }
    uint64_t ocupadas = totales - libres;
    fprintf(f, "MEMORIA configurados=%zu respondieron=%zu sin_respuesta=%zu computadoras=%zu"
               " nominal_bytes=%" PRIu64 " activa_bytes=%" PRIu64 " ocupada_bytes=%" PRIu64
               " libre_bytes=%" PRIu64 " paginas_totales=%" PRIu64 " paginas_ocupadas=%" PRIu64
               " paginas_libres=%" PRIu64 " utilizacion=%.2f cobertura=%s\n",
            cantidad, respondieron, cantidad - respondieron, computadoras,
            nominal, activa, ocupadas * cfg->page_size, libres * cfg->page_size,
            totales, ocupadas, libres, totales ? 100.0 * (double)ocupadas / (double)totales : 0,
            respondieron == cantidad ? "COMPLETA" : "PARCIAL");
    return DSM_OK;
}

static void contadores(FILE *f, const uint64_t *c)
{
    static const char *const nombres[] = {
        "lectura", "escritura", "inv_env", "inv_rec", "pag_env", "pag_rec", "bytes_env", "bytes_rec"
    };
    for (size_t i = 0; i < DSM_CONSULTA_CONTADORES; i++) fprintf(f, " %s=%" PRIu64, nombres[i], c[i]);
    fputc('\n', f);
}

static char msi(uint8_t n)
{
    return n == 0 ? 'I' : n == 1 ? 'S' : n == 2 ? 'M' : '?';
}

static int paginas(dsm_context_t *ctx, const dsm_config_t *cfg, uint16_t nodo,
                    int argc, char *const argv[], FILE *f, int copias)
{
    uint16_t hogar;
    int pos;
    if (argc < 2) return DSM_ERR_INVAL;
    if (!strcmp(argv[1], "local")) { hogar = nodo; pos = 2; }
    else if (!copias && !strcmp(argv[1], "global")) { hogar = DSM_HOGAR_TODOS; pos = 2; }
    else if (!copias && !strcmp(argv[1], "nodo") && argc >= 3) {
        uint64_t id;
        if (numero(argv[2], cfg->cantidad_nodos - 1u, &id)) return DSM_ERR_INVAL;
        hogar = (uint16_t)id; pos = 3;
    } else return DSM_ERR_INVAL;
    uint64_t desde = 0, limite = 64;
    if (argc > pos + 2 || (argc > pos && numero(argv[pos], UINT64_MAX, &desde)) ||
        (argc > pos + 1 && numero(argv[pos + 1], DSM_CONSULTA_RANGO_MAX, &limite)) || !limite)
        return DSM_ERR_INVAL;
    dsm_pagina_info_t *tabla = calloc((size_t)limite, sizeof *tabla);
    if (!tabla) return DSM_ERR_NOMEM;
    size_t cantidad = 0;
    uint64_t siguiente = desde;
    int mas = 0;
    int rc = dsm_consultar_rango(ctx, hogar, copias, desde, (size_t)limite, tabla,
                                 &cantidad, &siguiente, &mas);
    if (rc == DSM_OK || rc == DSM_ERR_PARCIAL) {
        for (size_t i = 0; i < cantidad; i++) {
            const dsm_pagina_info_t *p = &tabla[i];
            const char *asignacion = p->asignacion == DSM_ASIGNACION_LIBRE ? "LIBRE" :
                                    p->asignacion == DSM_ASIGNACION_PREPARADA ? "PREPARADA" : "ASIGNADA";
            char libres[32] = "?";
            char creador[24] = "-", reserva[24] = "-", orden[24] = "-", solicitante[24] = "-";
            char poseedor[24] = "-", version[24] = "-";
            int libre = p->asignacion == DSM_ASIGNACION_LIBRE;
            if (!libre) {
                snprintf(creador, sizeof creador, "%u", p->creador);
                snprintf(reserva, sizeof reserva, "%" PRIu32, p->reserva);
                snprintf(orden, sizeof orden, "%" PRIu32, p->orden);
                snprintf(solicitante, sizeof solicitante, "%" PRIu32, p->proc);
            }
            if (p->asignacion == DSM_ASIGNACION_ASIGNADA) {
                if (p->poseedor != UINT16_MAX) snprintf(poseedor, sizeof poseedor, "%u", p->poseedor);
                snprintf(version, sizeof version, "%" PRIu32, p->version);
            }
            if (p->memoria_libre != UINT32_MAX) snprintf(libres, sizeof libres, "%" PRIu32, p->memoria_libre);
            fprintf(f, "PAGINA gpn=%" PRIu64 " hogar=%u creador=%s propietario=%" PRIu32
                       " solicitante=%s reserva=%s orden=%s"
                       " asignacion=%s procedencia=%s msi=%c poseedor=%s version=%s"
                       " mascara=%" PRIu64 " permiso=%u libre_hogar=%s valida=%s msi_local=%c\n",
                    p->gpn, p->hogar, creador, p->propietario, solicitante, reserva, orden,
                    asignacion, libre ? "NINGUNA" : p->publica ? "PUBLICA" : "HISTORICA",
                    p->asignacion == DSM_ASIGNACION_ASIGNADA ? msi(p->msi) : '-', poseedor,
                    version, p->mascara, p->permiso, libres, p->permiso ? "SI" : "NO",
                    p->asignacion != DSM_ASIGNACION_ASIGNADA ? '-' : p->permiso ? msi(p->msi) : 'I');
        }
        fprintf(f, "PAGINAS tabla=%s desde=%" PRIu64 " filas=%zu siguiente=%" PRIu64
                   " mas=%s cobertura=%s\n", copias ? "COPIAS" : "HOGAR", desde,
                cantidad, siguiente, mas ? "SI" : "NO", rc == DSM_OK ? "COMPLETA" : "PARCIAL");
        rc = DSM_OK;
    }
    free(tabla);
    return rc;
}

int consola_consultar(dsm_context_t *ctx, const dsm_config_t *cfg, uint16_t nodo,
                       int argc, char *const argv[], FILE *f)
{
    if (!ctx || !cfg || !f || argc <= 0 || !argv || !argv[0] || !cfg->cantidad_nodos ||
        nodo >= cfg->cantidad_nodos) return DSM_ERR_INVAL;
    int es_paginas = !strcmp(argv[0], "paginas"), copias = !strcmp(argv[0], "copias");
    if (es_paginas || copias) {
        int rc = paginas(ctx, cfg, nodo, argc, argv, f, copias);
        return rc == DSM_OK ? (ferror(f) ? DSM_ERR_ESTADO : 1) : rc;
    }
    int es_estado = !strcmp(argv[0], "estado"), es_memoria = !strcmp(argv[0], "memoria");
    int es_nodos = !strcmp(argv[0], "nodos"), es_local = !strcmp(argv[0], "local");
    int estadisticas = !strcmp(argv[0], "estadisticas");
    if (!es_estado && !es_memoria && !es_nodos && !es_local && !estadisticas) return 0;
    if (es_local || (estadisticas && argc > 1)) {
        uint64_t id = nodo;
        if (es_local ? argc != 1 :
            (argc != 3 || strcmp(argv[1], "nodo") || numero(argv[2], cfg->cantidad_nodos - 1u, &id)))
            return DSM_ERR_INVAL;
        dsm_nodo_info_t info;
        int rc = dsm_consultar_nodo(ctx, (uint16_t)id, &info);
        if (rc != DSM_OK) return rc;
        if (es_local) nodo_fila(f, &info);
        else {
            flockfile(f);
            fprintf(f, "ESTADISTICAS nodo=%u", info.id); contadores(f, info.contadores);
            funlockfile(f);
        }
    } else {
        if (argc != 1) return DSM_ERR_INVAL;
        dsm_nodo_info_t *tabla;
        size_t cantidad;
        int rc = tabla_nodos(ctx, cfg, &tabla, &cantidad);
        if (rc != DSM_OK) return rc;
        if (es_estado) fprintf(f, "ESTADO nodo=%u\n", nodo);
        if (es_estado || es_memoria) rc = memoria(f, cfg, tabla, cantidad);
        else if (es_nodos) for (size_t i = 0; i < cantidad; i++) nodo_fila(f, &tabla[i]);
        else {
            uint64_t suma[DSM_CONSULTA_CONTADORES] = {0};
            size_t confirmados = 0;
            for (size_t i = 0; i < cantidad && rc == DSM_OK; i++) {
                if (!tabla[i].respondio) continue;
                confirmados++;
                flockfile(f);
                fprintf(f, "ESTADISTICAS nodo=%u", tabla[i].id); contadores(f, tabla[i].contadores);
                funlockfile(f);
                for (size_t j = 0; j < DSM_CONSULTA_CONTADORES; j++) {
                    if (UINT64_MAX - suma[j] < tabla[i].contadores[j]) { rc = DSM_ERR_ESTADO; break; }
                    suma[j] += tabla[i].contadores[j];
                }
            }
            if (rc == DSM_OK) {
                flockfile(f);
                fprintf(f, "ESTADISTICAS global=SI respondieron=%zu sin_respuesta=%zu cobertura=%s",
                        confirmados, cantidad - confirmados, confirmados == cantidad ? "COMPLETA" : "PARCIAL");
                contadores(f, suma);
                funlockfile(f);
            }
        }
        free(tabla);
        if (rc != DSM_OK) return rc;
    }
    return ferror(f) ? DSM_ERR_ESTADO : 1;
}
