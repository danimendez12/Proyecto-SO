#include "../apps/demanda_grupo.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* Ejecutables reales, donantes persistentes y grupos de hasta tres procesos.
 * No simula coherencia, calculo ni avisos. Solo usa loopback autorizado. */
#define EXIGIR(c) do { if (!(c)) { fprintf(stderr, "aceptacion_demanda:%d: %s\n", __LINE__, #c); goto terminar; } } while (0)

enum { PAGINA = 4096, PAGINAS = 16, NODOS = 5 };

struct proceso { pid_t pid; FILE *log; int recogido, estado, entrada, salida; };

static void pausa(void) { const struct timespec t = {.tv_nsec = 1000000}; nanosleep(&t, NULL); }

static int transferir(int fd, void *datos, size_t bytes, int escribir)
{
    unsigned char *p = datos;
    while (bytes) {
        if (!escribir) {
            struct pollfd f = {.fd = fd, .events = POLLIN};
            int rc;
            do { rc = poll(&f, 1, 15000); } while (rc < 0 && errno == EINTR);
            if (rc <= 0) return -1;
        }
        ssize_t n = escribir ? write(fd, p, bytes) : read(fd, p, bytes);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; bytes -= (size_t)n;
    }
    return 0;
}

static char *texto(const struct proceso *p)
{
    if (!p->log) return NULL;
    struct stat st;
    if (fstat(fileno(p->log), &st) || st.st_size < 0 || st.st_size > 32 * 1024 * 1024) return NULL;
    size_t n = (size_t)st.st_size, leidos = 0;
    char *s = malloc(n + 1);
    if (!s) return NULL;
    while (leidos < n) {
        ssize_t rc = pread(fileno(p->log), s + leidos, n - leidos, (off_t)leidos);
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0) { free(s); return NULL; }
        leidos += (size_t)rc;
    }
    s[n] = 0; return s;
}

static int contiene(const struct proceso *p, const char *marca)
{
    char *s = texto(p);
    char *pos = s ? strstr(s, marca) : NULL;
    int ok = pos && strchr(pos, '\n');
    free(s); return ok;
}

static int revisar(struct proceso *p)
{
    if (p->recogido) return 1;
    pid_t rc;
    do { rc = waitpid(p->pid, &p->estado, WNOHANG); } while (rc < 0 && errno == EINTR);
    if (rc == p->pid) { p->recogido = 1; return 1; }
    return rc == 0 ? 0 : -1;
}

static int esperar(struct proceso *p, const char *marca, unsigned ms)
{
    uint64_t fin = demanda_ahora_ms() + ms;
    do {
        if (marca ? contiene(p, marca) : revisar(p) == 1) return 0;
        if (marca && revisar(p) != 0) return -1;
        pausa();
    } while (demanda_ahora_ms() < fin);
    return -1;
}

static int codigo(const struct proceso *p, int exito)
{ return p->recogido && WIFEXITED(p->estado) && (exito ? WEXITSTATUS(p->estado) == 0 : WEXITSTATUS(p->estado) != 0); }

static void cerrar(struct proceso *p, int mostrar)
{
    if (p->pid > 0 && !p->recogido) {
        kill(p->pid, SIGCONT);
        kill(p->pid, SIGTERM);
        if (esperar(p, NULL, 1000) != 0) {
            kill(p->pid, SIGKILL);
            while (waitpid(p->pid, &p->estado, 0) < 0 && errno == EINTR) {}
            p->recogido = 1;
        }
    }
    if (mostrar && p->log) {
        char *s = texto(p);
        fprintf(stderr, "--- demanda pid=%ld status=%d ---\n%s", (long)p->pid, p->estado, s ? s : "sin log\n");
        free(s);
    }
    if (p->log) { fclose(p->log); p->log = NULL; }
    if (p->entrada > 0) { close(p->entrada); p->entrada = 0; }
    if (p->salida > 0) { close(p->salida); p->salida = 0; }
}

static int redirigir(struct proceso *p)
{
    int fd = open("/dev/null", O_RDONLY);
    if (fd < 0 || dup2(fd, STDIN_FILENO) < 0 || dup2(fileno(p->log), STDOUT_FILENO) < 0 ||
        dup2(fileno(p->log), STDERR_FILENO) < 0) return -1;
    close(fd); return 0;
}

