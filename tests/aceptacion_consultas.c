/* Snapshot por hogar, réplica en otro proceso y consultas durante invalidaciones TCP. */
#include "dsm.h"
#include "coherence.h"
#include "directory.h"
#include "network.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define EXIGIR(x) do { if (!(x)) { fprintf(stderr, "consultas:%d: %s\n", __LINE__, #x); return 1; } } while (0)

static const char ini[] = "config/lab-multihogar.ini";
/* Sustituye una única pregunta del cliente por un tipo desconocido para
 * comprobar que PROTO_ERROR despierta la RPC y que el enlace sigue activo. */
static atomic_int tipo_desconocido;
static atomic_int dejar_sin_respuesta;
int __real_dsm_red_enviar(dsm_red_t *, uint16_t, const uint8_t *, size_t);
int __wrap_dsm_red_enviar(dsm_red_t *red, uint16_t nodo, const uint8_t *buf, size_t n)
{
    dsm_cabecera_t cab;
    if (n == 40 && dsm_cabecera_decodificar(buf, n, &cab) == 0 &&
        cab.tipo == DSM_TIPO_STATS_REQUEST) {
        if (atomic_exchange(&dejar_sin_respuesta, 0)) return 0;
        if (!atomic_exchange(&tipo_desconocido, 0)) return __real_dsm_red_enviar(red, nodo, buf, n);
        uint8_t raw[40];
        memcpy(raw, buf, n);
        cab.tipo = 0x7e;
        if (dsm_cabecera_codificar(raw, &cab)) return -1;
        return __real_dsm_red_enviar(red, nodo, raw, n);
    }
    return __real_dsm_red_enviar(red, nodo, buf, n);
}

struct trabajo {
    dsm_context_t *ctx;
    dsm_reserva_id_t id;
    atomic_int parar;
    atomic_uint observaciones;
    int error;
};

static void *consultar(void *arg)
{
    struct trabajo *t = arg;
    do {
        dsm_pagina_info_t p[4];
        size_t n = 0;
        if (dsm_consultar_paginas(t->ctx, DSM_HOGAR_TODOS, p, 4, &n) != DSM_ERR_PARCIAL ||
            (n != 3 && n != 4) ||
            dsm_imprimir(t->ctx, stdout, 1) != DSM_ERR_PARCIAL ||
            dsm_aviso_publicar(t->ctx, "consulta", &n, sizeof n) != DSM_OK) {
            t->error = 1;
            break;
        }
        unsigned propias = 0;
        for (size_t i = 0; i < n; i++) {
            propias += p[i].reserva == t->id;
            if ((p[i].msi == DSM_MSI_M &&
                 (p[i].poseedor >= 40 || p[i].mascara != (UINT64_C(1) << p[i].poseedor))) ||
                (p[i].msi == DSM_MSI_S && p[i].poseedor != DSM_POSEEDOR_NINGUNO)) t->error = 1;
            for (size_t j = 0; j < i; j++) {
                if (p[i].gpn == p[j].gpn) t->error = 1;
            }
        }
        if (propias != 3 || t->error) { t->error = 1; break; }
        atomic_fetch_add(&t->observaciones, 1);
    } while (!atomic_load(&t->parar));
    return NULL;
}

static int cliente(int orden, int respuesta)
{
    dsm_context_t *ctx;
    uint32_t id;
    void *ar;
    size_t n;
    EXIGIR(dsm_iniciar(ini, 1, &ctx) == DSM_OK);
    EXIGIR(read(orden, &id, sizeof id) == sizeof id);
    EXIGIR(dsm_abrir_reserva(ctx, id, &ar, &n) == DSM_OK && n == 12288);
    dsm_pagina_info_t paginas[4];
    EXIGIR(dsm_consultar_paginas(ctx, DSM_HOGAR_TODOS, paginas, 4, &n) == DSM_ERR_PARCIAL && (n == 3 || n == 4));
    for (unsigned i = 0; i < 15; i++) {
        unsigned char v;
        EXIGIR(read(orden, &v, 1) == 1);
        EXIGIR(dsm_leer(ctx, id, 8192, &v, 1) == DSM_OK && v == i);
        v += 32;
        EXIGIR(dsm_escribir(ctx, id, 8192, &v, 1) == DSM_OK && write(respuesta, &v, 1) == 1);
    }
    EXIGIR(dsm_desvincular_reserva(ctx, id) == DSM_OK);
    EXIGIR(write(respuesta, "X", 1) == 1);
    char fin;
    EXIGIR(read(orden, &fin, 1) == 1 && dsm_cerrar(ctx) == DSM_OK);
    return 0;
}

