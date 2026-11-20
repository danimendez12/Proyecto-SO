#include "config.h"
#include "control.h"
#include "datos.h"
#include "orden.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t techo(size_t n, size_t d) { return n / d + (n % d != 0); }

static int numero(const char *s, uint64_t min, uint64_t max, uint64_t *valor)
{
    if (!s || *s < '0' || *s > '9') return 0;
    char *fin;
    errno = 0;
    uintmax_t v = strtoumax(s, &fin, 10);
    if (errno || *fin || v < min || v > max) return 0;
    *valor = v;
    return 1;
}

static int conservar_error(int anterior, int nuevo)
{
    return anterior == DSM_OK ? nuevo : anterior;
}

/* Se consulta cada página; se condensa la salida solo cuando el manifiesto
 * confirma continuidad física y lógica. El control también consume capacidad. */
static int mostrar_mapa(dsm_context_t *ctx, const char *nombre, dsm_reserva_id_t id,
                        size_t bytes, size_t pagina, size_t usados[DSM_CONFIG_MAX_NODOS])
{
    size_t paginas = techo(bytes, pagina);
    printf("RESERVA %s id=%" PRIu32 " bytes=%zu paginas=%zu\n", nombre, id, bytes, paginas);
    dsm_node_id_t ultimo = UINT16_MAX;
    uint64_t anterior = 0;
    for (size_t i = 0; i < paginas; ++i) {
        dsm_node_id_t hogar;
        uint64_t gpn;
        int rc = dsm_ubicacion_pagina(ctx, id, i, &hogar, &gpn);
        if (rc != DSM_OK) return rc;
        if (hogar >= DSM_CONFIG_MAX_NODOS) return DSM_ERR_PROTO;
        if (!i || hogar != ultimo || gpn != anterior + 1)
            printf("TRAMO %s pagina=%zu hogar=%u gpn=%" PRIu64 "\n", nombre, i, hogar, gpn);
        usados[hogar] += pagina;
        ultimo = hogar;
        anterior = gpn;
    }
    return DSM_OK;
}

static int lanzar(dsm_context_t *ctx, dsm_reserva_id_t control, mergesort_control_fase_t *fase,
                  uint32_t tipo, uint32_t origen, uint64_t ancho, const struct timespec *plazo)
{
    fase->generacion++;
    fase->tipo = tipo;
    fase->origen = origen;
    fase->ancho = ancho;
    int rc = mergesort_control_publicar_fase(ctx, control, fase);
    if (rc == DSM_OK) rc = mergesort_control_esperar(ctx, control, fase->generacion, plazo);
    return rc;
}