static int lanzar(struct proceso *p, const char *ini, int matriz, const char *grupo, unsigned plazo,
                   int grande)
{
    p->log = tmpfile();
    if (!p->log) return -1;
    p->pid = fork();
    if (!p->pid) {
        if (redirigir(p)) _exit(125);
        char segundos[16]; snprintf(segundos, sizeof segundos, "%u", plazo);
        setenv("DSM_DEMANDA_ESPERA_MS", "500", 1);
        setenv("DSM_DEMANDA_PLAZO_SEG", segundos, 1);
        if (grupo) setenv("DSM_DEMANDA_GRUPO", grupo, 1); else unsetenv("DSM_DEMANDA_GRUPO");
        if (matriz) execl("./bin/apps/matrices", "matrices", ini, grande ? "96" : "64",
                          "47", grande ? "96" : "64", (char *)NULL);
        else execl("./bin/apps/mergesort", "mergesort", ini, grande == 2 ? "1000003" : grande ? "65539" : "16385",
                    "repetidos", "2026", (char *)NULL);
        _exit(127);
    }
    return p->pid > 0 ? 0 : -1;
}

static int probar_capacidad(dsm_context_t *ctx, dsm_proc_id_t proc, uint16_t id)
{
    dsm_reserva_id_t reserva = 0;
    void *ar;
    int rc = dsm_reservar(ctx, proc, PAGINAS * PAGINA, &reserva, &ar);
    for (size_t p = 0; p < PAGINAS && rc == DSM_OK; p++) {
        uint16_t hogar;
        uint64_t gpn;
        rc = dsm_ubicacion_pagina(ctx, reserva, p, &hogar, &gpn);
        if (rc == DSM_OK && hogar != id) rc = DSM_ERR_ESTADO;
    }
    if (reserva) { int otro = dsm_liberar(ctx, reserva); if (rc == DSM_OK) rc = otro; }
    return rc;
}

static int donante(struct proceso *p, const char *ini, uint16_t id)
{
    int a[2], b[2];
    if (pipe2(a, O_CLOEXEC) || pipe2(b, O_CLOEXEC)) return -1;
    p->log = tmpfile();
    if (!p->log) return -1;
    p->pid = fork();
    if (!p->pid) {
        close(a[1]); close(b[0]);
        if (redirigir(p)) _exit(125);
        setvbuf(stdout, NULL, _IONBF, 0);
        dsm_context_t *ctx = NULL;
        dsm_proc_id_t proc = 0;
        int rc = dsm_iniciar(ini, id, &ctx);
        if (rc == DSM_OK) rc = dsm_proc_abrir(ctx, &proc);
        if (transferir(b[1], &rc, sizeof rc, 1)) _exit(124);
        char orden;
        while (rc == DSM_OK) {
            ssize_t recibidos;
            do { recibidos = read(a[0], &orden, 1); } while (recibidos < 0 && errno == EINTR);
            if (recibidos != 1) break;
            if (orden == 'P') rc = probar_capacidad(ctx, proc, id);
            else if (orden == 'X') rc = demanda_contexto_cerrar(&ctx, demanda_ahora_ms() + 5000);
            else rc = DSM_ERR_INVAL;
            if (transferir(b[1], &rc, sizeof rc, 1)) _exit(124);
            if (orden == 'X') break;
        }
        _exit(rc == DSM_OK ? 0 : 1);
    }
    close(a[0]); close(b[1]); p->entrada = a[1]; p->salida = b[0];
    int rc;
    return p->pid > 0 && transferir(p->salida, &rc, sizeof rc, 0) == 0 && rc == DSM_OK ? 0 : -1;
}

static int orden(struct proceso *p, char c)
{
    int rc;
    return transferir(p->entrada, &c, 1, 1) == 0 && transferir(p->salida, &rc, sizeof rc, 0) == 0 &&
           rc == DSM_OK ? 0 : -1;
}

static int token(const struct proceso *p, uint64_t *salida)
{
    char *s = texto(p), *pos = s ? strstr(s, " sesion=") : NULL;
    int rc = pos && sscanf(pos, " sesion=%" SCNx64, salida) == 1 && *salida != 0 ? 0 : -1;
    free(s); return rc;
}

