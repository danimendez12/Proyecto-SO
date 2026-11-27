#include "aviso.h"
#include "dsm.h"

#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "avisos:%d: %s\n", __LINE__, #x); goto fin; } } while (0)

static uint64_t ahora(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) return 0;
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

static int esperar(dsm_coherencia_t *c, const char *clave, uint16_t autor,
                   int valor, int presente)
{
    uint64_t limite = ahora() + 5000;
    do {
        int recibido = 0;
        int rc = dsm_aviso_leer_autor(c, clave, autor, &recibido, sizeof recibido);
        if (presente ? rc == DSM_OK && recibido == valor : rc == DSM_ERR_ESTADO) return 1;
        struct timespec t = {.tv_nsec = 2000000};
        nanosleep(&t, NULL);
    } while (ahora() < limite);
    return 0;
}

static int esperar_fuera(dsm_coherencia_t *c, uint16_t par)
{
    uint64_t limite = ahora() + 5000, sesion;
    do {
        if (dsm_coherencia_sesion(c, par, &sesion) != 0) return 1;
        struct timespec t = {.tv_nsec = 2000000};
        nanosleep(&t, NULL);
    } while (ahora() < limite);
    return 0;
}

static void configurar(dsm_config_t *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->page_size = 4096; cfg->memoria_inicial = 65536;
    cfg->max_nodos = 3; cfg->max_hilos = 4;
    cfg->heartbeat_intervalo_ms = 100; cfg->heartbeat_fallos = 5;
    cfg->rpc_timeout_ms = 1000;
    cfg->puerto_rango_inicio = 27200; cfg->puerto_rango_fin = 27299;
    cfg->cantidad_nodos = 3;
    for (uint16_t i = 0; i < 3; i++) {
        cfg->nodos[i].id = i; cfg->nodos[i].puerto = (uint16_t)(27290 + i);
        cfg->nodos[i].ipv4_host = 0x7f000001u; cfg->nodos[i].memoria = 65536;
        snprintf(cfg->nodos[i].nombre, sizeof cfg->nodos[i].nombre, "avisos-%u", i);
    }
}

static int inyectar(dsm_coherencia_t *destino, uint16_t autor, uint64_t sesion,
                    uint64_t generacion, uint16_t banderas, int valor)
{
    dsm_cabecera_t cab = {.version = DSM_PROTO_VERSION, .tipo = DSM_TIPO_ANUNCIO,
                          .origen = autor, .destino = dsm_coherencia_nodo(destino), .id_msg = 17};
    dsm_anuncio_t msg = {.autor = autor, .sesion = sesion, .generacion = generacion,
                         .banderas = banderas, .largo = banderas ? 0 : sizeof valor,
                         .datos = (const uint8_t *)&valor};
    strcpy(msg.clave, "orden");
    uint8_t buf[DSM_ANUNCIO_VERSIONADO_BASE + DSM_ANUNCIO_DATOS];
    size_t n;
    return dsm_anuncio_versionado_codificar(buf, sizeof buf, &cab, &msg, &n) == 0 &&
           dsm_aviso_recibir(destino, buf, n) == 0;
}

struct publicador { dsm_coherencia_t *c; unsigned indice; int ok; };

static void *publicar_concurrente(void *arg)
{
    struct publicador *p = arg;
    char clave[32], otra[32];
    snprintf(clave, sizeof clave, "paralelo-%u", p->indice);
    snprintf(otra, sizeof otra, "paralelo-%u", 1u - p->indice);
    p->ok = 1;
    for (int i = 0; i < 100 && p->ok; i++) {
        int v = -1;
        p->ok = dsm_aviso_emitir(p->c, 0, clave, &i, sizeof i) == DSM_OK &&
                dsm_aviso_leer_autor(p->c, clave, 0, &v, sizeof v) == DSM_OK && v == i;
        int rc = dsm_aviso_leer_autor(p->c, otra, 0, &v, sizeof v);
        p->ok = p->ok && (rc == DSM_ERR_ESTADO || (rc == DSM_OK && v >= 0 && v < 100));
    }
    return NULL;
}

static int concurrentes(dsm_coherencia_t *origen, dsm_coherencia_t *replica)
{
    pthread_t hilos[2];
    struct publicador p[2] = {{.c = origen, .indice = 0}, {.c = origen, .indice = 1}};
    if (pthread_create(&hilos[0], NULL, publicar_concurrente, &p[0]) != 0) return 0;
    int segundo = pthread_create(&hilos[1], NULL, publicar_concurrente, &p[1]) == 0;
    pthread_join(hilos[0], NULL);
    if (segundo) pthread_join(hilos[1], NULL);
    if (!segundo || !p[0].ok || !p[1].ok) return 0;
    for (unsigned i = 0; i < 2; i++) {
        char clave[32];
        snprintf(clave, sizeof clave, "paralelo-%u", i);
        if (!esperar(replica, clave, 0, 99, 1) || dsm_aviso_quitar(origen, clave) != DSM_OK ||
            !esperar(replica, clave, 0, 0, 0)) return 0;
    }
    return 1;
}

