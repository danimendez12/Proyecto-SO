#include "../tools/consola_tareas.h"

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define EXIGIR(c) do { if (!(c)) { fprintf(stderr, "aceptacion_tareas:%d: %s\n", __LINE__, #c); goto terminar; } } while (0)

struct orden { int salir; uint32_t id; size_t offset; };

static int transferir(int fd, void *datos, size_t bytes, int escribir)
{
    unsigned char *p = datos;
    while (bytes) {
        if (!escribir) {
            struct pollfd f = {.fd = fd, .events = POLLIN};
            int rc;
            do { rc = poll(&f, 1, 10000); } while (rc < 0 && errno == EINTR);
            if (rc <= 0) return -1;
        }
        ssize_t n = escribir ? write(fd, p, bytes) : read(fd, p, bytes);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; bytes -= (size_t)n;
    }
    return 0;
}

/* Formula independiente equivalente a xor de cada byte del offset. */
static unsigned char esperado(size_t offset, unsigned char valor)
{
    do { valor ^= (unsigned char)(offset & 255u); offset /= 256u; } while (offset);
    return valor;
}

static int verificar(dsm_context_t *ctx, uint32_t id, size_t bytes, unsigned char valor)
{
    unsigned char buffer[4096];
    for (size_t o = 0; o < bytes;) {
        size_t n = bytes - o < sizeof buffer ? bytes - o : sizeof buffer;
        if (dsm_leer(ctx, id, o, buffer, n) != DSM_OK) return -1;
        for (size_t i = 0; i < n; i++) if (buffer[i] != esperado(o + i, valor)) return -1;
        o += n;
    }
    return 0;
}

static void donante(const char *ini, int entrada, int salida)
{
    dsm_context_t *ctx = NULL;
    int rc = dsm_iniciar(ini, 1, &ctx);
    dsm_proc_id_t proceso;
    if (rc == DSM_OK) rc = dsm_proc_abrir(ctx, &proceso);
    if (transferir(salida, &rc, sizeof rc, 1)) _exit(2);
    struct orden orden;
    while (rc == DSM_OK && transferir(entrada, &orden, sizeof orden, 0) == 0) {
        if (orden.salir == 1) rc = dsm_cerrar(ctx);
        else if (orden.salir == 2) rc = dsm_desvincular_reserva(ctx, orden.id);
        else {
            void *base;
            size_t bytes;
            unsigned char cero = 0;
            rc = dsm_abrir_reserva(ctx, orden.id, &base, &bytes);
            if (rc == DSM_OK) rc = dsm_escribir(ctx, orden.id, orden.offset, &cero, 1);
        }
        if (transferir(salida, &rc, sizeof rc, 1)) _exit(2);
        if (orden.salir == 1) break;
    }
    _exit(rc == DSM_OK ? 0 : 1);
}

static int casos(size_t pagina)
{
    FILE *config = tmpfile();
    dsm_context_t *ctx = NULL;
    dsm_reserva_id_t id = 0;
    pid_t pid = -1;
    int a[2] = {-1, -1}, b[2] = {-1, -1}, resultado = 1;
    EXIGIR(config != NULL);
    fprintf(config, "page_size=%zu\nmemoria_inicial=%zu\nmax_nodos=2\nmax_hilos=8\n"
                    "heartbeat_intervalo_ms=100\nheartbeat_fallos=10\nrpc_timeout_ms=1500\n"
                    "puerto_rango_inicio=27200\npuerto_rango_fin=27299\n"
                    "nodo=0 tareas0 127.0.0.1 27200 %zu\nnodo=1 tareas1 127.0.0.1 27201 %zu\n",
            pagina, 4 * pagina, 4 * pagina, 16 * pagina);
    EXIGIR(fflush(config) == 0 && pipe(a) == 0 && pipe(b) == 0);
    char ini[64]; snprintf(ini, sizeof ini, "/proc/self/fd/%d", fileno(config));
    pid = fork();
    EXIGIR(pid >= 0);
    if (!pid) { close(a[1]); close(b[0]); donante(ini, a[0], b[1]); }
    close(a[0]); a[0] = -1; close(b[1]); b[1] = -1;
    int rc;
    EXIGIR(transferir(b[0], &rc, sizeof rc, 0) == 0 && rc == DSM_OK);
    EXIGIR(dsm_iniciar(ini, 0, &ctx) == DSM_OK && dsm_sondear(ctx, 2000) == DSM_OK);
    dsm_proc_id_t proceso;
    EXIGIR(dsm_proc_abrir(ctx, &proceso) == DSM_OK);
    const size_t bytes[] = {1, pagina - 1, pagina + 17, 5 * pagina + 19};
    const size_t hilos[] = {1, 2, 4, 8};
    for (size_t caso = 0; caso < sizeof bytes / sizeof bytes[0]; caso++) {
        void *ar;
        EXIGIR(dsm_reservar(ctx, proceso, bytes[caso], &id, &ar) == DSM_OK);
        if (caso == 3) {
            uint16_t hogar;
            uint64_t gpn;
            EXIGIR(dsm_ubicacion_pagina(ctx, id, 4, &hogar, &gpn) == DSM_OK && hogar == 1);
        }
        EXIGIR(consola_tarea_patron(ctx, proceso, id, bytes[caso], pagina, 0, 1, 0, stdout) == DSM_ERR_INVAL);
        EXIGIR(consola_tarea_patron(ctx, proceso, id, bytes[caso], pagina, 65, 1, 0, stdout) == DSM_ERR_INVAL);
        for (size_t h = 0; h < sizeof hilos / sizeof hilos[0]; h++) {
            EXIGIR(consola_tarea_patron(ctx, proceso, id, bytes[caso], pagina, hilos[h], 29, 0, stdout) == DSM_OK);
            EXIGIR(verificar(ctx, id, bytes[caso], 29) == 0);
            struct orden invalidar = {.id = id, .offset = caso == 3 ? 4 * pagina : 0};
            EXIGIR(transferir(a[1], &invalidar, sizeof invalidar, 1) == 0 &&
                   transferir(b[0], &rc, sizeof rc, 0) == 0 && rc == DSM_OK);
            dsm_nodo_info_t antes[2], despues[2];
            size_t cantidad;
            EXIGIR(dsm_consultar_nodos(ctx, antes, 2, &cantidad) == DSM_OK && cantidad == 2);
            EXIGIR(consola_tarea_patron(ctx, proceso, id, bytes[caso], pagina, hilos[h], 177, 1, stdout) == DSM_OK);
            EXIGIR(dsm_consultar_nodos(ctx, despues, 2, &cantidad) == DSM_OK &&
                   despues[0].contadores[1] > antes[0].contadores[1]);
            struct orden desvincular = {.salir = 2, .id = id};
            EXIGIR(transferir(a[1], &desvincular, sizeof desvincular, 1) == 0 &&
                   transferir(b[0], &rc, sizeof rc, 0) == 0 && rc == DSM_OK);
            EXIGIR(verificar(ctx, id, bytes[caso], 177) == 0);
        }
        printf("TAREAS ACEPTACION pagina=%zu bytes=%zu hilos=1,2,4,8 api=OK Ar=OK verificacion=exhaustiva\n", pagina, bytes[caso]);
        EXIGIR(dsm_liberar(ctx, id) == DSM_OK); id = 0;
    }
    void *base;
    EXIGIR(dsm_reservar(ctx, proceso, 4 * pagina, &id, &base) == DSM_OK);
    for (size_t p = 0; p < 4; p++) {
        uint16_t hogar; uint64_t gpn;
        EXIGIR(dsm_ubicacion_pagina(ctx, id, p, &hogar, &gpn) == DSM_OK && hogar == 0);
    }
    EXIGIR(dsm_liberar(ctx, id) == DSM_OK); id = 0;
    EXIGIR(dsm_cerrar(ctx) == DSM_OK); ctx = NULL;
    struct orden salir = {.salir = 1};
    EXIGIR(transferir(a[1], &salir, sizeof salir, 1) == 0 && transferir(b[0], &rc, sizeof rc, 0) == 0 && rc == DSM_OK);
    int estado;
    EXIGIR(waitpid(pid, &estado, 0) == pid && WIFEXITED(estado) && WEXITSTATUS(estado) == 0); pid = -1;
    printf("TAREAS ACEPTACION pagina=%zu reserva multipagina/multihogar, cola parcial y reutilizacion OK\n", pagina);
    resultado = 0;
terminar:
    if (ctx) { if (id) (void)dsm_liberar(ctx, id); (void)dsm_cerrar(ctx); }
    if (pid > 0) { kill(pid, SIGKILL); while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {} }
    for (size_t i = 0; i < 2; i++) { if (a[i] >= 0) close(a[i]); if (b[i] >= 0) close(b[i]); }
    if (config) fclose(config);
    return resultado;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);
    return casos(4096) != 0 || casos(8192) != 0;
}