static int sesion(const char *ini, int matriz, unsigned n, uint64_t *anterior)
{
    struct proceso p[3] = {0};
    int rc = 1;
    uint64_t inicio = demanda_ahora_ms();
    EXIGIR(n >= 2 && n <= 3);
    EXIGIR(lanzar(&p[0], ini, matriz, NULL, 15, 0) == 0);
    EXIGIR(esperar(&p[0], "PUESTO id=1", 3000) == 0);
    for (unsigned i = 1; i < n; i++) EXIGIR(lanzar(&p[i], ini, matriz, NULL, 15, 0) == 0);
    uint64_t comun = 0;
    for (unsigned i = 0; i < n; i++) {
        EXIGIR(esperar(&p[i], NULL, 20000) == 0 && codigo(&p[i], 1));
        uint64_t actual;
        EXIGIR(token(&p[i], &actual) == 0 && (i == 0 || actual == comun));
        if (!i) comun = actual;
        EXIGIR(contiene(&p[i], n == 2 ? "participantes=2 creador=1" : "participantes=3 creador=1"));
        EXIGIR(contiene(&p[i], n == 2 ? " ids=1,3" : " ids=1,3,4"));
        EXIGIR(contiene(&p[i], i == 0 ? "LIBERACION OK" : "TRABAJADOR OK"));
        EXIGIR(!contiene(&p[i], "DEMANDA ERROR"));
    }
    EXIGIR(comun != *anterior); *anterior = comun;
    EXIGIR(demanda_ahora_ms() - inicio >= 450);
    EXIGIR(contiene(&p[0], matriz ? "VERIFICACION OK" :
                     "VERIFICACION OK elementos=16385 comparados=16385 ordenado=si coincide=si"));
    EXIGIR(contiene(&p[0], "hogar=0"));
    for (unsigned i = 1; i < n; i++) EXIGIR(!contiene(&p[i], "VERIFICACION OK"));
    printf("DEMANDA REGRESION %s n=%u creador=1 ids=%s token=%016" PRIx64 " verificacion y liberacion OK\n",
           matriz ? "matrices rectangular" : "mergesort impar", n, n == 2 ? "1,3" : "1,3,4", comun);
    rc = 0;
terminar:
    for (unsigned i = 0; i < 3; i++) cerrar(&p[i], rc != 0);
    return rc;
}

static int independientes(const char *ini)
{
    struct proceso p[2] = {0};
    int rc = 1;
    /* Dos ejecuciones simultaneas con identicos parametros, grupos distintos. */
    EXIGIR(lanzar(&p[0], ini, 1, "independiente-A", 15, 0) == 0);
    EXIGIR(lanzar(&p[1], ini, 1, "independiente-B", 15, 0) == 0);
    uint64_t tokens[2];
    for (unsigned i = 0; i < 2; i++) {
        EXIGIR(esperar(&p[i], NULL, 20000) == 0 && codigo(&p[i], 1));
        EXIGIR(contiene(&p[i], "DEMANDA participantes=1") && contiene(&p[i], "VERIFICACION OK") &&
               contiene(&p[i], "LIBERACION OK") && token(&p[i], &tokens[i]) == 0);
    }
    EXIGIR(tokens[0] != tokens[1]);
    puts("DEMANDA REGRESION grupos independientes: mismas dimensiones, dos creadores y tokens distintos OK");
    rc = 0;
terminar:
    for (unsigned i = 0; i < 2; i++) cerrar(&p[i], rc != 0);
    return rc;
}

