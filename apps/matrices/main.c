#include "calculo.h"
#include "config.h"
#include "control.h"
#include "entrada.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int numero(const char *s, size_t max, size_t *valor)
{
    char *fin;
    errno = 0;
    uintmax_t n = strtoumax(s, &fin, 10);
    if (*s < '0' || *s > '9' || *fin || errno || !n || n > max) return -1;
    *valor = (size_t)n;
    return 0;
}

static double ahora(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) {
        perror("clock_gettime");
        exit(1);
    }
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static int mapa(dsm_context_t *ctx, char nombre, dsm_reserva_id_t id, size_t bytes, size_t pagina)
{
    size_t n = 1 + (bytes - 1) / pagina;
    printf("MATRIZ %c id=%" PRIu32 " bytes=%zu paginas=%zu\n", nombre, id, bytes, n);
    uint16_t anterior = UINT16_MAX;
    uint64_t previo = 0;
    for (size_t i = 0; i < n; i++) {
        dsm_node_id_t hogar;
        uint64_t gpn;
        int rc = dsm_ubicacion_pagina(ctx, id, i, &hogar, &gpn);
        if (rc != DSM_OK) return rc;
        if (hogar != anterior || gpn != previo + 1) {
            printf("TRAMO %c hogar=%u ordinal=%zu GPN=%" PRIu64 "\n", nombre, (unsigned)hogar, i, gpn);
        }
        previo = gpn;
        anterior = hogar;
    }
    return DSM_OK;
}

static int crear(const char *programa, const char *ini, const dsm_config_t *cfg, matrices_t *m,
                 size_t trabajadores, size_t segundos, const entradas_t *archivos)
{
    size_t bytes[3];
    int rc = matrices_tamanos(m, bytes);
    if (rc != DSM_OK) return rc;
    uint64_t capacidad = dsm_config_base(cfg, cfg->cantidad_nodos);
    for (size_t i = 0; i < 3; i++) {
        if (bytes[i] > capacidad) return DSM_ERR_NOMEM;
    }
    size_t paginas_c = 1 + (bytes[2] - 1) / m->page_size;
    if (trabajadores > paginas_c) {
        fprintf(stderr, "C tiene %zu paginas para %zu procesos de calculo\n", paginas_c, trabajadores);
        return DSM_ERR_INVAL;
    }
    struct timespec plazo;
    rc = matrices_control_plazo(segundos, &plazo);
    if (rc != DSM_OK) return rc;
    double inicio = ahora();
    dsm_context_t *ctx;
    rc = dsm_iniciar(ini, 0, &ctx);
    if (rc != DSM_OK) return rc;
    dsm_proc_id_t proc;
    dsm_reserva_id_t control = 0;
    matrices_control_datos_t datos = {
        .filas = m->filas, .interior = m->interior, .columnas = m->columnas,
        .page_size = cfg->page_size, .trabajadores = (uint32_t)trabajadores
    };
    rc = dsm_proc_abrir(ctx, &proc);
    if (rc == DSM_OK) rc = matrices_control_crear(ctx, proc, &datos, &control);
    if (rc == DSM_OK) {
        printf("CONTROL id=%" PRIu32 " trabajadores=%zu A=%zux%zu B=%zux%zu\n", control, trabajadores,
               m->filas, m->interior, m->interior, m->columnas);
        for (size_t i = 1; i <= trabajadores; i++) {
            printf("EJECUTAR %s trabajador \"%s\" %zu %" PRIu32 " %u %zu\n",
                   programa, ini, i, control, cfg->max_hilos < 4 ? (unsigned)cfg->max_hilos : 4u, segundos);
        }
        rc = matrices_control_esperar_listos(ctx, control, &plazo);
    }
    dsm_reserva_id_t *ids[] = {&m->a, &m->b, &m->c};
    void *ar;
    for (size_t i = 0; i < 3 && rc == DSM_OK; i++) {
        rc = dsm_reservar(ctx, proc, bytes[i], ids[i], &ar);
        if (rc == DSM_OK) rc = mapa(ctx, (char)('A' + i), *ids[i], bytes[i], m->page_size);
    }
    if (rc == DSM_OK) {
        double t = ahora();
        rc = archivos ? entradas_publicar(ctx, m, archivos) : matrices_inicializar(ctx, m);
        if (rc == DSM_OK) {
            printf("INICIALIZACION segundos=%.6f bytes_matrices=%zu elementos_C=%zu\n",
                   ahora() - t, bytes[0] + bytes[1] + bytes[2], m->filas * m->columnas);
            datos.a = m->a;
            datos.b = m->b;
            datos.c = m->c;
            rc = matrices_control_publicar(ctx, control, &datos);
        }
    }
    if (rc == DSM_OK) rc = matrices_control_esperar_fin(ctx, control, &plazo);
    if (rc == DSM_OK) {
        double t = ahora();
        rc = archivos ? entradas_verificar(ctx, m, archivos) : matrices_verificar(ctx, m);
        if (rc == DSM_OK) printf("VERIFICACION OK segundos=%.6f\n", ahora() - t);
    }
    if (rc != DSM_OK && control) matrices_control_abortar(ctx, control, rc);
    int limpieza = DSM_OK;
    for (size_t i = 3; i > 0; i--) {
        if (*ids[i - 1]) {
            int estado = matrices_control_liberar(ctx, *ids[i - 1], &plazo);
            if (estado != DSM_OK && limpieza == DSM_OK) limpieza = estado;
        }
    }
    if (control) {
        int estado = matrices_control_liberar(ctx, control, &plazo);
        if (limpieza == DSM_OK) limpieza = estado;
    }
    int cierre = matrices_control_cerrar(ctx, &plazo);
    if (limpieza == DSM_OK) limpieza = cierre;
    if (limpieza != DSM_OK) fprintf(stderr, "Limpieza creador DSM=%d\n", limpieza);
    if (rc == DSM_OK) rc = limpieza;
    if (rc == DSM_OK) printf("LIBERACION OK total_segundos=%.6f\n", ahora() - inicio);
    return rc;
}

