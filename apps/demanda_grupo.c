#include "demanda_grupo.h"
#include "demanda_grupo_privado.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

struct recursos {
    uint64_t token;
    uint32_t magic, largo;
    unsigned char datos[DSM_ANUNCIO_DATOS - 16];
};

struct marca { uint32_t generacion; int32_t resultado; };

_Static_assert(sizeof(struct recursos) == DSM_ANUNCIO_DATOS, "Sobre de recursos");
_Static_assert(sizeof(demanda_fase_t) + DSM_CONFIG_MAX_NODOS * sizeof(struct marca) <= 4096,
               "Las marcas deben caber en una pagina");

uint64_t demanda_transcurrido_ms(const struct timespec *inicio, const struct timespec *fin)
{
    int64_t segundos = (int64_t)fin->tv_sec - (int64_t)inicio->tv_sec;
    int64_t nanos = (int64_t)fin->tv_nsec - (int64_t)inicio->tv_nsec;
    if (nanos < 0) { segundos--; nanos += INT64_C(1000000000); }
    if (segundos < 0) return 0;
    return (uint64_t)segundos * 1000u + (uint64_t)nanos / 1000000u;
}

uint64_t demanda_ahora_ms(void)
{
    struct timespec ahora;
    if (clock_gettime(CLOCK_MONOTONIC, &ahora) != 0) return 0;
    return (uint64_t)ahora.tv_sec * 1000u + (uint64_t)ahora.tv_nsec / 1000000u;
}

static void pausa(void) { const struct timespec t = {.tv_nsec = 10000000}; nanosleep(&t, NULL); }

static int opcion(const char *nombre, uint64_t defecto, uint64_t maximo, uint64_t *valor)
{
    const char *texto = getenv(nombre);
    if (texto == NULL) { *valor = defecto; return DSM_OK; }
    if (*texto < '0' || *texto > '9') return DSM_ERR_INVAL;
    char *fin;
    errno = 0;
    unsigned long long n = strtoull(texto, &fin, 10);
    if (errno || *fin || n == 0 || n > maximo) return DSM_ERR_INVAL;
    *valor = n;
    return DSM_OK;
}

static uint64_t huella(uint64_t h, const void *datos, size_t n)
{
    const unsigned char *p = datos;
    while (n--) { h ^= *p++; h *= UINT64_C(1099511628211); }
    return h;
}

