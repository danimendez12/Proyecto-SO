#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Solo coordina ejecutables reales. Ningun dato de las matrices pasa por el test. */
#define EXIGIR(condicion) do { \
    if (!(condicion)) { \
        fprintf(stderr, "aceptacion_app_matrices:%d: %s\n", __LINE__, #condicion); \
        goto terminar; \
    } \
} while (0)

struct aplicacion {
    pid_t pid;
    FILE *registro;
    const char *rol;
    int recogida;
    int estado;
};

static double ahora(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) {
        return -1;
    }
    return (double)t.tv_sec + (double)t.tv_nsec / 1000000000.0;
}

static FILE *temporal(void)
{
    FILE *archivo = tmpfile();
    if (archivo != NULL) {
        int fd = fileno(archivo);
        int flags = fcntl(fd, F_GETFD);
        if (flags == -1 || fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC) == -1) {
            fclose(archivo);
            return NULL;
        }
    }
    return archivo;
}

static void ruta_temporal(FILE *archivo, char ruta[64])
{
    snprintf(ruta, 64, "/proc/self/fd/%d", fileno(archivo));
}

static FILE *configurar(unsigned nodos, size_t paginas0, size_t paginas1, size_t paginas2)
{
    FILE *config = temporal();
    if (config == NULL) {
        return NULL;
    }
    const size_t paginas[] = {paginas0, paginas1, paginas2};
    int rc = fprintf(config,
        "page_size=4096\nmemoria_inicial=4096\nmax_nodos=%u\nmax_hilos=8\n"
        "rpc_timeout_ms=1500\nheartbeat_intervalo_ms=300\nheartbeat_fallos=10\n"
        "puerto_rango_inicio=27200\npuerto_rango_fin=27299\n", nodos);
    for (unsigned i = 0; i < nodos && rc >= 0; i++) {
        rc = fprintf(config, "nodo=%u matriz-%u 127.0.0.1 %u %zu\n",
                     i, i, 27200 + i, paginas[i] * 4096);
    }
    if (rc < 0 || fflush(config) != 0) {
        fclose(config);
        return NULL;
    }
    return config;
}

static char *registro(const struct aplicacion *app)
{
    if (app->registro == NULL) {
        return NULL;
    }
    struct stat st;
    if (fstat(fileno(app->registro), &st) != 0 || st.st_size < 0 ||
        st.st_size > 8 * 1024 * 1024) {
        return NULL;
    }
    size_t bytes = (size_t)st.st_size;
    char *texto = malloc(bytes + 1);
    if (texto == NULL) {
        return NULL;
    }
    size_t n = 0;
    while (n < bytes) {
        ssize_t leidos = pread(fileno(app->registro), texto + n, bytes - n, (off_t)n);
        if (leidos < 0 && errno == EINTR) {
            continue;
        }
        if (leidos <= 0) {
            free(texto);
            return NULL;
        }
        n += (size_t)leidos;
    }
    texto[bytes] = '\0';
    return texto;
}

static int contiene(const struct aplicacion *app, const char *marca)
{
    char *texto = registro(app);
    int encontrada = texto != NULL && strstr(texto, marca) != NULL;
    free(texto);
    return encontrada;
}

static int revisar(struct aplicacion *app)
{
    if (app->recogida) {
        return 1;
    }
    pid_t rc;
    do {
        rc = waitpid(app->pid, &app->estado, WNOHANG);
    } while (rc == -1 && errno == EINTR);
    if (rc == app->pid) {
        app->recogida = 1;
        return 1;
    }
    return rc == 0 ? 0 : -1;
}

static int esperar_fin(struct aplicacion *app, double segundos)
{
    double fin = ahora() + segundos;
    do {
        int rc = revisar(app);
        if (rc != 0) {
            return rc == 1 ? 0 : -1;
        }
        const struct timespec pausa = {.tv_nsec = 5000000};
        nanosleep(&pausa, NULL);
    } while (ahora() < fin);
    return -1;
}

static int iniciar(struct aplicacion *app, const char *rol, char *const argumentos[])
{
    app->rol = rol;
    app->registro = tmpfile();
    if (app->registro == NULL) {
        return -1;
    }
    app->pid = fork();
    if (app->pid == 0) {
        int entrada = open("/dev/null", O_RDONLY);
        if (entrada < 0 || dup2(entrada, STDIN_FILENO) == -1 ||
            dup2(fileno(app->registro), STDOUT_FILENO) == -1 ||
            dup2(fileno(app->registro), STDERR_FILENO) == -1) {
            _exit(126);
        }
        close(entrada);
        execv(argumentos[0], argumentos);
        _exit(127);
    }
    return app->pid > 0 ? 0 : -1;
}