static int trabajar(const char *ini, const dsm_config_t *cfg, dsm_node_id_t nodo,
                    dsm_reserva_id_t control, size_t hilos, size_t segundos)
{
    struct timespec plazo;
    int rc = matrices_control_plazo(segundos, &plazo);
    if (rc != DSM_OK) return rc;
    dsm_context_t *ctx;
    rc = dsm_iniciar(ini, nodo, &ctx);
    if (rc != DSM_OK) return rc;
    dsm_proc_id_t proc;
    matrices_control_datos_t datos = {0};
    rc = dsm_proc_abrir(ctx, &proc);
    if (rc == DSM_OK) rc = matrices_control_abrir(ctx, control, nodo, &datos);
    int abierto = rc == DSM_OK;
    if (rc == DSM_OK) rc = matrices_control_listo(ctx, control, nodo);
    if (rc == DSM_OK) {
        printf("ALTA nodo=%u\n", (unsigned)nodo);
        rc = matrices_control_esperar_datos(ctx, control, &datos, &plazo);
    }
    matrices_t m = {
        .filas = datos.filas, .interior = datos.interior, .columnas = datos.columnas,
        .page_size = datos.page_size, .a = datos.a, .b = datos.b, .c = datos.c
    };
    size_t bytes[3];
    if (rc == DSM_OK && (m.page_size != cfg->page_size || nodo > datos.trabajadores)) rc = DSM_ERR_CONFIG;
    if (rc == DSM_OK) rc = matrices_tamanos(&m, bytes);
    dsm_reserva_id_t ids[] = {m.a, m.b, m.c};
    size_t abiertas = 0;
    double t = ahora();
    while (rc == DSM_OK && abiertas < 3) {
        void *ar;
        size_t longitud;
        rc = dsm_abrir_reserva(ctx, ids[abiertas], &ar, &longitud);
        if (rc == DSM_OK) {
            if (longitud != bytes[abiertas]) rc = DSM_ERR_PROTO;
            abiertas++;
        }
    }
    if (rc == DSM_OK) {
        printf("APERTURA nodo=%u segundos=%.6f\n", (unsigned)nodo, ahora() - t);
        size_t np = 1 + (bytes[2] - 1) / m.page_size, w = datos.trabajadores;
        size_t inicio = np * (nodo - 1u) / w, fin = np * nodo / w;
        size_t efectivos = fin - inicio < hilos ? fin - inicio : hilos;
        printf("CALCULO nodo=%u hilos=%zu paginas=[%zu,%zu) solicitados=%zu\n",
               (unsigned)nodo, efectivos, inicio, fin, hilos);
        t = ahora();
        rc = matrices_calcular(ctx, proc, &m, inicio, fin, hilos);
        if (rc == DSM_OK) printf("CALCULO_FIN nodo=%u segundos=%.6f\n", (unsigned)nodo, ahora() - t);
    }
    while (abiertas) {
        int estado = matrices_control_desvincular(ctx, ids[--abiertas], &plazo);
        if (rc == DSM_OK) rc = estado;
    }
    if (abierto) {
        int estado = matrices_control_fin(ctx, control, nodo, rc);
        if (rc == DSM_OK) rc = estado;
        estado = matrices_control_desvincular(ctx, control, &plazo);
        if (rc == DSM_OK) rc = estado;
    }
    int cierre = matrices_control_cerrar(ctx, &plazo);
    if (rc == DSM_OK) rc = cierre;
    if (rc == DSM_OK) printf("TRABAJADOR OK nodo=%u\n", (unsigned)nodo);
    return rc;
}

