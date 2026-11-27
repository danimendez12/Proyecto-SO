#include "demanda.h"
#include "calculo.h"
#include "../demanda_grupo.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

struct anuncio {
    uint64_t filas, interior, columnas;
    uint32_t control, a, b, c, page_size;
};

static int conservar(int rc, int otro) { return rc == DSM_OK ? otro : rc; }

static int mapa(dsm_context_t *ctx, const char *nombre, dsm_reserva_id_t id,
                 size_t bytes, size_t pagina)
{
    printf("MATRIZ %s id=%" PRIu32 " bytes=%zu paginas=%zu\n", nombre, id, bytes,
           1 + (bytes - 1) / pagina);
    return demanda_mapa(ctx, nombre, id, bytes, pagina);
}

int matrices_demanda(const char *ini, size_t filas, size_t interior, size_t columnas)
{
    dsm_config_t cfg;
    if (dsm_config_cargar(ini, &cfg) != 0) return DSM_ERR_CONFIG;
    matrices_t m = {.filas = filas, .interior = interior, .columnas = columnas,
                    .page_size = cfg.page_size};
    size_t bytes[3];
    int rc = matrices_tamanos(&m, bytes);
    if (rc != DSM_OK) return rc;
    uint16_t puesto;
    rc = dsm_pool_reclamar(ini, &puesto);
    if (rc != DSM_OK) return rc;
    dsm_context_t *ctx = NULL;
    rc = dsm_iniciar(ini, puesto, &ctx);
    if (rc != DSM_OK) { dsm_pool_liberar(); return rc; }
    printf("PUESTO id=%u\n", puesto);
    demanda_grupo_t grupo = {0};
    dsm_proc_id_t proc;
    dsm_reserva_id_t control = 0;
    int propia = 0, vinculadas[4] = {0};
    char parametros[128];
    snprintf(parametros, sizeof parametros, "matrices:%zu:%zu:%zu", filas, interior, columnas);
    rc = dsm_proc_abrir(ctx, &proc);
    if (rc == DSM_OK) rc = demanda_grupo_iniciar(&grupo, ctx, puesto, parametros);
    if (rc != DSM_OK) goto terminar;
    propia = puesto == grupo.lider;
    struct anuncio anuncio = {.filas = filas, .interior = interior, .columnas = columnas,
                               .page_size = cfg.page_size};
    demanda_fase_t fase = {0};
    dsm_reserva_id_t *ids[] = {&control, &m.a, &m.b, &m.c};
    size_t longitudes[] = {cfg.page_size, bytes[0], bytes[1], bytes[2]};
    if (propia) {
        void *base;
        for (size_t i = 0; i < 4 && rc == DSM_OK; i++) {
            rc = dsm_reservar(ctx, proc, longitudes[i], ids[i], &base);
            vinculadas[i] = *ids[i] != 0;
        }
        if (rc == DSM_OK) rc = matrices_inicializar(ctx, &m);
        if (rc == DSM_OK) rc = demanda_fase_publicar(&grupo, control, &fase);
        anuncio.control = control; anuncio.a = m.a; anuncio.b = m.b; anuncio.c = m.c;
        if (rc == DSM_OK) rc = demanda_grupo_publicar(&grupo, &anuncio, sizeof anuncio);
        if (rc == DSM_OK) {
            printf("CONTROL id=%" PRIu32 "\n", control);
            rc = mapa(ctx, "A", m.a, bytes[0], cfg.page_size);
            if (rc == DSM_OK) rc = mapa(ctx, "B", m.b, bytes[1], cfg.page_size);
            if (rc == DSM_OK) rc = mapa(ctx, "C", m.c, bytes[2], cfg.page_size);
        }
    } else {
        rc = demanda_grupo_recibir(&grupo, &anuncio, sizeof anuncio);
        if (rc == DSM_OK && (anuncio.filas != filas || anuncio.interior != interior ||
            anuncio.columnas != columnas || anuncio.page_size != cfg.page_size ||
            !anuncio.control || !anuncio.a || !anuncio.b || !anuncio.c ||
            anuncio.control == anuncio.a || anuncio.control == anuncio.b || anuncio.control == anuncio.c ||
            anuncio.a == anuncio.b || anuncio.a == anuncio.c || anuncio.b == anuncio.c ||
            (anuncio.control >> 16) != grupo.lider || (anuncio.a >> 16) != grupo.lider ||
            (anuncio.b >> 16) != grupo.lider || (anuncio.c >> 16) != grupo.lider)) rc = DSM_ERR_PROTO;
        if (rc == DSM_OK) {
            control = anuncio.control; m.a = anuncio.a; m.b = anuncio.b; m.c = anuncio.c;
            for (size_t i = 0; i < 4 && rc == DSM_OK; i++) {
                void *base;
                size_t longitud;
                rc = dsm_abrir_reserva(ctx, *ids[i], &base, &longitud);
                vinculadas[i] = 1; /* Tambien se intenta cerrar una apertura ambigua. */
                if (rc == DSM_OK && longitud != longitudes[i]) rc = DSM_ERR_PROTO;
            }
        }
    }
    if (rc != DSM_OK) goto terminar;
    /* Todos abrieron las cuatro ventanas antes de comenzar el calculo. */
    rc = demanda_marcar(&grupo, control, 1, DSM_OK);
    if (rc == DSM_OK && propia) rc = demanda_marcas_esperar(&grupo, control, 1);
    fase = (demanda_fase_t){.generacion = 1, .tipo = 1};
    if (rc == DSM_OK) rc = propia ? demanda_fase_publicar(&grupo, control, &fase) :
                                  demanda_fase_esperar(&grupo, control, 1, &fase);
    if (rc == DSM_OK && fase.tipo != 1) rc = DSM_ERR_PROTO;
    size_t paginas = 1 + (bytes[2] - 1) / cfg.page_size;
    size_t desde = paginas * grupo.rango / grupo.cantidad;
    size_t hasta = paginas * (grupo.rango + 1u) / grupo.cantidad;
    size_t hilos = cfg.max_hilos < 2 ? cfg.max_hilos : 2;
    if (rc == DSM_OK) printf("CALCULO nodo=%u ordinal=%u paginas=[%zu,%zu) hilos=%zu\n",
                              puesto, grupo.rango, desde, hasta, hilos);
    if (rc == DSM_OK && hasta > desde) rc = matrices_calcular(ctx, proc, &m, desde, hasta, hilos);
    if (rc == DSM_OK) rc = demanda_marcar(&grupo, control, 2, DSM_OK);
    if (rc == DSM_OK && propia) rc = demanda_marcas_esperar(&grupo, control, 2);
    if (rc == DSM_OK && propia) rc = matrices_verificar(ctx, &m);
    if (rc == DSM_OK && propia) printf("VERIFICACION OK\n");
    fase = (demanda_fase_t){.generacion = 2, .tipo = DEMANDA_FASE_CERRAR};
    if (rc == DSM_OK) rc = propia ? demanda_fase_publicar(&grupo, control, &fase) :
                                  demanda_fase_esperar(&grupo, control, 2, &fase);
    if (rc == DSM_OK && fase.tipo != DEMANDA_FASE_CERRAR) rc = DSM_ERR_PROTO;
    if (rc == DSM_OK && !propia) {
        uint64_t limite = demanda_ahora_ms() + 10000;
        for (size_t i = 4; i-- > 1;) {
            int otro = demanda_reserva_retirar(ctx, ids[i], 0, limite);
            rc = conservar(rc, otro);
            if (otro == DSM_OK) vinculadas[i] = 0;
        }
    }
    if (rc == DSM_OK) rc = demanda_marcar(&grupo, control, 3, DSM_OK);
    if (rc == DSM_OK && propia) rc = demanda_marcas_esperar(&grupo, control, 3);
terminar:
    if (rc != DSM_OK) demanda_grupo_error(&grupo, rc);
    uint64_t limite = demanda_ahora_ms() + 10000;
    dsm_reserva_id_t *limpiar[] = {&m.c, &m.b, &m.a, &control};
    int indices[] = {3, 2, 1, 0};
    for (size_t i = 0; i < 4; i++) if (vinculadas[indices[i]] && *limpiar[i]) {
        int otro = demanda_reserva_retirar(ctx, limpiar[i], propia, limite);
        if (otro != DSM_OK) fprintf(stderr, "LIMPIEZA matriz reserva=%" PRIu32 " DSM=%d\n", *limpiar[i], otro);
        rc = conservar(rc, otro);
    }
    rc = conservar(rc, demanda_grupo_retirar(&grupo));
    rc = conservar(rc, demanda_contexto_cerrar(&ctx, limite));
    if (rc == DSM_OK) printf("%s puesto=%u\n", propia ? "LIBERACION OK" : "TRABAJADOR OK", puesto);
    return rc;
}