static int crear(const char *programa, const char *ini, const dsm_config_t *cfg, size_t n,
                 unsigned workers, unsigned hilos, const char *patron, uint32_t semilla,
                 const char *archivo, const struct timespec *plazo)
{
    double t0 = datos_reloj();
    size_t bytes = n * sizeof(int32_t), pagina = cfg->page_size;
    size_t paginas = techo(bytes, pagina), control_bytes = (1 + workers) * pagina;
    uint64_t necesario = (uint64_t)2 * paginas * pagina + control_bytes;
    if (workers >= cfg->cantidad_nodos || hilos > cfg->max_hilos ||
        cfg->nodos[0].memoria < control_bytes ||
        necesario > dsm_config_base(cfg, cfg->cantidad_nodos)) return DSM_ERR_CONFIG;
    size_t paginas_bloque = techo(paginas, workers * hilos);
    if (paginas_bloque > ORDEN_PAGINAS_BLOQUE_MAX) paginas_bloque = ORDEN_PAGINAS_BLOQUE_MAX;
    mergesort_control_datos_t d = {
        .elementos = n, .bloque = paginas_bloque * pagina / sizeof(int32_t),
        .trabajadores = workers, .hilos = hilos, .page_size = cfg->page_size
    };
    FILE *original = NULL;
    int rc = datos_preparar(n, patron, semilla, archivo, &original);
    if (rc != DSM_OK) return rc;
    printf("ENTRADA elementos=%zu patron=%s semilla=%" PRIu32 " generacion_s=%.6f\n",
           n, patron, semilla, datos_reloj() - t0);
    dsm_context_t *ctx;
    rc = dsm_iniciar(ini, 0, &ctx);
    if (rc != DSM_OK) { fclose(original); return rc; }
    dsm_proc_id_t proc;
    dsm_reserva_id_t control = 0;
    rc = dsm_proc_abrir(ctx, &proc);
    if (rc == DSM_OK) rc = mergesort_control_crear(ctx, proc, &d, &control);
    if (rc == DSM_OK) {
        printf("CONTROL id=%" PRIu32 " elementos=%zu trabajadores=%u hilos=%u\n",
               control, n, workers, hilos);
        for (unsigned i = 1; i <= workers; ++i)
            printf("EJECUTAR %s trabajar \"%s\" %u %" PRIu32 "\n", programa, ini, i, control);
        rc = mergesort_control_esperar(ctx, control, 0, plazo);
    }
    double inicio = datos_reloj();
    size_t usados[DSM_CONFIG_MAX_NODOS] = {0};
    if (rc == DSM_OK) rc = mostrar_mapa(ctx, "control", control, control_bytes, pagina, usados);
    void *vista;
    if (rc == DSM_OK) rc = dsm_reservar(ctx, proc, bytes, &d.a, &vista);
    if (rc == DSM_OK) rc = mostrar_mapa(ctx, "A", d.a, bytes, pagina, usados);
    if (rc == DSM_OK) rc = dsm_reservar(ctx, proc, bytes, &d.b, &vista);
    if (rc == DSM_OK) rc = mostrar_mapa(ctx, "B", d.b, bytes, pagina, usados);
    if (rc == DSM_OK) rc = datos_inicializar(ctx, d.a, n, original);
    if (rc == DSM_OK) {
        for (unsigned i = 0; i < cfg->cantidad_nodos; ++i)
            if (usados[i]) printf("HOGAR nodo=%u bytes=%zu\n", i, usados[i]);
        printf("INICIALIZACION elementos=%zu bytes_dsm=%" PRIu64 " segundos=%.6f\n",
               n, necesario, datos_reloj() - inicio);
        rc = mergesort_control_publicar_datos(ctx, control, &d);
    }
    mergesort_control_fase_t fase = {0};
    uint32_t origen = 0;
    uint64_t ancho = d.bloque;
    unsigned niveles = 0;
    double calculo = datos_reloj();
    if (rc == DSM_OK)
        rc = lanzar(ctx, control, &fase, MERGESORT_CONTROL_ORDENAR, origen, ancho, plazo);
    double local = datos_reloj() - calculo;
    if (rc == DSM_OK) printf("BLOQUES ancho=%" PRIu64 " segundos=%.6f\n", ancho, local);
    double fusion = datos_reloj();
    while (rc == DSM_OK && ancho < n) {
        rc = lanzar(ctx, control, &fase, MERGESORT_CONTROL_FUSIONAR, origen, ancho, plazo);
        if (rc == DSM_OK) {
            ++niveles;
            origen ^= 1;
            printf("FUSION nivel=%u ancho_entrada=%" PRIu64 " salida=%c\n",
                   niveles, ancho, origen ? 'B' : 'A');
            ancho *= 2;
        }
    }
    if (rc == DSM_OK) {
        printf("CALCULO bloques_s=%.6f fusion_s=%.6f fases_fusion=%u\n",
               local, datos_reloj() - fusion, niveles);
        rc = lanzar(ctx, control, &fase, MERGESORT_CONTROL_CERRAR, origen, 0, plazo);
    }
    if (rc == DSM_OK) rc = datos_verificar(ctx, origen ? d.b : d.a, n, original);
    if (fclose(original) && rc == DSM_OK) rc = DSM_ERR_ESTADO;
    if (rc != DSM_OK && control) (void)mergesort_control_abortar(ctx, control, rc);
    dsm_reserva_id_t ids[] = {d.b, d.a, control};
    int retiro = DSM_OK;
    for (size_t j = 0; j < sizeof ids / sizeof ids[0]; ++j)
        if (ids[j]) retiro = conservar_error(retiro, mergesort_control_liberar(ctx, ids[j], plazo));
    retiro = conservar_error(retiro, mergesort_control_cerrar(ctx, plazo));
    printf("LIBERACION %s total_s=%.6f\n", retiro == DSM_OK ? "OK" : "ERROR", datos_reloj() - t0);
    return conservar_error(rc, retiro);
}