/* La tabla sintética representa réplicas de muchos hogares. Se verifica el
 * límite de una respuesta local sin montar decenas de miles de ventanas. */
static int saturacion(void)
{
    dsm_config_t cfg;
    dsm_directorio_t dir;
    dsm_coherencia_t *c;
    EXIGIR(dsm_config_cargar(ini, &cfg) == 0 && dsm_directorio_init(&dir, &cfg, 0) == 0);
    EXIGIR(dsm_coherencia_iniciar(&c, &cfg, 0, &dir) == 0);
    size_t grandes = 1 + (DSM_LONGITUD_MAX - DSM_STATS_PREF) / DSM_FICHA_PAGINA;
    dsm_pagina_desc_t *replicas = calloc(grandes, sizeof *replicas);
    EXIGIR(replicas);
    for (size_t i = 0; i < grandes; i++) replicas[i].hogar = 1;
    pthread_mutex_lock(&dir.mutex);
    dsm_pagina_desc_t *original = dir.paginas;
    dir.paginas = replicas;
    dir.npaginas = grandes;
    pthread_mutex_unlock(&dir.mutex);
    dsm_stats_reply_t msg;
    EXIGIR(dsm_coherencia_stats(c, 0, DSM_STATS_PAGINAS, &msg) == -1 && errno == ENOSPC);
    EXIGIR(dsm_coherencia_stats(c, 0, DSM_STATS_HOGAR, &msg) == 0 && msg.p == 0 && msg.n == 1);
    free(msg.paginas);
    pthread_mutex_lock(&dir.mutex);
    dir.paginas = original;
    dir.npaginas = 0;
    pthread_mutex_unlock(&dir.mutex);
    free(replicas);
    dsm_coherencia_cerrar(c);
    dsm_directorio_cerrar(&dir);
    return 0;
}

