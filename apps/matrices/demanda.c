#include "calculo.h"
#include "config.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { MAGIC = 0x4d584431, MAXP = 40 };

struct anuncio {
    uint16_t n;
    uint16_t participantes[MAXP];
    uint32_t control, a, b, c;
    uint32_t filas, interior, columnas, page_size;
};

struct cabecera {
    uint32_t magic;
    uint32_t generacion;
};

static uint32_t espera_ms(void)
{
    const char *texto = getenv("DSM_DEMANDA_ESPERA_MS");
    unsigned long n = texto == NULL ? 3000 : strtoul(texto, NULL, 10);
    return n == 0 || n > 60000ul ? 3000u : (uint32_t)n;
}

static int comparar_id(const void *a, const void *b)
{
    uint16_t x = *(const uint16_t *)a, y = *(const uint16_t *)b;
    return (x > y) - (x < y);
}

static int reunir(dsm_context_t *ctx, uint16_t yo, const char *clave, uint16_t *ids, size_t *n)
{
    uint16_t propio = yo;
    uint32_t quiet = espera_ms();
    struct timespec inicio, absoluto;
    clock_gettime(CLOCK_MONOTONIC, &inicio);
    absoluto = inicio;
    size_t previos = 0;
    uint16_t antes[MAXP] = {0};
    for (;;) {
        if (dsm_aviso_publicar(ctx, clave, &propio, sizeof propio) != DSM_OK) return DSM_ERR_ESTADO;
        size_t cantidad = 0;
        uint16_t actuales[MAXP];
        int rc = dsm_aviso_participantes(ctx, clave, actuales, MAXP, &cantidad);
        if (rc != DSM_OK) return rc;
        qsort(actuales, cantidad, sizeof actuales[0], comparar_id);
        struct timespec ahora;
        clock_gettime(CLOCK_MONOTONIC, &ahora);
        uint64_t ms = (uint64_t)(ahora.tv_sec - inicio.tv_sec) * 1000u +
                      (uint64_t)(ahora.tv_nsec - inicio.tv_nsec) / 1000000u;
        uint64_t total = (uint64_t)(ahora.tv_sec - absoluto.tv_sec) * 1000u;
        int cambio = cantidad != previos || memcmp(actuales, antes, cantidad * sizeof actuales[0]) != 0;
        if (cambio) {
            inicio = ahora;
            previos = cantidad;
            memcpy(antes, actuales, cantidad * sizeof actuales[0]);
        } else if (cantidad > 0 && ms >= quiet) {
            memcpy(ids, actuales, cantidad * sizeof actuales[0]);
            *n = cantidad;
            return DSM_OK;
        }
        if (total > 30000u) return DSM_ERR_TIEMPO;
        usleep(100000);
    }
}