/* Con sobrantes, los primeros nodos reciben una tarea adicional. La unidad
 * repartida es un bloque o una pareja de bloques, nunca una fracción de página. */
static int ejecutar_fase(dsm_context_t *ctx, dsm_proc_id_t proc, unsigned indice,
                         const mergesort_control_datos_t *d, const mergesort_control_fase_t *f)
{
    orden_t o = {.elementos = d->elementos, .page_size = d->page_size,
                 .paginas_bloque = d->bloque / (d->page_size / sizeof(int32_t)),
                 .a = d->a, .b = d->b};
    int inicial = f->tipo == MERGESORT_CONTROL_ORDENAR;
    size_t ancho = inicial ? f->ancho : 2 * f->ancho;
    size_t tareas = techo(o.elementos, ancho), resto = tareas % d->trabajadores;
    size_t desde = (tareas / d->trabajadores) * indice + (indice < resto ? indice : resto);
    size_t fin = desde + tareas / d->trabajadores + (indice < resto);
    size_t inicio_elem = desde * ancho, fin_elem = fin * ancho;
    size_t activos = fin - desde < d->hilos ? fin - desde : d->hilos;
    if (inicio_elem > o.elementos) inicio_elem = o.elementos;
    if (fin_elem > o.elementos) fin_elem = o.elementos;
    double t = datos_reloj();
    int rc = DSM_OK;
    if (activos) {
        if (inicial) rc = orden_bloques(ctx, proc, &o, desde, fin, d->hilos);
        else rc = orden_fase(ctx, proc, &o, f->origen ? o.b : o.a, f->origen ? o.a : o.b,
                             f->ancho, desde, fin, d->hilos);
    }
    printf("TRABAJO nodo=%u gen=%" PRIu64 " tipo=%s elementos=[%zu,%zu) hilos=%zu segundos=%.6f rc=%d\n",
           indice + 1, f->generacion, inicial ? "ordenar" : "fusionar", inicio_elem, fin_elem,
           activos, datos_reloj() - t, rc);
    return rc;
}

static int trabajar(const char *ini, const dsm_config_t *cfg, unsigned nodo,
                    dsm_reserva_id_t control, const struct timespec *plazo)
{
    if (nodo >= cfg->cantidad_nodos) return DSM_ERR_CONFIG;
    dsm_context_t *ctx;
    double t0 = datos_reloj();
    int rc = dsm_iniciar(ini, nodo, &ctx);
    if (rc != DSM_OK) return rc;
    dsm_proc_id_t proc;
    unsigned indice = nodo - 1;
    mergesort_control_datos_t d = {0};
    rc = dsm_proc_abrir(ctx, &proc);
    if (rc == DSM_OK) rc = mergesort_control_abrir(ctx, control, indice, &d);
    int vinculado = rc == DSM_OK;
    if (rc == DSM_OK && (d.hilos > cfg->max_hilos || d.page_size != cfg->page_size))
        rc = DSM_ERR_CONFIG;
    if (vinculado) rc = conservar_error(rc, mergesort_control_marcar(ctx, control, indice, 0, rc));
    mergesort_control_fase_t f = {0};
    dsm_reserva_id_t abiertas[2] = {0};
    unsigned cantidad = 0;
    while (rc == DSM_OK) {
        rc = mergesort_control_esperar_fase(ctx, control, f.generacion, &f, plazo);
        if (rc != DSM_OK || f.tipo == MERGESORT_CONTROL_CERRAR) break;
        if (f.tipo == MERGESORT_CONTROL_ORDENAR) {
            double t = datos_reloj();
            rc = mergesort_control_datos(ctx, control, &d);
            dsm_reserva_id_t ids[] = {d.a, d.b};
            for (unsigned i = 0; i < 2 && rc == DSM_OK; ++i) {
                void *base;
                size_t longitud;
                rc = dsm_abrir_reserva(ctx, ids[i], &base, &longitud);
                if (rc == DSM_OK) {
                    abiertas[cantidad++] = ids[i];
                    if (longitud != d.elementos * sizeof(int32_t)) rc = DSM_ERR_PROTO;
                }
            }
            if (rc == DSM_OK) printf("APERTURA nodo=%u segundos=%.6f\n", nodo, datos_reloj() - t);
        }
        if (rc == DSM_OK) rc = ejecutar_fase(ctx, proc, indice, &d, &f);
        rc = conservar_error(rc, mergesort_control_marcar(ctx, control, indice, f.generacion, rc));
    }
    while (cantidad)
        rc = conservar_error(rc, mergesort_control_desvincular(ctx, abiertas[--cantidad], plazo));
    if (vinculado) {
        if (f.tipo == MERGESORT_CONTROL_CERRAR)
            rc = conservar_error(rc, mergesort_control_marcar(ctx, control, indice, f.generacion, rc));
        rc = conservar_error(rc, mergesort_control_desvincular(ctx, control, plazo));
    }
    rc = conservar_error(rc, mergesort_control_cerrar(ctx, plazo));
    if (rc == DSM_OK) printf("TRABAJADOR OK nodo=%u total_s=%.6f\n", nodo, datos_reloj() - t0);
    return rc;
}