static int controlado(struct proceso *p, const char *ini, int fallo)
{
    int a[2], b[2];
    if (pipe2(a, O_CLOEXEC) || pipe2(b, O_CLOEXEC)) return -1;
    p->log = tmpfile();
    if (!p->log) return -1;
    p->pid = fork();
    if (!p->pid) {
        close(a[1]); close(b[0]);
        if (redirigir(p)) _exit(125);
        setvbuf(stdout, NULL, _IONBF, 0);
        uint16_t id = 0;
        dsm_context_t *ctx = NULL;
        demanda_grupo_t g = {0};
        dsm_reserva_id_t reserva = 0;
        dsm_proc_id_t proc = 0;
        int rc = dsm_pool_reclamar(ini, &id);
        if (rc == DSM_OK) rc = dsm_iniciar(ini, id, &ctx);
        if (rc == DSM_OK) printf("PUESTO id=%u\n", id);
        if (rc == DSM_OK) rc = dsm_proc_abrir(ctx, &proc);
        setenv("DSM_DEMANDA_ESPERA_MS", "500", 1);
        setenv("DSM_DEMANDA_PLAZO_SEG", fallo && id == 1 ? "4" : "15", 1);
        unsetenv("DSM_DEMANDA_GRUPO");
        if (rc == DSM_OK) rc = demanda_grupo_iniciar(&g, ctx, id, "mergesort:16385:repetidos:2026");
        int propia = g.lider == id;
        if (rc == DSM_OK && propia) {
            void *base;
            rc = dsm_reservar(ctx, proc, PAGINA, &reserva, &base);
            if (rc == DSM_OK) rc = demanda_grupo_publicar(&g, &reserva, sizeof reserva);
        } else if (rc == DSM_OK) {
            rc = demanda_grupo_recibir(&g, &reserva, sizeof reserva);
            void *base;
            size_t bytes;
            if (rc == DSM_OK) rc = dsm_abrir_reserva(ctx, reserva, &base, &bytes);
            if (rc == DSM_OK && bytes != PAGINA) rc = DSM_ERR_PROTO;
        }
        if (transferir(b[1], &rc, sizeof rc, 1)) _exit(124);
        char orden;
        if (rc == DSM_OK && transferir(a[0], &orden, 1, 0) == 0 && orden == 'R') {
            do {
                rc = demanda_grupo_estado(&g);
                if (fallo && !propia && rc == DSM_OK) pausa();
            } while (fallo && !propia && rc == DSM_OK);
            if (rc == DSM_OK) rc = demanda_marcar(&g, reserva, 1, DSM_OK);
            if (rc == DSM_OK && propia) rc = demanda_marcas_esperar(&g, reserva, 1);
            if (rc != DSM_OK) demanda_grupo_error(&g, rc);
            printf("CONTROL RESULTADO puesto=%u DSM=%d\n", id, rc);
            if (transferir(b[1], &rc, sizeof rc, 1)) _exit(124);
            if (transferir(a[0], &orden, 1, 0) != 0 || orden != 'X') _exit(124);
        }
        int limpieza = demanda_reserva_retirar(ctx, &reserva, propia, demanda_ahora_ms() + 8000);
        int otro = demanda_grupo_retirar(&g);
        if (limpieza == DSM_OK) limpieza = otro;
        otro = demanda_contexto_cerrar(&ctx, demanda_ahora_ms() + 8000);
        if (limpieza == DSM_OK) limpieza = otro;
        _exit(limpieza == DSM_OK ? 0 : 1);
    }
    close(a[0]); close(b[1]); p->entrada = a[1]; p->salida = b[0];
    return p->pid > 0 ? 0 : -1;
}

static int coordinacion(const char *ini, int fallo)
{
    struct proceso p[3] = {0};
    int rc = 1, respuesta;
    char avanzar = 'R', salir = 'X';
    EXIGIR(controlado(&p[0], ini, fallo) == 0);
    EXIGIR(esperar(&p[0], "PUESTO id=1", 3000) == 0);
    EXIGIR(controlado(&p[1], ini, fallo) == 0);
    for (unsigned i = 0; i < 2; i++)
        EXIGIR(transferir(p[i].salida, &respuesta, sizeof respuesta, 0) == 0 && respuesta == DSM_OK);
    /* Ambos contextos abrieron el control, pero ninguna marca fue publicada.
     * Las pipes bloquean solo el caller; todos los hilos TCP siguen activos. */
    if (!fallo) {
        EXIGIR(lanzar(&p[2], ini, 0, NULL, 5, 0) == 0);
        EXIGIR(esperar(&p[2], NULL, 8000) == 0 && codigo(&p[2], 0));
        EXIGIR(contiene(&p[2], "DEMANDA llegada tardia") && !contiene(&p[2], "VERIFICACION OK"));
    }
    EXIGIR(transferir(p[0].entrada, &avanzar, 1, 1) == 0);
    if (!fallo) EXIGIR(transferir(p[1].entrada, &avanzar, 1, 1) == 0);
    EXIGIR(transferir(p[0].salida, &respuesta, sizeof respuesta, 0) == 0 &&
           respuesta == (fallo ? DSM_ERR_TIEMPO : DSM_OK));
    if (fallo) EXIGIR(transferir(p[1].entrada, &avanzar, 1, 1) == 0);
    EXIGIR(transferir(p[1].salida, &respuesta, sizeof respuesta, 0) == 0 &&
           respuesta == (fallo ? DSM_ERR_TIEMPO : DSM_OK));
    for (unsigned i = 0; i < 2; i++) EXIGIR(transferir(p[i].entrada, &salir, 1, 1) == 0);
    for (unsigned i = 0; i < 2; i++) EXIGIR(esperar(&p[i], NULL, 10000) == 0 && codigo(&p[i], 1));
    puts(fallo ? "DEMANDA REGRESION marca retenida: plazo del lider propagado a miembro con plazo vigente, limpieza OK" :
                 "DEMANDA REGRESION llegada tardia: rechazo OCUPADA y barrera de marcas confirmada por pipes");
    rc = 0;
terminar:
    for (unsigned i = 0; i < 3; i++) cerrar(&p[i], rc != 0);
    return rc;
}