static int aleatorio(uint64_t *valor)
{
    unsigned char *p = (unsigned char *)valor;
    size_t faltan = sizeof *valor;
    while (faltan) {
        ssize_t n = getrandom(p, faltan, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return DSM_ERR_ESTADO;
        p += n; faltan -= (size_t)n;
    }
    if (*valor == 0) *valor = 1;
    return DSM_OK;
}

static int miembro_valido(const struct miembro *m)
{
    if (m->magic != GRUPO_MAGIC || !m->nonce || m->cantidad > DSM_CONFIG_MAX_NODOS ||
        m->lider >= DSM_CONFIG_MAX_NODOS || m->etapa < ENTRAR || m->etapa > FALLAR) return 0;
    for (uint16_t i = 0; i < m->cantidad; i++)
        if (m->ids[i] >= DSM_CONFIG_MAX_NODOS || (i && m->ids[i - 1] >= m->ids[i])) return 0;
    if (m->etapa >= PROPONER && m->etapa <= EJECUTAR &&
        (!m->token || !m->cantidad || m->ids[0] != m->lider)) return 0;
    return 1;
}

int demanda_miembro_desde_grupo(const demanda_grupo_t *g, unsigned etapa, struct miembro *m)
{
    if (g == NULL || m == NULL || g->yo >= DSM_CONFIG_MAX_NODOS ||
        g->cantidad > DSM_CONFIG_MAX_NODOS || etapa < ENTRAR || etapa > FALLAR) return DSM_ERR_INVAL;
    struct miembro tmp = {0};
    tmp.magic = GRUPO_MAGIC; tmp.nonce = g->nonce; tmp.token = g->token;
    tmp.digest = g->digest; tmp.lider = g->lider; tmp.cantidad = g->cantidad;
    tmp.etapa = (uint16_t)etapa;
    for (uint16_t i = 0; i < g->cantidad; i++) {
        if (g->ids[i] >= DSM_CONFIG_MAX_NODOS || (i && g->ids[i - 1] >= g->ids[i])) return DSM_ERR_INVAL;
        tmp.ids[i] = (uint8_t)g->ids[i];
    }
    if (!miembro_valido(&tmp)) return DSM_ERR_PROTO;
    *m = tmp;
    return DSM_OK;
}

int demanda_miembro_codificar(const struct miembro *m, void *salida, size_t cap, size_t *bytes)
{
    if (m == NULL || salida == NULL || bytes == NULL || cap < sizeof *m) return DSM_ERR_INVAL;
    if (!miembro_valido(m)) return DSM_ERR_PROTO;
    struct miembro limpio;
    memset(&limpio, 0, sizeof limpio);
    limpio.nonce = m->nonce; limpio.token = m->token; limpio.digest = m->digest;
    limpio.magic = m->magic; limpio.resultado = m->resultado;
    limpio.etapa = m->etapa; limpio.lider = m->lider; limpio.cantidad = m->cantidad;
    memcpy(limpio.ids, m->ids, m->cantidad * sizeof m->ids[0]);
    memcpy(salida, &limpio, sizeof limpio);
    *bytes = sizeof limpio;
    return DSM_OK;
}

int demanda_miembro_decodificar(const void *datos, size_t bytes, struct miembro *m)
{
    if (datos == NULL || m == NULL) return DSM_ERR_INVAL;
    if (bytes != sizeof *m) return DSM_ERR_PROTO;
    struct miembro tmp;
    memcpy(&tmp, datos, sizeof tmp);
    if (!miembro_valido(&tmp)) return DSM_ERR_PROTO;
    *m = tmp;
    return DSM_OK;
}

static int publicar(demanda_grupo_t *g, const struct miembro *m)
{
    uint8_t datos[sizeof *m];
    size_t bytes;
    int rc = demanda_miembro_codificar(m, datos, sizeof datos, &bytes);
    return rc == DSM_OK ? dsm_aviso_publicar(g->ctx, g->clave, datos, bytes) : rc;
}

static int leer(demanda_grupo_t *g, uint16_t autor, struct miembro *m)
{
    uint8_t datos[sizeof *m];
    int rc = dsm_aviso_consultar_autor(g->ctx, g->clave, autor, datos, sizeof datos);
    if (rc != DSM_OK) return rc;
    return demanda_miembro_decodificar(datos, sizeof datos, m);
}

static int mismo(const struct miembro *a, const struct miembro *b)
{
    return a->token == b->token && a->digest == b->digest && a->lider == b->lider &&
           a->cantidad == b->cantidad &&
           memcmp(a->ids, b->ids, a->cantidad * sizeof a->ids[0]) == 0;
}

static int comparar(const void *a, const void *b)
{ uint16_t x = *(const uint16_t *)a, y = *(const uint16_t *)b; return (x > y) - (x < y); }

static int digest_grupo(demanda_grupo_t *g, const uint16_t *ids, size_t n, uint64_t *digest)
{
    uint64_t h = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < n; i++) {
        struct miembro m;
        int rc = leer(g, ids[i], &m);
        if (rc != DSM_OK) return rc;
        h = huella(h, &ids[i], sizeof ids[i]);
        h = huella(h, &m.nonce, sizeof m.nonce);
    }
    *digest = h;
    return DSM_OK;
}

