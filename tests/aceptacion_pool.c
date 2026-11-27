#include "dsm.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sched.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "pool:%d: %s\n", __LINE__, #x); goto fin; } } while (0)

static int transferir(int fd, void *buf, size_t n, int escribir)
{
    unsigned char *p = buf;
    while (n) {
        ssize_t rc = escribir ? write(fd, p, n) : read(fd, p, n);
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0) return -1;
        p += rc; n -= (size_t)rc;
    }
    return 0;
}

static uint64_t ahora(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) return 0;
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

static int esperar_aviso(dsm_context_t *ctx, int presente)
{
    uint64_t limite = ahora() + 5000;
    do {
        int v = 0;
        int rc = dsm_aviso_consultar_autor(ctx, "latido", 1, &v, sizeof v);
        if (presente ? rc == DSM_OK && v == 7 : rc == DSM_ERR_ESTADO) return 1;
        struct timespec t = {.tv_nsec = 2000000};
        nanosleep(&t, NULL);
    } while (ahora() < limite);
    return 0;
}

static int hijo_puesto(const char *ruta, int inicio, int salida, int fin)
{
    char b;
    uint16_t id = UINT16_MAX;
    if (transferir(inicio, &b, 1, 0) != 0 || dsm_pool_reclamar(ruta, &id) != DSM_OK ||
        transferir(salida, &id, sizeof id, 1) != 0 || transferir(fin, &b, 1, 0) != 0) return 1;
    dsm_pool_liberar();
    return 0;
}

static int namespace_puesto(const char *ruta, int flags, uint16_t esperado)
{
    pid_t hijo = fork();
    if (hijo < 0) return 0;
    if (hijo == 0) {
        if (unshare(flags) != 0) _exit(77);
        if ((flags & CLONE_NEWNET) != 0) {
            int fd = socket(AF_INET, SOCK_DGRAM, 0);
            struct ifreq req = {0};
            strcpy(req.ifr_name, "lo");
            if (fd < 0 || ioctl(fd, SIOCGIFFLAGS, &req) != 0) _exit(1);
            req.ifr_flags |= IFF_UP;
            if (ioctl(fd, SIOCSIFFLAGS, &req) != 0) _exit(1);
            close(fd);
        }
        uint16_t id;
        int ok = dsm_pool_reclamar(ruta, &id) == DSM_OK && id == esperado;
        dsm_pool_liberar();
        _exit(ok ? 0 : 1);
    }
    int st;
    if (waitpid(hijo, &st, 0) != hijo || !WIFEXITED(st)) return 0;
    if (WEXITSTATUS(st) == 77) {
        printf("pool: namespace flags=%d no disponible; ensayo omitido\n", flags);
        return 1;
    }
    return WEXITSTATUS(st) == 0;
}

