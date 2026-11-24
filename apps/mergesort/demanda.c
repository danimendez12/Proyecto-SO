#include "demanda.h"
#include "datos.h"
#include "orden.h"
#include "../demanda_grupo.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

enum { TIPO_ORDENAR = 1, TIPO_FUSIONAR = 2 };

struct anuncio {
    uint64_t elementos, bloque;
    uint32_t control, a, b, hilos, page_size;
};

static int conservar(int rc, int otro) { return rc == DSM_OK ? otro : rc; }

static int trabajar(dsm_context_t *ctx, dsm_proc_id_t proc, const struct anuncio *a,
                    const demanda_grupo_t *g, const demanda_fase_t *fase)
{
    orden_t orden = {.elementos = (size_t)a->elementos, .page_size = a->page_size,
                     .paginas_bloque = a->bloque / (a->page_size / sizeof(int32_t)), .a = a->a, .b = a->b};
    size_t ancho = fase->tipo == TIPO_ORDENAR ? (size_t)fase->ancho : (size_t)fase->ancho * 2u;
    if (!ancho) return DSM_ERR_PROTO;
    size_t tareas = orden.elementos / ancho + (orden.elementos % ancho != 0);
    size_t resto = tareas % g->cantidad;
    size_t desde = tareas / g->cantidad * g->rango + (g->rango < resto ? g->rango : resto);
    size_t hasta = desde + tareas / g->cantidad + (g->rango < resto);
    printf("TRABAJO nodo=%u ordinal=%u generacion=%u tipo=%u tareas=[%zu,%zu)\n",
           g->yo, g->rango, fase->generacion, fase->tipo, desde, hasta);
    if (desde == hasta) return DSM_OK;
    if (fase->tipo == TIPO_ORDENAR) return orden_bloques(ctx, proc, &orden, desde, hasta, a->hilos);
    if (fase->tipo != TIPO_FUSIONAR || fase->origen > 1) return DSM_ERR_PROTO;
    return orden_fase(ctx, proc, &orden, fase->origen ? a->b : a->a,
                      fase->origen ? a->a : a->b, (size_t)fase->ancho, desde, hasta, a->hilos);
}

static int mapa(dsm_context_t *ctx, const char *nombre, dsm_reserva_id_t id, size_t bytes, size_t pagina)
{
    printf("RESERVA %s id=%" PRIu32 " bytes=%zu paginas=%zu\n", nombre, id, bytes, 1 + (bytes - 1) / pagina);
    return demanda_mapa(ctx, nombre, id, bytes, pagina);
}

