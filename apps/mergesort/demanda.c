#include "config.h"
#include "datos.h"
#include "orden.h"
#include "protocol.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum {
    DEMANDA_MAGIC = 0x44454D31,
    TIPO_ORDENAR = 1,
    TIPO_FUSIONAR = 2,
    TIPO_CERRAR = 3,
    PARTICIPANTES_MAX = 40
};

struct anuncio {
    uint16_t n;
    uint16_t participantes[PARTICIPANTES_MAX];
    uint32_t control;
    uint32_t a;
    uint32_t b;
    uint32_t elementos;
    uint32_t bloque;
    uint32_t hilos;
    uint32_t page_size;
};

struct fase {
    uint32_t magic;
    uint32_t generacion;
    uint32_t tipo;
    uint32_t origen;
    uint32_t ancho;
};

static uint32_t espera_ms(void)
{
    const char *texto = getenv("DSM_DEMANDA_ESPERA_MS");
    if (texto == NULL || texto[0] < '0' || texto[0] > '9') {
        return 3000;
    }
    unsigned long n = strtoul(texto, NULL, 10);
    return n == 0 || n > 60000ul ? 3000u : (uint32_t)n;
}

static int comparar_id(const void *a, const void *b)
{
    uint16_t x = *(const uint16_t *)a;
    uint16_t y = *(const uint16_t *)b;
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
    uint16_t antes[PARTICIPANTES_MAX];
    for (;;) {
        if (dsm_aviso_publicar(ctx, clave, &propio, sizeof propio) != DSM_OK) {
            return DSM_ERR_ESTADO;
        }
        size_t cantidad = 0;
        uint16_t actuales[PARTICIPANTES_MAX];
        int rc = dsm_aviso_participantes(ctx, clave, actuales, PARTICIPANTES_MAX, &cantidad);
        if (rc != DSM_OK) {
            return rc;
        }
        qsort(actuales, cantidad, sizeof actuales[0], comparar_id);
        struct timespec ahora;
        clock_gettime(CLOCK_MONOTONIC, &ahora);
        uint64_t ms = (uint64_t)(ahora.tv_sec - inicio.tv_sec) * 1000u +
                      (uint64_t)(ahora.tv_nsec - inicio.tv_nsec) / 1000000u;
        int cambio = cantidad != previos ||
                     memcmp(actuales, antes, cantidad * sizeof actuales[0]) != 0;
        if (cambio) {
            inicio = ahora;
            previos = cantidad;
            memcpy(antes, actuales, cantidad * sizeof actuales[0]);
        } else if (cantidad > 0 && ms >= quiet) {
            memcpy(ids, actuales, cantidad * sizeof actuales[0]);
            *n = cantidad;
            return DSM_OK;
        }
        uint64_t total = (uint64_t)(ahora.tv_sec - absoluto.tv_sec) * 1000u;
        if (total > 30000u) {
            memcpy(ids, actuales, cantidad * sizeof actuales[0]);
            *n = cantidad;
            return cantidad > 0 ? DSM_OK : DSM_ERR_TIEMPO;
        }
        usleep(100000);
    }
}

static int leer_fase(dsm_context_t *ctx, dsm_reserva_id_t control, uint32_t esperada, struct fase *fase)
{
    struct timespec inicio;
    clock_gettime(CLOCK_MONOTONIC, &inicio);
    for (;;) {
        int rc = dsm_leer(ctx, control, 0, fase, sizeof *fase);
        if (rc == DSM_OK && fase->magic == DEMANDA_MAGIC && fase->generacion == esperada) {
            return DSM_OK;
        }
        struct timespec ahora;
        clock_gettime(CLOCK_MONOTONIC, &ahora);
        if (ahora.tv_sec - inicio.tv_sec > 60) {
            return DSM_ERR_TIEMPO;
        }
        usleep(50000);
    }
}