static int pruebas_pool(const char *ruta, const char *ejecutable)
{
    int resultado = 1;
    int inicio[2] = {-1, -1}, salida[2] = {-1, -1}, terminar[2] = {-1, -1};
    pid_t hijos[2] = {-1, -1};
    int ocupado = -1;
    CHECK(pipe(inicio) == 0 && pipe(salida) == 0 && pipe(terminar) == 0);
    for (unsigned i = 0; i < 2; i++) {
        hijos[i] = fork();
        CHECK(hijos[i] >= 0);
        if (hijos[i] == 0) _exit(hijo_puesto(ruta, inicio[0], salida[1], terminar[0]));
    }
    char salir[2] = {1, 1};
    CHECK(transferir(inicio[1], salir, 2, 1) == 0);
    uint16_t ids[2], id = UINT16_MAX;
    CHECK(transferir(salida[0], ids, sizeof ids, 0) == 0 && ids[0] != ids[1] &&
          ids[0] <= 1 && ids[1] <= 1);
    CHECK(dsm_pool_reclamar(ruta, &id) == DSM_OK && id == 2);
    dsm_pool_liberar();
    CHECK(transferir(terminar[1], salir, 2, 1) == 0);
    for (unsigned i = 0; i < 2; i++) {
        int st;
        CHECK(waitpid(hijos[i], &st, 0) == hijos[i] && WIFEXITED(st) && WEXITSTATUS(st) == 0);
        hijos[i] = -1;
    }
    /* Mismo puerto en dos IPv4 locales: no colisionan los candados. */
    ocupado = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in dir = {.sin_family = AF_INET, .sin_port = htons(27296),
                               .sin_addr.s_addr = htonl(0x7f000001u)};
    CHECK(ocupado >= 0 && bind(ocupado, (struct sockaddr *)&dir, sizeof dir) == 0 &&
          listen(ocupado, 1) == 0);
    CHECK(dsm_pool_reclamar(ruta, &id) == DSM_OK && id == 1);
    dsm_pool_liberar();
    close(ocupado); ocupado = -1;
    CHECK(dsm_pool_reclamar(ruta, &id) == DSM_OK && id == 0);
    CHECK(dsm_pool_reclamar(ruta, &id) == DSM_OK && id == 0);
    /* Otra userns conserva exclusion; otra netns tiene puestos propios. */
    CHECK(namespace_puesto(ruta, CLONE_NEWUSER, 1));
    CHECK(namespace_puesto(ruta, CLONE_NEWNET, 0));
    hijos[0] = fork();
    CHECK(hijos[0] >= 0);
    if (hijos[0] == 0) {
        uint16_t propio;
        int ok = dsm_pool_reclamar(ruta, &propio) == DSM_OK && propio == 1;
        dsm_pool_liberar();
        _exit(ok ? 0 : 1);
    }
    int st;
    CHECK(waitpid(hijos[0], &st, 0) == hijos[0] && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    hijos[0] = -1;
    dsm_pool_liberar();

    /* El fd guardia sobrevive al exec utilizado por dsm_unir. */
    hijos[0] = fork();
    CHECK(hijos[0] >= 0);
    if (hijos[0] == 0) {
        uint16_t propio;
        if (dsm_pool_reclamar(ruta, &propio) != DSM_OK || propio != 0) _exit(1);
        char fd1[32], fd2[32];
        snprintf(fd1, sizeof fd1, "%d", salida[1]);
        snprintf(fd2, sizeof fd2, "%d", terminar[0]);
        execl(ejecutable, ejecutable, "--esperar", fd1, fd2, (char *)NULL);
        _exit(1);
    }
    CHECK(transferir(salida[0], &id, sizeof id, 0) == 0 && id == 0);
    CHECK(dsm_pool_reclamar(ruta, &id) == DSM_OK && id == 1);
    dsm_pool_liberar();
    CHECK(transferir(terminar[1], salir, 1, 1) == 0);
    CHECK(waitpid(hijos[0], &st, 0) == hijos[0] && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    hijos[0] = -1;
    CHECK(dsm_pool_reclamar(ruta, &id) == DSM_OK && id == 0);
    dsm_pool_liberar();
    puts("pool: reclamo simultaneo, IPv4+puerto, puerto ocupado, fork, exec y reuso OK");
    resultado = 0;
fin:
    dsm_pool_liberar();
    for (unsigned i = 0; i < 2; i++) if (hijos[i] > 0) { kill(hijos[i], SIGKILL); waitpid(hijos[i], NULL, 0); }
    for (unsigned i = 0; i < 2; i++) {
        if (inicio[i] >= 0) close(inicio[i]);
        if (salida[i] >= 0) close(salida[i]);
        if (terminar[i] >= 0) close(terminar[i]);
    }
    if (ocupado >= 0) close(ocupado);
    return resultado;
}

static int pruebas_latidos(const char *ruta)
{
    int resultado = 1, listo[2] = {-1, -1}, terminar[2] = {-1, -1};
    pid_t hijo = -1;
    dsm_context_t *ctx = NULL;
    CHECK(pipe(listo) == 0 && pipe(terminar) == 0);
    hijo = fork();
    CHECK(hijo >= 0);
    if (hijo == 0) {
        dsm_context_t *par = NULL;
        int valor = 7;
        char b = 1;
        if (dsm_iniciar(ruta, 1, &par) != DSM_OK ||
            dsm_aviso_publicar(par, "latido", &valor, sizeof valor) != DSM_OK ||
            transferir(listo[1], &b, 1, 1) != 0 || transferir(terminar[0], &b, 1, 0) != 0)
            _exit(1);
        _exit(dsm_cerrar(par) == DSM_OK ? 0 : 1);
    }
    uint16_t id;
    CHECK(dsm_pool_reclamar(ruta, &id) == DSM_OK && id == 0);
    CHECK(dsm_iniciar(ruta, id, &ctx) == DSM_OK);
    char b;
    CHECK(transferir(listo[0], &b, 1, 0) == 0);
    CHECK(dsm_sondear(ctx, 600) == DSM_OK);
    CHECK(esperar_aviso(ctx, 1));
    CHECK(kill(hijo, SIGSTOP) == 0);
    /* La difusion de avisos no ocupa el lector de heartbeat. */
    for (unsigned i = 0; i < 20; i++) {
        char clave[32];
        snprintf(clave, sizeof clave, "en-pausa-%u", i);
        CHECK(dsm_aviso_publicar(ctx, clave, &i, sizeof i) == DSM_OK);
    }
    CHECK(esperar_aviso(ctx, 0));
    uint64_t sesion;
    CHECK(dsm_aviso_sesion(ctx, 1, &sesion) == DSM_ERR_NODO);
    CHECK(kill(hijo, SIGCONT) == 0);
    CHECK(dsm_sondear(ctx, 1000) == DSM_OK);
    CHECK(esperar_aviso(ctx, 1));
    b = 1;
    CHECK(transferir(terminar[1], &b, 1, 1) == 0);
    int st;
    CHECK(waitpid(hijo, &st, 0) == hijo && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    hijo = -1;
    CHECK(dsm_cerrar(ctx) == DSM_OK); ctx = NULL;
    /* El cierre devuelve el puesto y no deja avisos de otro contexto. */
    CHECK(dsm_pool_reclamar(ruta, &id) == DSM_OK && id == 0);
    CHECK(dsm_iniciar(ruta, id, &ctx) == DSM_OK);
    CHECK(dsm_aviso_consultar(ctx, "en-pausa-0", &id, &st, sizeof st) == DSM_ERR_ESTADO);
    CHECK(dsm_cerrar(ctx) == DSM_OK); ctx = NULL;
    puts("pool: SIGSTOP/heartbeat, reincorporacion, cierre y contexto nuevo OK");
    resultado = 0;
fin:
    if (hijo > 0) { kill(hijo, SIGKILL); waitpid(hijo, NULL, 0); }
    if (ctx != NULL) dsm_cerrar(ctx);
    dsm_pool_liberar();
    for (unsigned i = 0; i < 2; i++) {
        if (listo[i] >= 0) close(listo[i]);
        if (terminar[i] >= 0) close(terminar[i]);
    }
    return resultado;
}

int main(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "--esperar") == 0) {
        uint16_t id = 0; char b;
        return transferir(atoi(argv[2]), &id, sizeof id, 1) == 0 &&
               transferir(atoi(argv[3]), &b, 1, 0) == 0 ? 0 : 1;
    }
    alarm(40);
    setvbuf(stdout, NULL, _IOLBF, 0);
    char ruta[] = "/tmp/dsm-pool-prueba-XXXXXX";
    int fd = mkstemp(ruta);
    if (fd < 0) return 1;
    FILE *f = fdopen(fd, "w");
    if (f == NULL) { close(fd); unlink(ruta); return 1; }
    fputs("page_size=4096\nmemoria_inicial=65536\nmax_nodos=3\nmax_hilos=4\n"
          "heartbeat_intervalo_ms=100\nheartbeat_fallos=5\nrpc_timeout_ms=1000\n"
          "puerto_rango_inicio=27200\npuerto_rango_fin=27299\n"
          "nodo=0 pool-0 127.0.0.1 27296\nnodo=1 pool-1 127.0.0.2 27296\n"
          "nodo=2 pool-2 127.0.0.1 27297\n", f);
    if (fclose(f) != 0) { unlink(ruta); return 1; }
    int rc = pruebas_pool(ruta, argv[0]);
    if (rc == 0) rc = pruebas_latidos(ruta);
    unlink(ruta);
    return rc;
}