int mergesort_demanda(const char *ini, size_t n, const char *patron, uint32_t semilla)
{
    if (!n || n > INT32_MAX || n > SIZE_MAX / sizeof(int32_t)) return DSM_ERR_INVAL;
    const char *patrones[] = {"aleatorio", "inverso", "ordenado", "negativos", "repetidos", "iguales"};
    int valido = 0;
    for (size_t i = 0; i < sizeof patrones / sizeof patrones[0]; i++) valido |= strcmp(patron, patrones[i]) == 0;
    if (!valido) return DSM_ERR_INVAL;
    dsm_config_t cfg;
    if (dsm_config_cargar(ini, &cfg) != 0) return DSM_ERR_CONFIG;
    uint16_t puesto;
    int rc = dsm_pool_reclamar(ini, &puesto);
    if (rc != DSM_OK) return rc;
    dsm_context_t *ctx = NULL;
    rc = dsm_iniciar(ini, puesto, &ctx);
    if (rc != DSM_OK) { dsm_pool_liberar(); return rc; }
    printf("PUESTO id=%u\n", puesto);
    demanda_grupo_t grupo = {0};
    dsm_proc_id_t proc;
    struct anuncio a = {.elementos = n, .page_size = cfg.page_size,
        .bloque = cfg.page_size / sizeof(int32_t), .hilos = cfg.max_hilos >= 2 ? 2 : 1};
    FILE *original = NULL;
    int propia = 0, vinculadas[3] = {0};
    char parametros[160];
    snprintf(parametros, sizeof parametros, "mergesort:%zu:%s:%" PRIu32, n, patron, semilla);
    rc = dsm_proc_abrir(ctx, &proc);
    if (rc == DSM_OK) rc = demanda_grupo_iniciar(&grupo, ctx, puesto, parametros);
    if (rc != DSM_OK) goto terminar;
    propia = puesto == grupo.lider;
    dsm_reserva_id_t *ids[] = {&a.control, &a.a, &a.b};
    size_t longitudes[] = {cfg.page_size, n * sizeof(int32_t), n * sizeof(int32_t)};
    demanda_fase_t fase = {0};
    if (propia) {
        rc = datos_preparar(n, patron, semilla, NULL, &original);
        for (size_t i = 0; i < 3 && rc == DSM_OK; i++) {
            void *base;
            rc = dsm_reservar(ctx, proc, longitudes[i], ids[i], &base);
            vinculadas[i] = *ids[i] != 0;
        }
        if (rc == DSM_OK) rc = datos_inicializar(ctx, a.a, n, original);
        if (rc == DSM_OK) rc = demanda_fase_publicar(&grupo, a.control, &fase);
        if (rc == DSM_OK) rc = demanda_grupo_publicar(&grupo, &a, sizeof a);
        if (rc == DSM_OK) {
            printf("CONTROL id=%" PRIu32 "\n", a.control);
            rc = mapa(ctx, "A", a.a, n * sizeof(int32_t), cfg.page_size);
            if (rc == DSM_OK) rc = mapa(ctx, "B", a.b, n * sizeof(int32_t), cfg.page_size);
        }
    } else {
        rc = demanda_grupo_recibir(&grupo, &a, sizeof a);
        if (rc == DSM_OK && (a.elementos != n || a.page_size != cfg.page_size ||
            a.bloque != cfg.page_size / sizeof(int32_t) || !a.hilos || a.hilos > cfg.max_hilos ||
            a.hilos > ORDEN_HILOS_MAX || !a.control || !a.a || !a.b ||
            a.control == a.a || a.control == a.b || a.a == a.b ||
            (a.control >> 16) != grupo.lider || (a.a >> 16) != grupo.lider || (a.b >> 16) != grupo.lider)) rc = DSM_ERR_PROTO;
        if (rc == DSM_OK) {
            for (size_t i = 0; i < 3 && rc == DSM_OK; i++) {
                void *base;
                size_t longitud;
                rc = dsm_abrir_reserva(ctx, *ids[i], &base, &longitud);
                vinculadas[i] = 1;
                if (rc == DSM_OK && longitud != longitudes[i]) rc = DSM_ERR_PROTO;
            }
        }
    }
    if (rc != DSM_OK) goto terminar;
    rc = demanda_marcar(&grupo, a.control, 1, DSM_OK);
    if (rc == DSM_OK && propia) rc = demanda_marcas_esperar(&grupo, a.control, 1);
    uint32_t generacion = 1, origen = 0;
    uint64_t ancho = a.bloque;
    while (rc == DSM_OK) {
        uint32_t tipo = generacion == 1 ? TIPO_ORDENAR :
                        ancho >= n ? DEMANDA_FASE_CERRAR : TIPO_FUSIONAR;
        fase = (demanda_fase_t){.generacion = generacion, .tipo = tipo,
                                .origen = origen, .ancho = ancho};
        if (propia && tipo == DEMANDA_FASE_CERRAR) rc = datos_verificar(ctx, origen ? a.b : a.a, n, original);
        if (rc == DSM_OK) rc = propia ? demanda_fase_publicar(&grupo, a.control, &fase) :
                                      demanda_fase_esperar(&grupo, a.control, generacion, &fase);
        if (rc == DSM_OK && (fase.tipo != tipo || fase.origen != origen || fase.ancho != ancho)) rc = DSM_ERR_PROTO;
        if (rc != DSM_OK) break;
        if (tipo == DEMANDA_FASE_CERRAR) {
            if (!propia) {
                uint64_t limite = demanda_ahora_ms() + 10000;
                for (size_t i = 3; i-- > 1;) {
                    int otro = demanda_reserva_retirar(ctx, ids[i], 0, limite);
                    rc = conservar(rc, otro);
                    if (otro == DSM_OK) vinculadas[i] = 0;
                }
            }
            if (rc == DSM_OK) rc = demanda_marcar(&grupo, a.control, generacion + 1u, DSM_OK);
            if (rc == DSM_OK && propia) rc = demanda_marcas_esperar(&grupo, a.control, generacion + 1u);
            break;
        }
        rc = trabajar(ctx, proc, &a, &grupo, &fase);
        if (rc == DSM_OK) rc = demanda_marcar(&grupo, a.control, generacion + 1u, DSM_OK);
        if (rc == DSM_OK && propia) rc = demanda_marcas_esperar(&grupo, a.control, generacion + 1u);
        if (tipo == TIPO_FUSIONAR) { origen ^= 1u; ancho *= 2u; }
        generacion++;
    }
terminar:
    if (rc != DSM_OK) demanda_grupo_error(&grupo, rc);
    uint64_t limite = demanda_ahora_ms() + 10000;
    dsm_reserva_id_t *limpiar[] = {&a.b, &a.a, &a.control};
    int indices[] = {2, 1, 0};
    for (size_t i = 0; i < 3; i++) if (vinculadas[indices[i]] && *limpiar[i]) {
        int otro = demanda_reserva_retirar(ctx, limpiar[i], propia, limite);
        if (otro != DSM_OK) fprintf(stderr, "LIMPIEZA merge reserva=%" PRIu32 " DSM=%d\n", *limpiar[i], otro);
        rc = conservar(rc, otro);
    }
    if (original && fclose(original) != 0) rc = conservar(rc, DSM_ERR_ESTADO);
    rc = conservar(rc, demanda_grupo_retirar(&grupo));
    rc = conservar(rc, demanda_contexto_cerrar(&ctx, limite));
    if (rc == DSM_OK) printf("%s puesto=%u\n", propia ? "LIBERACION OK" : "TRABAJADOR OK", puesto);
    return rc;
}
