#include "consola_arranque.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct hijo { pid_t pid; consola_alta_t alta; int terminado, estado; };

static uint64_t reloj_ms(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) return 0;
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

static int ruta_consola(char ruta[PATH_MAX])
{
    ssize_t n = readlink("/proc/self/exe", ruta, PATH_MAX - 1);
    if (n <= 0 || n >= PATH_MAX - 1) return -1;
    ruta[n] = 0;
    char *barra = strrchr(ruta, '/');
    if (!barra || (size_t)(barra - ruta + 1) + sizeof "dsm_consola" > PATH_MAX) return -1;
    memcpy(barra + 1, "dsm_consola", sizeof "dsm_consola");
    return 0;
}

static int leer_alta(int fd, consola_alta_t *alta)
{
    unsigned char *p = (unsigned char *)alta;
    size_t faltan = sizeof *alta;
    uint64_t fin = reloj_ms() + 30000;
    while (faltan && reloj_ms() < fin) {
        struct pollfd f = {.fd = fd, .events = POLLIN};
        int rc = poll(&f, 1, 100);
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0) return -1;
        if (!rc) continue;
        ssize_t n = read(fd, p, faltan);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; faltan -= (size_t)n;
    }
    return faltan == 0 && alta->magic == CONSOLA_ALTA_MAGIC ? 0 : -1;
}

static int lanzar(struct hijo *h, const char *programa, const char *ini, int secundario)
{
    int canal[2];
    if (pipe2(canal, O_CLOEXEC) != 0) return -1;
    h->pid = fork();
    if (h->pid == 0) {
        close(canal[0]);
        char fd[32];
        snprintf(fd, sizeof fd, "%d", canal[1]);
        int banderas = fcntl(canal[1], F_GETFD);
        if (banderas >= 0 && fcntl(canal[1], F_SETFD, banderas & ~FD_CLOEXEC) == 0 &&
            (!secundario || setsid() >= 0)) {
            if (secundario) execl(programa, programa, ini, "--servir", "--alta-fd", fd, (char *)NULL);
            else execl(programa, programa, ini, "--alta-fd", fd, (char *)NULL);
        }
        perror("dsm_consola");
        consola_alta_t error = {.magic = CONSOLA_ALTA_MAGIC, .resultado = -5,
            .pid = (int32_t)getpid(), .nodo = UINT16_MAX};
        (void)write(canal[1], &error, sizeof error);
        _exit(1);
    }
    close(canal[1]);
    if (h->pid < 0) { close(canal[0]); return -1; }
    int rc = leer_alta(canal[0], &h->alta);
    close(canal[0]);
    if (rc != 0 || h->alta.pid != (int32_t)h->pid) {
        h->alta = (consola_alta_t){.magic = CONSOLA_ALTA_MAGIC, .resultado = -4,
            .pid = (int32_t)h->pid, .nodo = UINT16_MAX};
        /* No se fuerza la destruccion de un nodo que podria estar prestando. */
        kill(h->pid, SIGTERM);
    }
    printf("LANZADOR ALTA pid=%ld nodo=%u puerto=%u rc=%d rol=%s\n",
           (long)h->pid, h->alta.nodo, h->alta.puerto, h->alta.resultado,
           secundario ? "secundario" : "interactivo");
    return h->alta.resultado == 0 ? 0 : -1;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 2 || argc > 3) { fprintf(stderr, "uso: %s CONFIG [cantidad 1..8]\n", argv[0]); return 1; }
    long cantidad = 1;
    if (argc == 3) {
        char *fin;
        errno = 0;
        cantidad = strtol(argv[2], &fin, 10);
        if (errno || argv[2][0] < '0' || argv[2][0] > '9' || *fin || cantidad < 1 || cantidad > 8) return 1;
    }
    char programa[PATH_MAX];
    if (ruta_consola(programa) != 0) { perror("ruta dsm_consola"); return 1; }
    if (cantidad == 1) { execl(programa, programa, argv[1], (char *)NULL); perror(programa); return 1; }
    sigset_t senales;
    sigemptyset(&senales); sigaddset(&senales, SIGINT); sigaddset(&senales, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &senales, NULL) != 0) return 1;
    signal(SIGPIPE, SIG_IGN);
    struct hijo hijos[8] = {0};
    int resultado = 0;
    for (long i = 0; i < cantidad; i++) {
        if (lanzar(&hijos[i], programa, argv[1], i + 1 < cantidad) != 0) resultado = 1;
        if (hijos[i].pid <= 0) fprintf(stderr, "LANZADOR ERROR indice=%ld no se creo proceso\n", i);
    }
    unsigned altas = 0;
    for (long i = 0; i < cantidad; i++) altas += hijos[i].pid > 0 && hijos[i].alta.resultado == 0;
    printf("LANZADOR RESUMEN solicitados=%ld altas=%u fallos=%ld\n", cantidad, altas, cantidad - altas);
    long interactivo = cantidad - 1;
    if (hijos[interactivo].pid > 0) {
        while (!hijos[interactivo].terminado) {
            const struct timespec cero = {0};
            int signo = sigtimedwait(&senales, NULL, &cero);
            if (signo == SIGINT || signo == SIGTERM) kill(hijos[interactivo].pid, signo);
            for (long i = 0; i < cantidad; i++) if (hijos[i].pid > 0 && !hijos[i].terminado) {
                pid_t n = waitpid(hijos[i].pid, &hijos[i].estado, WNOHANG);
                if (n == hijos[i].pid) {
                    hijos[i].terminado = 1;
                    int salida = WIFEXITED(hijos[i].estado) ? WEXITSTATUS(hijos[i].estado) :
                                 WIFSIGNALED(hijos[i].estado) ? 128 + WTERMSIG(hijos[i].estado) : 1;
                    printf("LANZADOR FIN pid=%ld nodo=%u salida=%d\n", (long)hijos[i].pid, hijos[i].alta.nodo, salida);
                    if (salida) resultado = 1;
                } else if (n < 0 && errno != EINTR) { hijos[i].terminado = 1; resultado = 1; }
            }
            const struct timespec pausa = {.tv_nsec = 100000000};
            nanosleep(&pausa, NULL);
        }
    }
    for (long i = 0; i < interactivo; i++) if (hijos[i].pid > 0 && !hijos[i].terminado)
        printf("LANZADOR SECUNDARIO pid=%ld nodo=%u permanece_activo=si\n", (long)hijos[i].pid, hijos[i].alta.nodo);
    return resultado;
}