static int marcar(dsm_context_t *ctx, dsm_reserva_id_t control, unsigned rango, uint32_t generacion)
{
    size_t offset = sizeof(struct fase) + rango * sizeof(uint32_t);
    return dsm_escribir(ctx, control, offset, &generacion, sizeof generacion);
}

static int marcas_listas(dsm_context_t *ctx, dsm_reserva_id_t control, size_t n, uint32_t generacion)
{
    struct timespec inicio;
    clock_gettime(CLOCK_MONOTONIC, &inicio);
    uint32_t marcas[PARTICIPANTES_MAX];
    for (;;) {
        int rc = dsm_leer(ctx, control, sizeof(struct fase), marcas, n * sizeof marcas[0]);
        if (rc == DSM_OK) {
            size_t listas = 0;
            for (size_t i = 0; i < n; i++) {
                listas += marcas[i] == generacion;
            }
            if (listas == n) {
                return DSM_OK;
            }
        }
        struct timespec ahora;
        clock_gettime(CLOCK_MONOTONIC, &ahora);
        if (ahora.tv_sec - inicio.tv_sec > 120) {
            return DSM_ERR_TIEMPO;
        }
        usleep(50000);
    }
}

static int trabajar_fase(dsm_context_t *ctx, dsm_proc_id_t proc, const struct anuncio *anuncio,
                         unsigned rango, const struct fase *fase)
{
    orden_t orden = {
        .elementos = anuncio->elementos,
        .page_size = anuncio->page_size,
        .paginas_bloque = anuncio->bloque / (anuncio->page_size / sizeof(int32_t)),
        .a = anuncio->a,
        .b = anuncio->b,
    };
    int inicial = fase->tipo == TIPO_ORDENAR;
    size_t ancho = inicial ? fase->ancho : 2u * fase->ancho;
    size_t tareas = orden.elementos / ancho + (orden.elementos % ancho != 0);
    size_t resto = tareas % anuncio->n;
    size_t desde = (tareas / anuncio->n) * rango + (rango < resto ? rango : resto);
    size_t fin = desde + tareas / anuncio->n + (rango < resto);
    if (!inicial && fin > desde) {
        return orden_fase(ctx, proc, &orden, fase->origen ? orden.b : orden.a,
                          fase->origen ? orden.a : orden.b, fase->ancho, desde, fin, anuncio->hilos);
    }
    if (inicial && fin > desde) {
        return orden_bloques(ctx, proc, &orden, desde, fin, anuncio->hilos);
    }
    return DSM_OK;
}

static void mostrar_hogares(dsm_context_t *ctx, const char *nombre, dsm_reserva_id_t id, size_t bytes)
{
    size_t paginas = bytes / 4096 + (bytes % 4096 != 0);
    uint16_t anterior = UINT16_MAX;
    for (size_t i = 0; i < paginas; i++) {
        dsm_node_id_t hogar = 0;
        uint64_t gpn = 0;
        if (dsm_ubicacion_pagina(ctx, id, i, &hogar, &gpn) != DSM_OK) {
            return;
        }
        if (i == 0 || hogar != anterior) {
            printf("HOGAR %s pagina=%zu hogar=%u gpn=%" PRIu64 "\n", nombre, i, hogar, gpn);
        }
        anterior = hogar;
    }
}