static int insuficiente(const char *ini)
{
    struct proceso p[2] = {0};
    int rc = 1;
    for (unsigned i = 0; i < 2; i++) EXIGIR(lanzar(&p[i], ini, 0, NULL, 10, 1) == 0);
    for (unsigned i = 0; i < 2; i++) {
        EXIGIR(esperar(&p[i], NULL, 18000) == 0 && codigo(&p[i], 0));
        EXIGIR(!contiene(&p[i], "VERIFICACION OK") && !contiene(&p[i], "LIBERACION OK"));
    }
    puts("DEMANDA REGRESION capacidad insuficiente: ambos participantes fallan sin verificar resultado incompleto");
    rc = 0;
terminar:
    for (unsigned i = 0; i < 2; i++) cerrar(&p[i], rc != 0);
    return rc;
}

static int caida_real(void)
{
    struct proceso p[2] = {0};
    FILE *config = tmpfile();
    int rc = 1;
    EXIGIR(config != NULL);
    fprintf(config, "page_size=4096\nmemoria_inicial=4194304\nmax_nodos=2\nmax_hilos=8\n"
                    "heartbeat_intervalo_ms=100\nheartbeat_fallos=3\nrpc_timeout_ms=500\n"
                    "puerto_rango_inicio=27200\npuerto_rango_fin=27299\n"
                    "nodo=0 creador 127.0.0.1 27200\nnodo=1 trabajador 127.0.0.1 27201\n");
    EXIGIR(fflush(config) == 0);
    char ini[64]; snprintf(ini, sizeof ini, "/proc/self/fd/%d", fileno(config));
    EXIGIR(lanzar(&p[0], ini, 0, "caida-real", 8, 2) == 0);
    EXIGIR(esperar(&p[0], "PUESTO id=0", 3000) == 0);
    EXIGIR(lanzar(&p[1], ini, 0, "caida-real", 8, 2) == 0);
    EXIGIR(esperar(&p[1], "TRABAJO nodo=1", 7000) == 0);
    EXIGIR(kill(p[1].pid, SIGKILL) == 0);
    EXIGIR(esperar(&p[1], NULL, 2000) == 0 && WIFSIGNALED(p[1].estado) && WTERMSIG(p[1].estado) == SIGKILL);
    EXIGIR(esperar(&p[0], NULL, 18000) == 0 && codigo(&p[0], 0));
    EXIGIR(contiene(&p[0], "ERROR demanda DSM=") && !contiene(&p[0], "VERIFICACION OK") &&
           !contiene(&p[0], "LIBERACION OK"));
    puts("DEMANDA REGRESION caida real: SIGKILL del trabajador durante fase, creador sale con error controlado");
    puts("DEMANDA LIMITE: no se exige recuperar paginas M ambiguas; caso usa namespace y contextos propios");
    rc = 0;
terminar:
    for (unsigned i = 0; i < 2; i++) cerrar(&p[i], rc != 0);
    if (config) fclose(config);
    return rc;
}