int demanda_grupo_iniciar(demanda_grupo_t *g, dsm_context_t *ctx, uint16_t yo,
                          const char *parametros)
{
    memset(g, 0, sizeof *g);
    g->ctx = ctx; g->yo = yo; g->lider = yo;
    uint64_t espera, segundos;
    int rc = opcion("DSM_DEMANDA_ESPERA_MS", 3000, 60000, &espera);
    if (rc == DSM_OK) rc = opcion("DSM_DEMANDA_PLAZO_SEG", 1800, 86400, &segundos);
    if (rc != DSM_OK || yo >= DSM_CONFIG_MAX_NODOS) return DSM_ERR_INVAL;
    const char *nombre = getenv("DSM_DEMANDA_GRUPO");
    if (nombre && (!*nombre || strlen(nombre) > 64)) return DSM_ERR_INVAL;
    uint64_t h = huella(UINT64_C(14695981039346656037), parametros, strlen(parametros) + 1);
    if (nombre) h = huella(h, nombre, strlen(nombre));
    snprintf(g->clave, sizeof g->clave, "dg3:%016" PRIx64, h);
    snprintf(g->recursos, sizeof g->recursos, "dr3:%016" PRIx64, h);
    rc = aleatorio(&g->nonce);
    if (rc != DSM_OK) return rc;
    uint64_t inicio = demanda_ahora_ms(), estable = inicio;
    g->limite = inicio + segundos * 1000u;
    uint64_t limite_grupo = inicio + espera + 30000u;
    if (limite_grupo > g->limite) limite_grupo = g->limite;
    struct miembro propio;
    rc = demanda_miembro_desde_grupo(g, ENTRAR, &propio);
    if (rc == DSM_OK) rc = publicar(g, &propio);
    if (rc != DSM_OK) return rc;
    /* El JOIN existe durante una ronda real de reconexion. La quietud local
     * nunca reemplaza ni el sondeo ni las confirmaciones de la propuesta. */
    rc = dsm_sondear(ctx, 2000);
    if (rc != DSM_OK) return rc;
    uint64_t anterior = 0;
    for (;;) {
        uint16_t autores[DSM_CONFIG_MAX_NODOS];
        size_t n = 0;
        rc = dsm_aviso_participantes(ctx, g->clave, autores, DSM_CONFIG_MAX_NODOS, &n);
        if (rc != DSM_OK) return rc;
        qsort(autores, n, sizeof autores[0], comparar);
        uint16_t candidatos[DSM_CONFIG_MAX_NODOS];
        size_t nc = 0;
        struct miembro propuesta = {0};
        uint16_t autor_propuesta = UINT16_MAX;
        for (size_t i = 0; i < n; i++) {
            struct miembro m;
            rc = leer(g, autores[i], &m);
            if (rc == DSM_ERR_ESTADO || rc == DSM_ERR_NODO) continue;
            if (rc != DSM_OK) return rc;
            if (m.etapa == ENTRAR) candidatos[nc++] = autores[i];
            if (m.etapa >= PROPONER && m.etapa <= EJECUTAR && m.lider == autores[i] &&
                m.token == m.nonce) {
                if (propio.etapa != ENTRAR && m.lider != g->lider) {
                    fprintf(stderr, "DEMANDA propuestas incompatibles puesto=%u lider=%u otro=%u\n",
                            yo, g->lider, m.lider);
                    return DSM_ERR_OCUPADA;
                }
                if (autores[i] < autor_propuesta) { propuesta = m; autor_propuesta = autores[i]; }
            }
        }
        if (propio.etapa == ENTRAR && autor_propuesta != UINT16_MAX) {
            uint64_t digest = 0;
            size_t rango = 0;
            while (rango < propuesta.cantidad && propuesta.ids[rango] != yo) rango++;
            if (rango == propuesta.cantidad || propuesta.etapa != PROPONER) {
                fprintf(stderr, "DEMANDA llegada tardia puesto=%u grupo ya cerrado\n", yo);
                return DSM_ERR_OCUPADA;
            }
            uint16_t ids_propuestos[DSM_CONFIG_MAX_NODOS];
            for (uint16_t i = 0; i < propuesta.cantidad; i++)
                ids_propuestos[i] = (uint16_t)propuesta.ids[i];
            /* El digest conserva la representacion canonica de IDs uint16_t,
             * independientemente del arreglo compacto del anuncio. */
            rc = digest_grupo(g, ids_propuestos, propuesta.cantidad, &digest);
            if (rc == DSM_OK && digest == propuesta.digest && propuesta.cantidad &&
                propuesta.ids[0] == autor_propuesta) {
                g->lider = autor_propuesta; g->token = propuesta.token; g->digest = digest;
                g->cantidad = propuesta.cantidad; g->rango = (uint16_t)rango;
                for (uint16_t i = 0; i < g->cantidad; i++) g->ids[i] = (uint16_t)propuesta.ids[i];
                rc = demanda_miembro_desde_grupo(g, ACEPTAR, &propio);
                if (rc == DSM_OK) rc = publicar(g, &propio);
                if (rc != DSM_OK) return rc;
            } else if (rc != DSM_OK && rc != DSM_ERR_ESTADO && rc != DSM_ERR_NODO) return rc;
        } else if (propio.etapa == ENTRAR && nc && candidatos[0] == yo) {
            uint64_t digest = 0;
            rc = digest_grupo(g, candidatos, nc, &digest);
            if (rc != DSM_OK) { if (rc != DSM_ERR_ESTADO && rc != DSM_ERR_NODO) return rc; }
            else {
                uint64_t ahora = demanda_ahora_ms();
                if (digest != anterior) { anterior = digest; estable = ahora; }
                if (ahora - estable >= espera) {
                    g->token = g->nonce; g->digest = digest; g->cantidad = (uint16_t)nc;
                    memcpy(g->ids, candidatos, nc * sizeof candidatos[0]);
                    rc = demanda_miembro_desde_grupo(g, PROPONER, &propio);
                    if (rc == DSM_OK) rc = publicar(g, &propio);
                    if (rc != DSM_OK) return rc;
                }
            }
        } else if (g->lider == yo && propio.etapa >= PROPONER) {
            int todos = 1;
            for (uint16_t i = 1; i < g->cantidad; i++) {
                struct miembro m;
                rc = leer(g, g->ids[i], &m);
                if (rc == DSM_OK && m.etapa == FALLAR) return m.resultado ? m.resultado : DSM_ERR_ESTADO;
                unsigned esperada = propio.etapa == PROPONER ? ACEPTAR : CONFIRMADO;
                if (rc != DSM_OK || !mismo(&propio, &m) || m.etapa != esperada) todos = 0;
            }
            if (todos) {
                propio.etapa = propio.etapa == PROPONER ? CONFIRMAR : EJECUTAR;
                rc = publicar(g, &propio);
                if (rc != DSM_OK) return rc;
                if (propio.etapa == EJECUTAR) break;
            }
        } else if (propio.etapa == ACEPTAR || propio.etapa == CONFIRMADO) {
            struct miembro m;
            rc = leer(g, g->lider, &m);
            if (rc == DSM_OK && m.etapa == FALLAR) return m.resultado ? m.resultado : DSM_ERR_ESTADO;
            if (rc == DSM_OK && mismo(&propio, &m)) {
                if (m.etapa == CONFIRMAR && propio.etapa == ACEPTAR) {
                    propio.etapa = CONFIRMADO;
                    rc = publicar(g, &propio);
                    if (rc != DSM_OK) return rc;
                } else if (m.etapa == EJECUTAR && propio.etapa == CONFIRMADO) break;
            }
        }
        if (demanda_ahora_ms() >= limite_grupo) return DSM_ERR_TIEMPO;
        pausa();
    }
    char lista[DSM_CONFIG_MAX_NODOS * 4 + 1] = {0};
    size_t usados = 0;
    for (uint16_t i = 0; i < g->cantidad; i++)
        usados += (size_t)snprintf(lista + usados, sizeof lista - usados, "%s%u", i ? "," : "", g->ids[i]);
    printf("DEMANDA participantes=%u creador=%u sesion=%016" PRIx64 " ids=%s\n",
           g->cantidad, g->lider, g->token, lista);
    return DSM_OK;
}