int main(void)
{
    alarm(40);
    setvbuf(stdout, NULL, _IOLBF, 0);
    int resultado = 1;
    dsm_config_t cfg;
    configurar(&cfg);
    dsm_directorio_t dir[3] = {0};
    dsm_coherencia_t *c[3] = {0};
    int v = 11;
    CHECK(dsm_directorio_init(&dir[0], &cfg, 0) == 0);
    CHECK(dsm_coherencia_iniciar(&c[0], &cfg, 0, &dir[0]) == 0);
    CHECK(dsm_directorio_init(&dir[1], &cfg, 1) == 0);
    CHECK(dsm_coherencia_iniciar(&c[1], &cfg, 1, &dir[1]) == 0);
    CHECK(dsm_coherencia_esperar(c[0], 1) == 0);
    uint64_t sesion0, nueva;
    CHECK(dsm_coherencia_sesion(c[0], 0, &sesion0) == 0 && sesion0 != 0);
    CHECK(dsm_aviso_emitir(c[0], 0, "nulo", NULL, 1) == DSM_ERR_INVAL);
    char larga[DSM_ANUNCIO_CLAVE + 1];
    memset(larga, 'x', sizeof larga); larga[sizeof larga - 1] = '\0';
    CHECK(dsm_aviso_emitir(c[0], 0, larga, &v, sizeof v) == DSM_ERR_INVAL);
    CHECK(dsm_aviso_emitir(c[0], 0, "vacio", NULL, 0) == DSM_OK);
    CHECK(dsm_aviso_emitir(c[0], 0, "comun", &v, sizeof v) == DSM_OK);
    CHECK(esperar(c[1], "comun", 0, v, 1));
    v = 22;
    CHECK(dsm_aviso_emitir(c[1], 1, "comun", &v, sizeof v) == DSM_OK);
    CHECK(esperar(c[0], "comun", 1, v, 1));
    uint16_t autor = UINT16_MAX;
    int copia;
    CHECK(dsm_aviso_leer(c[0], "comun", &autor, &copia, sizeof copia) == DSM_OK &&
          autor == 0 && copia == 11);

    /* El tercero entra tarde: recibe directamente de los dos autores. */
    CHECK(dsm_directorio_init(&dir[2], &cfg, 2) == 0);
    CHECK(dsm_coherencia_iniciar(&c[2], &cfg, 2, &dir[2]) == 0);
    CHECK(esperar(c[2], "comun", 0, 11, 1));
    CHECK(esperar(c[2], "comun", 1, 22, 1));
    CHECK(dsm_aviso_quitar(c[0], "comun") == DSM_OK);
    CHECK(esperar(c[1], "comun", 0, 0, 0));
    CHECK(esperar(c[2], "comun", 0, 0, 0));
    CHECK(concurrentes(c[0], c[1]));
    puts("avisos: publicadores y lectores simultaneos, propagacion y retirada OK");

    /* Recicla mas claves que el limite simultaneo de la tabla. */
    for (unsigned i = 0; i < 260; i++) {
        char clave[32];
        snprintf(clave, sizeof clave, "ciclo-%u", i);
        v = (int)i;
        CHECK(dsm_aviso_emitir(c[0], 0, clave, &v, sizeof v) == DSM_OK);
        CHECK(esperar(c[1], clave, 0, v, 1));
        CHECK(dsm_aviso_quitar(c[0], clave) == DSM_OK);
        CHECK(esperar(c[1], clave, 0, 0, 0));
    }
    puts("avisos: 260 ciclos publicar/retirar y replay tardio OK");

    /* Reproduccion determinista de duplicado, atraso y sesion antigua. */
    CHECK(inyectar(c[2], 0, sesion0, 10000, 0, 2));
    CHECK(inyectar(c[2], 0, sesion0, 9999, 0, 1));
    CHECK(inyectar(c[2], 0, sesion0 ^ UINT64_C(1), 10001, 0, 3));
    CHECK(esperar(c[2], "orden", 0, 2, 1));
    CHECK(inyectar(c[2], 0, sesion0, 10001, DSM_ANUNCIO_RETIRADO, 0));
    CHECK(inyectar(c[2], 0, sesion0, 10000, 0, 2));
    CHECK(esperar(c[2], "orden", 0, 0, 0));

    /* Saturacion remota valida no debe convertir un peer sano en FUERA. */
    unsigned llenos = 0;
    int rc;
    do {
        char clave[32];
        snprintf(clave, sizeof clave, "lleno-%u", llenos);
        rc = dsm_aviso_emitir(c[1], 1, clave, &v, sizeof v);
        if (rc == DSM_OK) llenos++;
    } while (rc == DSM_OK && llenos <= 128);
    CHECK(rc == DSM_ERR_NOMEM && llenos != 0 && llenos <= 128);
    v = 77;
    dsm_cabecera_t cab = {.version = DSM_PROTO_VERSION, .tipo = DSM_TIPO_ANUNCIO,
                          .origen = 0, .destino = 1, .id_msg = dsm_coherencia_nuevo_id(c[0])};
    dsm_anuncio_t exceso = {.autor = 0, .sesion = sesion0, .generacion = 100000,
                            .largo = sizeof v, .datos = (const uint8_t *)&v};
    strcpy(exceso.clave, "exceso-remoto");
    uint8_t trama[DSM_ANUNCIO_VERSIONADO_BASE + DSM_ANUNCIO_DATOS];
    size_t bytes;
    CHECK(dsm_anuncio_versionado_codificar(trama, sizeof trama, &cab, &exceso, &bytes) == 0);
    CHECK(dsm_coherencia_enviar(c[0], 1, trama, bytes) == 0);
    /* Un RPC completo comparte la conexion que recibe avisos saturados. */
    dsm_stats_reply_t stats;
    CHECK(dsm_coherencia_stats(c[0], 1, DSM_STATS_HOGAR, &stats) == 0);
    free(stats.paginas);
    CHECK(dsm_coherencia_sesion(c[0], 1, &nueva) == 0 && nueva != 0);
    puts("avisos: saturacion conserva TCP y RPC STATS OK");

    uint64_t enlace_viejo, enlace_nuevo, sesion_par;
    CHECK(dsm_coherencia_enlace(c[1], 0, &sesion_par, &enlace_viejo) == 0);
    dsm_coherencia_cerrar(c[0]); c[0] = NULL;
    dsm_directorio_cerrar(&dir[0]);
    CHECK(esperar_fuera(c[2], 0));
    CHECK(dsm_aviso_leer_autor(c[2], "vacio", 0, NULL, 0) == DSM_ERR_ESTADO);
    CHECK(dsm_directorio_init(&dir[0], &cfg, 0) == 0);
    CHECK(dsm_coherencia_iniciar(&c[0], &cfg, 0, &dir[0]) == 0);
    CHECK(dsm_coherencia_sesion(c[0], 0, &nueva) == 0 && nueva != sesion0);
    v = 99;
    CHECK(dsm_aviso_emitir(c[0], 0, "comun", &v, sizeof v) == DSM_OK);
    CHECK(esperar(c[2], "comun", 0, v, 1));
    CHECK(inyectar(c[2], 0, sesion0, UINT64_MAX, 0, 9));
    CHECK(esperar(c[2], "comun", 0, v, 1));
    CHECK(dsm_coherencia_esperar(c[1], 0) == 0);
    CHECK(dsm_coherencia_enlace(c[1], 0, &sesion_par, &enlace_nuevo) == 0 &&
          enlace_nuevo != enlace_viejo);
    CHECK(esperar(c[0], "comun", 1, 22, 1));
    CHECK(dsm_coherencia_sesion(c[1], 1, &sesion_par) == 0);
    v = 123;
    cab = (dsm_cabecera_t){.version = DSM_PROTO_VERSION, .tipo = DSM_TIPO_ANUNCIO,
                           .origen = 1, .destino = 0, .id_msg = dsm_coherencia_nuevo_id(c[1])};
    exceso = (dsm_anuncio_t){.autor = 1, .sesion = sesion_par, .generacion = 1000000,
                             .largo = sizeof v, .datos = (const uint8_t *)&v};
    strcpy(exceso.clave, "comun");
    CHECK(dsm_anuncio_versionado_codificar(trama, sizeof trama, &cab, &exceso, &bytes) == 0);
    /* Un snapshot preparado para el fd anterior no cruza al stream nuevo. */
    errno = 0;
    CHECK(dsm_coherencia_enviar_enlace(c[1], 0, enlace_viejo, trama, bytes) == -1 && errno == ENOTCONN);
    CHECK(esperar(c[0], "comun", 1, 22, 1));
    CHECK(dsm_coherencia_enviar_enlace(c[1], 0, enlace_nuevo, trama, bytes) == 0);
    CHECK(esperar(c[0], "comun", 1, v, 1));
    puts("avisos: snapshot del fd retirado rechazado y enlace nuevo utilizable OK");
    puts("avisos: reinicio del autor con donante vivo no reutiliza sesion OK");
    resultado = 0;
fin:
    for (unsigned i = 0; i < 3; i++) {
        dsm_coherencia_cerrar(c[i]);
        if (dir[i].inicializado) dsm_directorio_cerrar(&dir[i]);
    }
    return resultado;
}