static int uso(const char *programa)
{
    fprintf(stderr, "Uso:\n  %s crear INI N TRABAJADORES HILOS PATRON SEMILLA [ARCHIVO]\n"
            "  %s trabajar INI NODO ID_CONTROL\n  %s secuencial N PATRON SEMILLA [ARCHIVO]\n"
            "Patrones: aleatorio, inverso, ordenado, negativos, repetidos, iguales, archivo\n",
            programa, programa, programa);
    return 2;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 2) return uso(argv[0]);
    uint64_t n, semilla, workers, hilos, nodo, id, segundos = 1800;
    int sec = !strcmp(argv[1], "secuencial");
    int worker = !strcmp(argv[1], "trabajar");
    int creator = !strcmp(argv[1], "crear");
    if ((sec && argc != 5 && argc != 6) || (worker && argc != 5) ||
        (creator && argc != 8 && argc != 9) || (!sec && !creator && !worker)) return uso(argv[0]);
    if (!worker && (!numero(argv[sec ? 2 : 3], 1, INT32_MAX, &n) ||
                    n > SIZE_MAX / sizeof(int32_t) ||
                    !numero(argv[sec ? 4 : 7], 0, UINT32_MAX, &semilla))) return uso(argv[0]);
    int rc;
    if (sec) {
        FILE *original;
        rc = datos_preparar(n, argv[3], semilla, argc == 6 ? argv[5] : NULL, &original);
        if (rc == DSM_OK) {
            rc = datos_secuencial(n, original);
            if (fclose(original) && rc == DSM_OK) rc = DSM_ERR_ESTADO;
        }
    } else {
        dsm_config_t cfg;
        struct timespec plazo;
        const char *limite = getenv("MERGESORT_PLAZO_SEG");
        if (limite && !numero(limite, 1, INT_MAX, &segundos)) return uso(argv[0]);
        if (dsm_config_cargar(argv[2], &cfg) != 0) return 1;
        rc = mergesort_control_plazo(segundos, &plazo);
        if (rc == DSM_OK && creator) {
            if (!numero(argv[4], 1, DSM_CONFIG_MAX_NODOS - 1, &workers) ||
                !numero(argv[5], 1, ORDEN_HILOS_MAX, &hilos)) return uso(argv[0]);
            rc = crear(argv[0], argv[2], &cfg, n, workers, hilos, argv[6], semilla,
                       argc == 9 ? argv[8] : NULL, &plazo);
        } else if (rc == DSM_OK) {
            if (!numero(argv[3], 1, DSM_CONFIG_MAX_NODOS - 1, &nodo) ||
                !numero(argv[4], 1, UINT32_MAX, &id)) return uso(argv[0]);
            rc = trabajar(argv[2], &cfg, nodo, id, &plazo);
        }
    }
    if (rc != DSM_OK) fprintf(stderr, "ERROR %s DSM=%d\n", argv[1], rc);
    return rc == DSM_OK ? 0 : 1;
}