int demanda_grupo_estado(demanda_grupo_t *g)
{
    if (demanda_ahora_ms() >= g->limite) return DSM_ERR_TIEMPO;
    if (g->yo == g->lider) return DSM_OK;
    struct miembro m;
    int rc = leer(g, g->lider, &m);
    if (rc != DSM_OK) return rc == DSM_ERR_ESTADO ? DSM_ERR_NODO : rc;
    if (m.token != g->token || m.digest != g->digest) return DSM_ERR_PROTO;
    return m.etapa == FALLAR ? (m.resultado ? m.resultado : DSM_ERR_ESTADO) : DSM_OK;
}

void demanda_grupo_error(demanda_grupo_t *g, int resultado)
{
    if (!g->ctx || !g->clave[0] || resultado == DSM_OK) return;
    fprintf(stderr, "DEMANDA ERROR puesto=%u DSM=%d\n", g->yo, resultado);
    struct miembro m;
    if (demanda_miembro_desde_grupo(g, FALLAR, &m) != DSM_OK) return;
    m.resultado = resultado;
    (void)publicar(g, &m);
}

int demanda_grupo_publicar(demanda_grupo_t *g, const void *datos, size_t n)
{
    if (g->yo != g->lider || n > sizeof ((struct recursos *)0)->datos) return DSM_ERR_INVAL;
    struct recursos r = {.token = g->token, .magic = GRUPO_MAGIC, .largo = (uint32_t)n};
    memcpy(r.datos, datos, n);
    return dsm_aviso_publicar(g->ctx, g->recursos, &r, sizeof r);
}