int mergesort_demanda(const char *ini, size_t n, const char *patron, uint32_t semilla)
{
    uint16_t puesto = 0;
    int rc = dsm_pool_reclamar(ini, &puesto);
    if (rc != DSM_OK) {
        return rc;
    }
    dsm_context_t *ctx = NULL;
    rc = dsm_iniciar(ini, puesto, &ctx);
    if (rc != DSM_OK) {
        return rc;
    }
    printf("PUESTO id=%u\n", puesto);
    dsm_proc_id_t proc = 0;
    rc = dsm_proc_abrir(ctx, &proc);
    char clave[DSM_ANUNCIO_CLAVE];
    snprintf(clave, sizeof clave, "ms:%zu:%s:%u", n, patron, semilla);
    uint16_t ids[PARTICIPANTES_MAX];
    size_t cantidad = 0;
    if (rc == DSM_OK) {
        rc = reunir(ctx, puesto, clave, ids, &cantidad);
    }
    if (rc != DSM_OK) {
        dsm_cerrar(ctx);
        return rc;
    }
    printf("DEMANDA participantes=%zu\n", cantidad);
    char clave_reserva[DSM_ANUNCIO_CLAVE];
    snprintf(clave_reserva, sizeof clave_reserva, "mr:%zu:%s:%u", n, patron, semilla);
    int ganador = ids[0] == puesto;
    struct anuncio anuncio;
    memset(&anuncio, 0, sizeof anuncio);
    FILE *original = NULL;
    dsm_reserva_id_t control = 0;
    if (ganador) {
        rc = datos_preparar(n, patron, semilla, NULL, &original);
        dsm_config_t cfg;
        if (rc == DSM_OK && dsm_config_cargar(ini, &cfg) != 0) {
            rc = DSM_ERR_CONFIG;
        }
        void *base = NULL;
        size_t bytes = n * sizeof(int32_t);
        uint32_t bloque = 0;
        uint32_t hilos = 1;
        if (rc == DSM_OK) {
            bloque = cfg.page_size / (uint32_t)sizeof(int32_t);
            hilos = cfg.max_hilos >= 2 ? 2 : 1;
            rc = dsm_reservar(ctx, proc, cfg.page_size, &control, &base);
        }
        if (rc == DSM_OK) {
            rc = dsm_reservar(ctx, proc, bytes, &anuncio.a, &base);
        }
        if (rc == DSM_OK) {
            rc = dsm_reservar(ctx, proc, bytes, &anuncio.b, &base);
        }
        if (rc == DSM_OK) {
            rc = datos_inicializar(ctx, anuncio.a, n, original);
        }
        if (rc == DSM_OK) {
            anuncio.n = (uint16_t)cantidad;
            memcpy(anuncio.participantes, ids, cantidad * sizeof ids[0]);
            anuncio.control = control;
            anuncio.elementos = (uint32_t)n;
            anuncio.bloque = bloque;
            anuncio.hilos = hilos;
            anuncio.page_size = cfg.page_size;
            struct fase cero = {.magic = DEMANDA_MAGIC};
            rc = dsm_escribir(ctx, control, 0, &cero, sizeof cero);
        }
        if (rc == DSM_OK) {
            rc = dsm_aviso_publicar(ctx, clave_reserva, &anuncio, sizeof anuncio);
            mostrar_hogares(ctx, "A", anuncio.a, bytes);
            mostrar_hogares(ctx, "B", anuncio.b, bytes);
        }
    } else {
        struct timespec inicio;
        clock_gettime(CLOCK_MONOTONIC, &inicio);
        uint16_t autor = 0;
        while (rc == DSM_OK) {
            rc = dsm_aviso_consultar(ctx, clave_reserva, &autor, &anuncio, sizeof anuncio);
            if (rc == DSM_OK) {
                break;
            }
            struct timespec ahora;
            clock_gettime(CLOCK_MONOTONIC, &ahora);
            if (ahora.tv_sec - inicio.tv_sec > 30) {
                rc = DSM_ERR_TIEMPO;
                break;
            }
            rc = DSM_OK;
            usleep(100000);
        }
    }
    unsigned rango = PARTICIPANTES_MAX;
    for (uint16_t i = 0; rc == DSM_OK && i < anuncio.n; i++) {
        if (anuncio.participantes[i] == puesto) {
            rango = i;
        }
    }
    if (rc == DSM_OK) {
        control = anuncio.control;
    }
    if (rc == DSM_OK && rango == PARTICIPANTES_MAX) {
        printf("DEMANDA espectador puesto=%u\n", puesto);
        if (original != NULL) {
            fclose(original);
        }
        dsm_cerrar(ctx);
        return DSM_OK;
    }
    if (!ganador && rc == DSM_OK) {
        void *base = NULL;
        size_t longitud = 0;
        rc = dsm_abrir_reserva(ctx, control, &base, &longitud);
        if (rc == DSM_OK) {
            rc = dsm_abrir_reserva(ctx, anuncio.a, &base, &longitud);
        }
        if (rc == DSM_OK) {
            rc = dsm_abrir_reserva(ctx, anuncio.b, &base, &longitud);
        }
    }
    uint32_t generacion = 1;
    uint32_t origen = 0;
    uint32_t ancho = anuncio.bloque;
    while (rc == DSM_OK) {
        uint32_t tipo = generacion == 1 ? TIPO_ORDENAR :
                        ancho >= anuncio.elementos ? TIPO_CERRAR : TIPO_FUSIONAR;
        struct fase fase = {
            .magic = DEMANDA_MAGIC,
            .generacion = generacion,
            .tipo = tipo,
            .origen = origen,
            .ancho = ancho >= anuncio.elementos ? anuncio.elementos : ancho,
        };
        if (ganador && tipo == TIPO_CERRAR && rc == DSM_OK) {
            rc = datos_verificar(ctx, origen ? anuncio.b : anuncio.a, n, original);
        }
        if (rc == DSM_OK && ganador) {
            rc = dsm_escribir(ctx, control, 0, &fase, sizeof fase);
        } else if (rc == DSM_OK) {
            rc = leer_fase(ctx, control, generacion, &fase);
        }
        if (rc != DSM_OK) {
            break;
        }
        if (fase.tipo == TIPO_CERRAR) {
            if (!ganador) {
                (void)dsm_desvincular_reserva(ctx, anuncio.a);
                (void)dsm_desvincular_reserva(ctx, anuncio.b);
            }
            rc = marcar(ctx, control, rango, generacion);
            if (rc == DSM_OK && ganador) {
                rc = marcas_listas(ctx, control, anuncio.n, generacion);
            }
            break;
        }
        if (ganador || fase.generacion == generacion) {
            rc = trabajar_fase(ctx, proc, &anuncio, rango, &fase);
            if (rc == DSM_OK) {
                rc = marcar(ctx, control, rango, generacion);
            }
        }
        if (rc == DSM_OK && ganador) {
            rc = marcas_listas(ctx, control, anuncio.n, generacion);
        }
        if (fase.tipo == TIPO_FUSIONAR) {
            origen ^= 1;
            ancho *= 2;
        }
        generacion++;
        if (fase.tipo == TIPO_ORDENAR) {
            printf("BLOQUES puesto=%u ancho=%u\n", puesto, fase.ancho);
        }
    }
    if (original != NULL) {
        fclose(original);
    }
    if (ganador) {
        for (int i = 0; i < 50; i++) {
            int una = control ? dsm_liberar(ctx, control) : DSM_OK;
            int dos = anuncio.a ? dsm_liberar(ctx, anuncio.a) : DSM_OK;
            int tres = anuncio.b ? dsm_liberar(ctx, anuncio.b) : DSM_OK;
            if (una != DSM_ERR_OCUPADA && dos != DSM_ERR_OCUPADA && tres != DSM_ERR_OCUPADA) {
                break;
            }
            usleep(100000);
        }
    } else if (control) {
        (void)dsm_desvincular_reserva(ctx, control);
    }
    int cierre = DSM_ERR_OCUPADA;
    for (int i = 0; i < 100 && cierre == DSM_ERR_OCUPADA; i++) {
        cierre = dsm_cerrar(ctx);
        if (cierre == DSM_ERR_OCUPADA) {
            usleep(100000);
        }
    }
    return rc == DSM_OK ? cierre : rc;
}