static int uso(const char *nombre)
{
    fprintf(stderr, "Uso:\n  %s creador INI FILAS INTERIOR COLUMNAS TRABAJADORES [PLAZO_SEG]\n"
            "  %s archivos INI A.tsv B.tsv TRABAJADORES [PLAZO_SEG]\n"
            "  %s trabajador INI NODO ID_CONTROL HILOS [PLAZO_SEG]\n"
            "  %s exportar A|B FILAS COLUMNAS ARCHIVO.tsv\n", nombre, nombre, nombre, nombre);
    return 1;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 6) return uso(argv[0]);
    if (strcmp(argv[1], "exportar") == 0) {
        size_t filas, columnas;
        if (argc != 6 || (strcmp(argv[2], "A") && strcmp(argv[2], "B")) ||
            numero(argv[3], SIZE_MAX, &filas) || numero(argv[4], SIZE_MAX, &columnas)) return uso(argv[0]);
        return entradas_exportar(argv[5], filas, columnas, *argv[2] == 'B') == DSM_OK ? 0 : 1;
    }
    int worker = strcmp(argv[1], "trabajador") == 0;
    int archivos = strcmp(argv[1], "archivos") == 0;
    if (!worker && !archivos && strcmp(argv[1], "creador") != 0) return uso(argv[0]);
    int minimo = worker || archivos ? 6 : 7;
    size_t segundos = 1800;
    if ((argc != minimo && argc != minimo + 1) ||
        (argc > minimo && numero(argv[minimo], INT_MAX, &segundos))) return uso(argv[0]);
    dsm_config_t cfg;
    if (dsm_config_cargar(argv[2], &cfg) != 0) return 1;
    int rc;
    if (worker) {
        size_t nodo, control, hilos;
        if (numero(argv[3], cfg.cantidad_nodos - 1u, &nodo) || numero(argv[4], UINT16_MAX, &control) ||
            numero(argv[5], cfg.max_hilos, &hilos)) return uso(argv[0]);
        rc = trabajar(argv[2], &cfg, (uint16_t)nodo, (uint32_t)control, hilos, segundos);
    } else {
        size_t n;
        matrices_t m = {.page_size = cfg.page_size};
        if (numero(argv[minimo - 1], cfg.cantidad_nodos - 1u, &n)) return uso(argv[0]);
        entradas_t e = {0};
        if (archivos) {
            rc = entradas_cargar(argv[3], argv[4], dsm_config_base(&cfg, cfg.cantidad_nodos), &m, &e);
        } else {
            if (numero(argv[3], SIZE_MAX, &m.filas) || numero(argv[4], SIZE_MAX, &m.interior) ||
                numero(argv[5], SIZE_MAX, &m.columnas)) return uso(argv[0]);
            rc = DSM_OK;
        }
        if (rc == DSM_OK) rc = crear(argv[0], argv[2], &cfg, &m, n, segundos, archivos ? &e : NULL);
        entradas_cerrar(&e);
    }
    if (rc != DSM_OK) fprintf(stderr, "ERROR %s DSM=%d\n", worker ? "trabajador" : "creador", rc);
    return rc == DSM_OK ? 0 : 1;
}