int demanda_grupo_recibir(demanda_grupo_t *g, void *datos, size_t n)
{
    if (n > sizeof ((struct recursos *)0)->datos) return DSM_ERR_INVAL;
    for (;;) {
        int rc = demanda_grupo_estado(g);
        if (rc != DSM_OK) return rc;
        struct recursos r;
        rc = dsm_aviso_consultar_autor(g->ctx, g->recursos, g->lider, &r, sizeof r);
        if (rc == DSM_OK && r.token == g->token) {
            if (r.magic != GRUPO_MAGIC || r.largo != n) return DSM_ERR_PROTO;
            memcpy(datos, r.datos, n); return DSM_OK;
        }
        if (rc != DSM_OK && rc != DSM_ERR_ESTADO) return rc;
        pausa();
    }
}

int demanda_grupo_retirar(demanda_grupo_t *g)
{
    int rc = DSM_OK;
    if (g->recursos[0] && g->yo == g->lider) rc = dsm_aviso_retirar(g->ctx, g->recursos);
    int otro = g->clave[0] ? dsm_aviso_retirar(g->ctx, g->clave) : DSM_OK;
    return rc == DSM_OK ? otro : rc;
}

int demanda_fase_publicar(demanda_grupo_t *g, dsm_reserva_id_t control, demanda_fase_t *fase)
{
    fase->magic = FASE_MAGIC; fase->token = g->token;
    return dsm_escribir(g->ctx, control, 0, fase, sizeof *fase);
}

int demanda_fase_esperar(demanda_grupo_t *g, dsm_reserva_id_t control,
                        uint32_t generacion, demanda_fase_t *fase)
{
    for (;;) {
        int rc = demanda_grupo_estado(g);
        if (rc != DSM_OK) return rc;
        rc = dsm_leer(g->ctx, control, 0, fase, sizeof *fase);
        if (rc != DSM_OK) return rc;
        if (fase->magic != FASE_MAGIC || fase->token != g->token || fase->generacion > generacion)
            return DSM_ERR_PROTO;
        if (fase->resultado != DSM_OK) return fase->resultado;
        if (fase->generacion == generacion) return DSM_OK;
        pausa();
    }
}