int matrices_demanda(const char *ini, size_t filas, size_t interior, size_t columnas)
{
    uint16_t puesto = 0;
    int rc = dsm_pool_reclamar(ini, &puesto);
    if (rc != DSM_OK) return rc;
    dsm_context_t *ctx = NULL;
    rc = dsm_iniciar(ini, puesto, &ctx);
    if (rc != DSM_OK) return rc;
    printf("PUESTO id=%u\n", puesto);
    dsm_proc_id_t proc = 0;
    rc = dsm_proc_abrir(ctx, &proc);
    char clave[DSM_ANUNCIO_CLAVE];
    snprintf(clave, sizeof clave, "mx:%zu:%zu:%zu", filas, interior, columnas);
    uint16_t ids[MAXP];
    size_t cantidad = 0;
    if (rc == DSM_OK) rc = reunir(ctx, puesto, clave, ids, &cantidad);
    if (rc != DSM_OK) {
        dsm_cerrar(ctx);
        return rc;
    }
    printf("DEMANDA participantes=%zu\n", cantidad);
    int ganador = ids[0] == puesto;
    struct anuncio anuncio;
    memset(&anuncio, 0, sizeof anuncio);
    dsm_config_t cfg;
    if (dsm_config_cargar(ini, &cfg) != 0) {
        dsm_cerrar(ctx);
        return DSM_ERR_CONFIG;
    }
    matrices_t m = {.filas = filas, .interior = interior, .columnas = columnas, .page_size = cfg.page_size};
    size_t bytes[3];
    if (rc == DSM_OK) rc = matrices_tamanos(&m, bytes);
    dsm_reserva_id_t control = 0;
    char clave_reserva[DSM_ANUNCIO_CLAVE];
    snprintf(clave_reserva, sizeof clave_reserva, "mr:%zu:%zu:%zu", filas, interior, columnas);
    if (ganador && rc == DSM_OK) {
        void *base = NULL;
        rc = dsm_reservar(ctx, proc, cfg.page_size, &control, &base);
        if (rc == DSM_OK) rc = dsm_reservar(ctx, proc, bytes[0], &m.a, &base);
        if (rc == DSM_OK) rc = dsm_reservar(ctx, proc, bytes[1], &m.b, &base);
        if (rc == DSM_OK) rc = dsm_reservar(ctx, proc, bytes[2], &m.c, &base);
        if (rc == DSM_OK) rc = matrices_inicializar(ctx, &m);
        if (rc == DSM_OK) {
            anuncio.n = (uint16_t)cantidad;
            memcpy(anuncio.participantes, ids, cantidad * sizeof ids[0]);
            anuncio.control = control;
            anuncio.a = m.a;
            anuncio.b = m.b;
            anuncio.c = m.c;
            anuncio.filas = (uint32_t)filas;
            anuncio.interior = (uint32_t)interior;
            anuncio.columnas = (uint32_t)columnas;
            anuncio.page_size = cfg.page_size;
            struct cabecera cab = {.magic = MAGIC, .generacion = 1};
            rc = dsm_escribir(ctx, control, 0, &cab, sizeof cab);
        }
        if (rc == DSM_OK) rc = dsm_aviso_publicar(ctx, clave_reserva, &anuncio, sizeof anuncio);
    } else if (rc == DSM_OK) {
        struct timespec inicio;
        clock_gettime(CLOCK_MONOTONIC, &inicio);
        uint16_t autor = 0;
        for (;;) {
            if (dsm_aviso_consultar(ctx, clave_reserva, &autor, &anuncio, sizeof anuncio) == DSM_OK) break;
            struct timespec ahora;
            clock_gettime(CLOCK_MONOTONIC, &ahora);
            if (ahora.tv_sec - inicio.tv_sec > 30) {
                rc = DSM_ERR_TIEMPO;
                break;
            }
            usleep(100000);
        }
        control = anuncio.control;
        m.a = anuncio.a;
        m.b = anuncio.b;
        m.c = anuncio.c;
        void *base = NULL;
        size_t longitud = 0;
        if (rc == DSM_OK) rc = dsm_abrir_reserva(ctx, control, &base, &longitud);
        if (rc == DSM_OK) rc = dsm_abrir_reserva(ctx, m.a, &base, &longitud);
        if (rc == DSM_OK) rc = dsm_abrir_reserva(ctx, m.b, &base, &longitud);
        if (rc == DSM_OK) rc = dsm_abrir_reserva(ctx, m.c, &base, &longitud);
    }
    unsigned rango = MAXP;
    for (uint16_t i = 0; rc == DSM_OK && i < anuncio.n; i++) {
        if (anuncio.participantes[i] == puesto) rango = i;
    }
    if (rc == DSM_OK && rango == MAXP) {
        printf("DEMANDA espectador puesto=%u\n", puesto);
        dsm_cerrar(ctx);
        return DSM_OK;
    }
    size_t inicio = 0, fin = 0, hilos = 1;
    if (rc == DSM_OK && anuncio.n > 0) {
        size_t paginas = 1 + (bytes[2] - 1) / m.page_size;
        inicio = paginas * rango / anuncio.n;
        fin = paginas * (rango + 1u) / anuncio.n;
        hilos = fin - inicio < 2 ? (fin > inicio ? fin - inicio : 1) : 2;
    }
    if (rc == DSM_OK && fin > inicio) rc = matrices_calcular(ctx, proc, &m, inicio, fin, hilos);
    if (rc == DSM_OK) {
        uint32_t hecho = 1;
        rc = dsm_escribir(ctx, control, sizeof(struct cabecera) + rango * sizeof hecho, &hecho, sizeof hecho);
    }
    if (rc == DSM_OK && ganador) {
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (;;) {
            uint32_t marcas[MAXP];
            int lectura = dsm_leer(ctx, control, sizeof(struct cabecera), marcas, anuncio.n * sizeof marcas[0]);
            size_t listas = 0;
            if (lectura == DSM_OK) {
                for (uint16_t i = 0; i < anuncio.n; i++) listas += marcas[i] == 1;
            }
            if (listas == anuncio.n) break;
            struct timespec ahora;
            clock_gettime(CLOCK_MONOTONIC, &ahora);
            if (ahora.tv_sec - t0.tv_sec > 120) {
                rc = DSM_ERR_TIEMPO;
                break;
            }
            usleep(50000);
        }
        if (rc == DSM_OK) rc = matrices_verificar(ctx, &m);
        if (rc == DSM_OK) printf("VERIFICACION OK\n");
        for (int i = 0; i < 50; i++) {
            int a = dsm_liberar(ctx, m.a), b = dsm_liberar(ctx, m.b), c = dsm_liberar(ctx, m.c);
            int d = dsm_liberar(ctx, control);
            if (a != DSM_ERR_OCUPADA && b != DSM_ERR_OCUPADA && c != DSM_ERR_OCUPADA && d != DSM_ERR_OCUPADA) break;
            usleep(100000);
        }
    } else if (rc == DSM_OK) {
        (void)dsm_desvincular_reserva(ctx, m.a);
        (void)dsm_desvincular_reserva(ctx, m.b);
        (void)dsm_desvincular_reserva(ctx, m.c);
        (void)dsm_desvincular_reserva(ctx, control);
    }
    int cierre = dsm_cerrar(ctx);
    return rc == DSM_OK ? cierre : rc;
}