int main(void)
{
    alarm(35);
    int a[2], b[2], estado;
    EXIGIR(pipe(a) == 0 && pipe(b) == 0);
    pid_t pid = fork();
    EXIGIR(pid >= 0);
    if (!pid) {
        close(a[1]); close(b[0]);
        _exit(cliente(a[0], b[1]));
    }
    close(a[0]); close(b[1]);
    dsm_context_t *ctx;
    dsm_proc_id_t proc;
    struct trabajo t = {0};
    void *ar;
    size_t n = 0;
    dsm_nodo_info_t nodos[3];
    EXIGIR(dsm_iniciar(ini, 0, &ctx) == DSM_OK && dsm_proc_abrir(ctx, &proc) == DSM_OK);
    for (unsigned i = 0; i < 300; i++) {
        EXIGIR(dsm_consultar_nodos(ctx, nodos, 3, &n) == DSM_OK && n == 3);
        if (nodos[1].libres == 2) break;
        usleep(20000);
    }
    EXIGIR(nodos[0].libres == 2 && nodos[1].libres == 2 && nodos[2].libres == 0 &&
           nodos[2].memoria == 4096 && nodos[2].totales == 1 && nodos[2].base == 16384 && nodos[2].estado == DSM_NODO_FUERA);
    atomic_store(&dejar_sin_respuesta, 1);
    EXIGIR(dsm_consultar_nodos(ctx, nodos, 3, &n) == DSM_OK &&
           nodos[1].estado == DSM_NODO_SOSPECHA && nodos[1].libres == 0);
    atomic_store(&tipo_desconocido, 1);
    EXIGIR(dsm_consultar_paginas(ctx, 1, NULL, 0, &n) == DSM_ERR_PROTO);
    EXIGIR(dsm_consultar_paginas(ctx, 1, NULL, 0, &n) == DSM_OK && n == 0);
    int fuera = dsm_consultar_paginas(ctx, 2, NULL, 0, &n);
    EXIGIR(fuera == DSM_ERR_NODO || fuera == DSM_ERR_TIEMPO);
    EXIGIR(dsm_reservar(ctx, proc, 12288, &t.id, &ar) == DSM_OK);
    EXIGIR(write(a[1], &t.id, sizeof t.id) == sizeof t.id);
    t.ctx = ctx;
    pthread_t hilo;
    EXIGIR(pthread_create(&hilo, NULL, consultar, &t) == 0);
    dsm_reserva_id_t temporal;
    EXIGIR(dsm_reservar(ctx, proc, 1, &temporal, &ar) == DSM_OK);
    for (unsigned i = 0; i < 15; i++) {
        unsigned char v = (unsigned char)i;
        EXIGIR(dsm_escribir(ctx, t.id, 8192, &v, 1) == DSM_OK && write(a[1], &v, 1) == 1);
        EXIGIR(read(b[0], &v, 1) == 1);
        if (i == 7) EXIGIR(dsm_liberar(ctx, temporal) == DSM_OK);
        EXIGIR(dsm_leer(ctx, t.id, 8192, &v, 1) == DSM_OK && v == i + 32);
    }
    char fin;
    EXIGIR(read(b[0], &fin, 1) == 1);
    atomic_store(&t.parar, 1);
    EXIGIR(pthread_join(hilo, NULL) == 0 && !t.error && t.observaciones);
    dsm_pagina_info_t p;
    EXIGIR(dsm_consultar_paginas(ctx, 1, &p, 1, &n) == DSM_OK && n == 1 &&
           p.gpn == 2 && p.hogar == 1 && p.reserva == t.id && p.orden == 2 && p.version > 1);
    EXIGIR(dsm_consultar_nodos(ctx, nodos, 3, &n) == DSM_OK &&
           nodos[0].contadores[DSM_CONTA_PAG_REC] && nodos[0].contadores[DSM_CONTA_BYTES_REC] &&
           nodos[0].contadores[DSM_CONTA_BYTES_REC] == 4096 * nodos[0].contadores[DSM_CONTA_PAG_REC] &&
           nodos[0].contadores[DSM_CONTA_BYTES_REC] == nodos[1].contadores[DSM_CONTA_BYTES_ENV] &&
           nodos[1].contadores[DSM_CONTA_BYTES_REC] == nodos[0].contadores[DSM_CONTA_BYTES_ENV] &&
           nodos[0].contadores[DSM_CONTA_INV_REC] && nodos[1].contadores[DSM_CONTA_INV_ENV] &&
           nodos[0].aportados == 2 && nodos[1].aportados == 1);
    char *texto = NULL;
    size_t largo = 0;
    FILE *local = open_memstream(&texto, &largo);
    EXIGIR(local && dsm_imprimir(ctx, local, 0) == DSM_OK && fclose(local) == 0);
    EXIGIR(largo && strstr(texto, "pagina gpn 2 hogar 1") && strstr(texto, "tabla local"));
    free(texto);
    EXIGIR(dsm_liberar(ctx, t.id) == DSM_OK);
    EXIGIR(dsm_consultar_nodos(ctx, nodos, 3, &n) == DSM_OK && nodos[0].libres == 2 && nodos[1].libres == 2);
    EXIGIR(write(a[1], "X", 1) == 1 && waitpid(pid, &estado, 0) == pid &&
           WIFEXITED(estado) && WEXITSTATUS(estado) == 0);
    EXIGIR(dsm_cerrar(ctx) == DSM_OK);
    EXIGIR(saturacion() == 0);
    printf("consultas concurrentes=%u GPN=2 version=%u libres restauradas=2+2\n", t.observaciones, p.version);
    return 0;
}