int demanda_marcar(demanda_grupo_t *g, dsm_reserva_id_t control, uint32_t generacion, int resultado)
{
    struct marca m = {.generacion = generacion, .resultado = resultado};
    return dsm_escribir(g->ctx, control, sizeof(demanda_fase_t) + g->rango * sizeof m, &m, sizeof m);
}

int demanda_marcas_esperar(demanda_grupo_t *g, dsm_reserva_id_t control, uint32_t generacion)
{
    for (;;) {
        if (demanda_ahora_ms() >= g->limite) return DSM_ERR_TIEMPO;
        struct marca marcas[DSM_CONFIG_MAX_NODOS];
        int rc = dsm_leer(g->ctx, control, sizeof(demanda_fase_t), marcas, g->cantidad * sizeof marcas[0]);
        if (rc != DSM_OK) return rc;
        int todos = 1;
        for (uint16_t i = 0; i < g->cantidad; i++) {
            if (marcas[i].generacion > generacion) return DSM_ERR_PROTO;
            if (marcas[i].generacion == generacion) {
                if (marcas[i].resultado != DSM_OK) return marcas[i].resultado;
            } else {
                todos = 0;
                struct miembro m;
                rc = leer(g, g->ids[i], &m);
                if (rc != DSM_OK) return rc == DSM_ERR_ESTADO ? DSM_ERR_NODO : rc;
                if (m.token != g->token) return DSM_ERR_PROTO;
                if (m.etapa == FALLAR) return m.resultado ? m.resultado : DSM_ERR_ESTADO;
            }
        }
        if (todos) return DSM_OK;
        pausa();
    }
}

int demanda_reserva_retirar(dsm_context_t *ctx, dsm_reserva_id_t *id, int propia, uint64_t limite)
{
    if (!*id) return DSM_OK;
    int rc;
    do {
        rc = propia ? dsm_liberar(ctx, *id) : dsm_desvincular_reserva(ctx, *id);
        if (rc == DSM_OK) { *id = 0; return rc; }
        if (rc != DSM_ERR_OCUPADA) return rc;
        pausa();
    } while (demanda_ahora_ms() < limite);
    return rc;
}

int demanda_contexto_cerrar(dsm_context_t **ctx, uint64_t limite)
{
    if (!*ctx) return DSM_OK;
    int rc;
    do {
        rc = dsm_cerrar(*ctx);
        if (rc == DSM_OK) { *ctx = NULL; return rc; }
        if (rc != DSM_ERR_OCUPADA) return rc;
        pausa();
    } while (demanda_ahora_ms() < limite);
    return rc;
}

int demanda_mapa(dsm_context_t *ctx, const char *nombre, dsm_reserva_id_t id,
                 size_t bytes, size_t pagina)
{
    size_t paginas = 1 + (bytes - 1) / pagina, inicio = 0;
    uint16_t previo = UINT16_MAX;
    uint64_t base = 0, ultimo = 0;
    for (size_t i = 0; i <= paginas; i++) {
        uint16_t hogar = UINT16_MAX;
        uint64_t gpn = 0;
        if (i < paginas) {
            int rc = dsm_ubicacion_pagina(ctx, id, i, &hogar, &gpn);
            if (rc != DSM_OK) return rc;
        }
        if (i && (i == paginas || hogar != previo || gpn != ultimo + 1u)) {
            printf("TRAMO %s orden=%zu paginas=%zu hogar=%u gpn=%" PRIu64 "\n",
                   nombre, inicio, i - inicio, previo, base);
            printf("HOGAR %s pagina=%zu hogar=%u gpn=%" PRIu64 " paginas=%zu\n",
                   nombre, inicio, previo, base, i - inicio);
            inicio = i; base = gpn;
        } else if (!i) base = gpn;
        previo = hogar; ultimo = gpn;
    }
    return DSM_OK;
}