static void terminar(struct aplicacion *app)
{
    if (app->pid > 0 && !app->recogida) {
        if (revisar(app) == 0) {
            kill(app->pid, SIGTERM);
            if (esperar_fin(app, 1) != 0) {
                kill(app->pid, SIGKILL);
                while (waitpid(app->pid, &app->estado, 0) == -1 && errno == EINTR) {
                }
                app->recogida = 1;
            }
        }
    }
    if (app->registro != NULL) {
        char *texto = registro(app);
        printf("--- %s pid=%ld estado=%d ---\n", app->rol, (long)app->pid, app->estado);
        if (texto != NULL) {
            fputs(texto, stdout);
        }
        free(texto);
        fclose(app->registro);
        app->registro = NULL;
    }
}

static int codigo(const struct aplicacion *app, int exito)
{
    return app->recogida && WIFEXITED(app->estado) &&
           (exito ? WEXITSTATUS(app->estado) == 0 : WEXITSTATUS(app->estado) != 0);
}

static int sesion(const char *nombre, size_t filas, size_t interior, size_t columnas,
                  unsigned copias, int sin_memoria)
{
    struct aplicacion apps[3] = {0};
    FILE *config = configurar(copias, sin_memoria ? 1 : 64, 64, 64);
    int resultado = 1;
    EXIGIR(config != NULL);
    char ruta[64] = "";
    snprintf(ruta, sizeof ruta, "/tmp/matrices-aceptacion-XXXXXX");
    int fd = mkstemp(ruta);
    EXIGIR(fd >= 0);
    FILE *copia = fdopen(fd, "w");
    EXIGIR(copia != NULL);
    rewind(config);
    char bloque[256];
    size_t nleidos;
    while ((nleidos = fread(bloque, 1, sizeof bloque, config)) > 0) {
        EXIGIR(fwrite(bloque, 1, nleidos, copia) == nleidos);
    }
    EXIGIR(fflush(copia) == 0);
    fclose(copia);
    char f[32], k[32], c[32];
    snprintf(f, sizeof f, "%zu", filas);
    snprintf(k, sizeof k, "%zu", interior);
    snprintf(c, sizeof c, "%zu", columnas);
    double inicio = ahora();
    for (unsigned i = 0; i < copias; i++) {
        char *args[] = {"./bin/apps/matrices", ruta, f, k, c, NULL};
        EXIGIR(iniciar(&apps[i], "matrices", args) == 0);
    }
    for (unsigned i = 0; i < copias; i++) {
        EXIGIR(esperar_fin(&apps[i], 40) == 0 && codigo(&apps[i], !sin_memoria));
    }
    int verifico = 0;
    for (unsigned i = 0; i < copias; i++) {
        verifico += contiene(&apps[i], "VERIFICACION OK");
        if (sin_memoria) EXIGIR(!contiene(&apps[i], "VERIFICACION OK"));
    }
    if (!sin_memoria) EXIGIR(verifico == 1);
    printf("APP MATRICES %s: %zux%zux%zu copias=%u segundos=%.3f\n",
           nombre, filas, interior, columnas, copias, ahora() - inicio);
    resultado = 0;
terminar:
    for (unsigned i = 0; i < copias; i++) terminar(&apps[i]);
    if (config != NULL) fclose(config);
    if (ruta[0] == '/') unlink(ruta);
    return resultado;
}

static int errores(void)
{
    struct aplicacion app = {0};
    FILE *config = configurar(2, 32, 32, 32);
    int resultado = 1;
    EXIGIR(config != NULL);
    char ruta[64];
    ruta_temporal(config, ruta);
    char *cero[] = {"./bin/apps/matrices", ruta, "0", "4", "4", NULL};
    char *overflow[] = {"./bin/apps/matrices", ruta, "18446744073709551615", "4", "4", NULL};
    char **casos[] = {cero, overflow};
    for (size_t i = 0; i < sizeof casos / sizeof casos[0]; i++) {
        app = (struct aplicacion){0};
        EXIGIR(iniciar(&app, "argumentos invalidos", casos[i]) == 0);
        EXIGIR(esperar_fin(&app, 4) == 0 && codigo(&app, 0));
        EXIGIR(!contiene(&app, "VERIFICACION OK"));
        terminar(&app);
    }
    puts("APP MATRICES errores: dimensiones invalidas rechazadas");
    resultado = 0;
terminar:
    terminar(&app);
    if (config != NULL) fclose(config);
    return resultado;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);
    if (sesion("pequena", 4, 4, 4, 1, 0) != 0 ||
        sesion("dos procesos", 8, 8, 8, 2, 0) != 0 ||
        sesion("sin capacidad", 64, 64, 64, 1, 1) != 0 ||
        errores() != 0) {
        return 1;
    }
    puts("Aplicacion de matrices: procesos bajo demanda verificados");
    return 0;
}