static int caida_aislada(void)
{
    struct proceso p = {0};
    int rc = 1;
    p.log = tmpfile();
    EXIGIR(p.log != NULL);
    p.pid = fork();
    if (!p.pid) {
        if (redirigir(&p)) _exit(125);
        execlp("unshare", "unshare", "-n", "sh", "-c",
               "ip link set lo up; exec ./bin/tests/aceptacion_demanda --caida", (char *)NULL);
        _exit(127);
    }
    EXIGIR(p.pid > 0 && esperar(&p, NULL, 30000) == 0 && codigo(&p, 1));
    EXIGIR(contiene(&p, "DEMANDA REGRESION caida real:"));
    puts("DEMANDA REGRESION caida real: proceso SIGKILL y fallo del creador comprobados en namespace separado");
    rc = 0;
terminar:
    cerrar(&p, rc != 0);
    return rc;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);
    if (argc == 2 && strcmp(argv[1], "--caida") == 0) return caida_real();
    if (argc != 1) return 2;
    struct proceso d[2] = {0};
    FILE *config = tmpfile();
    int resultado = 1;
    EXIGIR(config != NULL);
    fprintf(config, "page_size=4096\nmemoria_inicial=65536\nmax_nodos=5\nmax_hilos=8\n"
                    "heartbeat_intervalo_ms=3000\nheartbeat_fallos=10\nrpc_timeout_ms=800\n"
                    "puerto_rango_inicio=27200\npuerto_rango_fin=27299\n");
    for (unsigned i = 0; i < NODOS; i++) fprintf(config, "nodo=%u demanda%u 127.0.0.1 %u\n", i, i, 27200 + i);
    EXIGIR(fflush(config) == 0);
    char ini[64]; snprintf(ini, sizeof ini, "/proc/self/fd/%d", fileno(config));
    struct timespec inicio = {.tv_sec = 10, .tv_nsec = 900000000};
    struct timespec fin = {.tv_sec = 11, .tv_nsec = 100000000};
    EXIGIR(demanda_transcurrido_ms(&inicio, &fin) == 200);
    EXIGIR(demanda_transcurrido_ms(&fin, &inicio) == 0);
    puts("DEMANDA REGRESION CLOCK_MONOTONIC: cruce de segundo conserva 200ms");
    EXIGIR(donante(&d[0], ini, 0) == 0 && donante(&d[1], ini, 2) == 0);
    EXIGIR(orden(&d[0], 'P') == 0 && orden(&d[1], 'P') == 0);
    uint64_t anterior_mx = 0, anterior_ms = 0;
    for (unsigned repeticion = 0; repeticion < 2; repeticion++) {
        EXIGIR(sesion(ini, 1, 2, &anterior_mx) == 0);
        EXIGIR(sesion(ini, 1, 3, &anterior_mx) == 0);
        EXIGIR(sesion(ini, 0, 3, &anterior_ms) == 0);
        EXIGIR(orden(&d[0], 'P') == 0 && orden(&d[1], 'P') == 0);
    }
    EXIGIR(independientes(ini) == 0);
    EXIGIR(orden(&d[0], 'P') == 0 && orden(&d[1], 'P') == 0);
    EXIGIR(coordinacion(ini, 0) == 0);
    EXIGIR(orden(&d[0], 'P') == 0 && orden(&d[1], 'P') == 0);
    EXIGIR(coordinacion(ini, 1) == 0);
    EXIGIR(orden(&d[0], 'P') == 0 && orden(&d[1], 'P') == 0);
    EXIGIR(insuficiente(ini) == 0);
    EXIGIR(orden(&d[0], 'P') == 0 && orden(&d[1], 'P') == 0);
    EXIGIR(caida_aislada() == 0);
    puts("DEMANDA REGRESION capacidad: 16 paginas propias de cada donante reutilizables tras cada caso");
    EXIGIR(orden(&d[0], 'X') == 0 && orden(&d[1], 'X') == 0);
    EXIGIR(esperar(&d[0], NULL, 5000) == 0 && codigo(&d[0], 1));
    EXIGIR(esperar(&d[1], NULL, 5000) == 0 && codigo(&d[1], 1));
    resultado = 0;
terminar:
    for (unsigned i = 0; i < 2; i++) cerrar(&d[i], resultado != 0);
    if (config) fclose(config);
    return resultado;
}
