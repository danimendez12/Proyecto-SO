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

static int esperar_marca(struct aplicacion *app, const char *marca, double segundos)
{
    double fin = ahora() + segundos;
    do {
        if (contiene(app, marca)) {
            return 0;
        }
        if (revisar(app) != 0) {
            return -1;
        }
        const struct timespec pausa = {.tv_nsec = 5000000};
        nanosleep(&pausa, NULL);
    } while (ahora() < fin);
    return -1;
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

static int info_matriz(const char *texto, char nombre, uint32_t *id, size_t *bytes, size_t *paginas)
{
    char marca[32];
    snprintf(marca, sizeof marca, "MATRIZ %c id=", nombre);
    const char *linea = strstr(texto, marca);
    if (linea == NULL || sscanf(linea + strlen(marca), "%" SCNu32, id) != 1) {
        return -1;
    }
    const char *fin = strchr(linea, '\n');
    const char *b = strstr(linea, " bytes=");
    const char *p = strstr(linea, " paginas=");
    return fin != NULL && b != NULL && p != NULL && b < fin && p < fin &&
           sscanf(b, " bytes=%zu", bytes) == 1 && sscanf(p, " paginas=%zu", paginas) == 1 ? 0 : -1;
}

static int ack_liberacion(const char *texto, unsigned hogar, uint32_t id)
{
    char marca[96];
    snprintf(marca, sizeof marca, "CAP_RELEASE_ACK hogar %u reserva %" PRIu32, hogar, id);
    const char *pos = texto;
    while ((pos = strstr(pos, marca)) != NULL) {
        const char *fin = strchr(pos, '\n');
        const char *resultado = strstr(pos, "resultado 0");
        if (fin != NULL && resultado != NULL && resultado < fin) {
            return 1;
        }
        pos += strlen(marca);
    }
    return 0;
}

static int muestra(const char *texto, size_t fila, size_t columna, double esperado)
{
    char marca[80];
    snprintf(marca, sizeof marca, "MUESTRA C[%zu,%zu]=", fila, columna);
    const char *pos = strstr(texto, marca);
    double valor;
    return pos != NULL && sscanf(pos + strlen(marca), "%lf", &valor) == 1 && valor == esperado;
}

static int sesion(const char *nombre, size_t filas, size_t interior, size_t columnas,
                  unsigned trabajadores, int multihogar, FILE *entrada_a, FILE *entrada_b,
                  int sin_memoria)
{
    struct aplicacion apps[3] = {0};
    FILE *config = configurar(trabajadores + 1, sin_memoria ? 1 : multihogar ? 8 : 4,
                             sin_memoria ? 1 : multihogar ? 6 : 4, 1);
    char *texto = NULL;
    int resultado = 1;
    double inicio = ahora();
    EXIGIR(config != NULL);
    char ruta[64], f[32], k[32], c[32], n[16], ra[64], rb[64];
    ruta_temporal(config, ruta);
    snprintf(f, sizeof f, "%zu", filas);
    snprintf(k, sizeof k, "%zu", interior);
    snprintf(c, sizeof c, "%zu", columnas);
    snprintf(n, sizeof n, "%u", trabajadores);
    if (entrada_a != NULL) {
        ruta_temporal(entrada_a, ra);
        ruta_temporal(entrada_b, rb);
    }
    char *creador[] = {"./bin/apps/matrices", "creador", ruta, f, k, c, n, "15", NULL};
    char *archivos[] = {"./bin/apps/matrices", "archivos", ruta, ra, rb, n, "15", NULL};
    EXIGIR(iniciar(&apps[0], "creador", entrada_a != NULL ? archivos : creador) == 0);
    EXIGIR(esperar_marca(&apps[0], "CONTROL id=", 6) == 0);
    texto = registro(&apps[0]);
    EXIGIR(texto != NULL);
    char *marca = strstr(texto, "CONTROL id=");
    uint32_t control;
    EXIGIR(marca != NULL && sscanf(marca, "CONTROL id=%" SCNu32, &control) == 1 && control != 0);
    free(texto);
    texto = NULL;
    char id[32];
    snprintf(id, sizeof id, "%" PRIu32, control);
    char nodos[2][16];
    for (unsigned i = 0; i < trabajadores; i++) {
        snprintf(nodos[i], sizeof nodos[i], "%u", i + 1);
        char *worker[] = {"./bin/apps/matrices", "trabajador", ruta, nodos[i], id, "2", "15", NULL};
        EXIGIR(iniciar(&apps[i + 1], "trabajador", worker) == 0);
        if (trabajadores == 2 && i == 0) {
            EXIGIR(esperar_marca(&apps[1], "ALTA nodo=1", 6) == 0);
            EXIGIR(!contiene(&apps[0], "VERIFICACION OK") && !contiene(&apps[0], "MATRIZ A id="));
        }
    }
    EXIGIR(esperar_fin(&apps[0], 23) == 0);
    for (unsigned i = 0; i < trabajadores; i++) {
        EXIGIR(esperar_fin(&apps[i + 1], 6) == 0 && codigo(&apps[i + 1], !sin_memoria));
    }
    texto = registro(&apps[0]);
    EXIGIR(texto != NULL && codigo(&apps[0], !sin_memoria));
    if (sin_memoria) {
        EXIGIR(strstr(texto, "DSM=-2") != NULL && strstr(texto, "VERIFICACION OK") == NULL);
        printf("APP MATRICES %s: falta de memoria devuelve error normal y termina todos los procesos\n", nombre);
    } else {
        EXIGIR(strstr(texto, "VERIFICACION OK") != NULL && strstr(texto, "LIBERACION OK") != NULL);
        uint32_t ids[3];
        const size_t elementos[] = {filas * interior, interior * columnas, filas * columnas};
        for (unsigned i = 0; i < 3; i++) {
            size_t bytes, paginas;
            EXIGIR(info_matriz(texto, (char)('A' + i), &ids[i], &bytes, &paginas) == 0 &&
                   bytes == elementos[i] * sizeof(double) && paginas == 1 + (bytes - 1) / 4096);
            EXIGIR(ids[i] != control);
            for (unsigned j = 0; j < i; j++) {
                EXIGIR(ids[i] != ids[j]);
            }
        }
        size_t fin_previo = 0;
        int varios_hilos = 0;
        for (unsigned i = 0; i < trabajadores; i++) {
            char ok[80];
            snprintf(ok, sizeof ok, "TRABAJADOR OK nodo=%u", i + 1);
            char *worker = registro(&apps[i + 1]);
            unsigned nodo;
            size_t hilos, inicio_bloque, fin_bloque;
            const char *calculo = worker != NULL ? strstr(worker, "CALCULO nodo=") : NULL;
            int reparto = calculo != NULL &&
                sscanf(calculo, "CALCULO nodo=%u hilos=%zu paginas=[%zu,%zu)",
                       &nodo, &hilos, &inicio_bloque, &fin_bloque) == 4 &&
                nodo == i + 1 && hilos >= 1 && hilos <= 2 &&
                inicio_bloque == fin_previo && fin_bloque > inicio_bloque &&
                hilos <= fin_bloque - inicio_bloque;
            if (reparto) {
                fin_previo = fin_bloque;
                varios_hilos |= hilos == 2;
            }
            free(worker);
            EXIGIR(reparto && contiene(&apps[i + 1], ok) &&
                   contiene(&apps[i + 1], "PAGE_REQUEST enviado hogar 0"));
        }
        EXIGIR(fin_previo == 1 + (elementos[2] * sizeof(double) - 1) / 4096);
        if (multihogar) {
            EXIGIR(varios_hilos);
            EXIGIR(strstr(texto, "TRAMO C hogar=0") != NULL && strstr(texto, "TRAMO C hogar=1") != NULL);
            EXIGIR(ack_liberacion(texto, 1, ids[2]));
        }
        if (entrada_a != NULL) {
            EXIGIR(muestra(texto, 0, 0, -1.5) && muestra(texto, 1, 0, 9) && muestra(texto, 1, 1, -1));
        }
        printf("APP MATRICES %s: %zux%zux%zu, trabajadores=%u hilos_solicitados_por_nodo=2, "
               "verificacion y liberacion confirmadas, segundos=%.6f\n",
               nombre, filas, interior, columnas, trabajadores, ahora() - inicio);
    }
    resultado = 0;
terminar:
    free(texto);
    for (int i = 2; i >= 0; i--) {
        terminar(&apps[i]);
    }
    if (config != NULL) {
        fclose(config);
    }
    return resultado;
}

static int referencia_original(FILE *a, FILE *b)
{
    struct aplicacion app = {0};
    char *texto = NULL;
    int resultado = 1;
    char ra[64], rb[64];
    ruta_temporal(a, ra);
    ruta_temporal(b, rb);
    char *args[] = {"./bin/apps/matrices_secuencial", ra, rb, NULL};
    EXIGIR(iniciar(&app, "referencia original MIT", args) == 0);
    EXIGIR(esperar_fin(&app, 3) == 0 && codigo(&app, 1));
    texto = registro(&app);
    double c00, c01, c10, c11;
    EXIGIR(texto != NULL && sscanf(texto, "%lf %lf %lf %lf", &c00, &c01, &c10, &c11) == 4 &&
           c00 == -1.5 && c01 == 17.5 && c10 == 9 && c11 == -1);
    puts("APP MATRICES referencia: secuencial original calcula los cuatro resultados conocidos del TSV");
    resultado = 0;
terminar:
    free(texto);
    terminar(&app);
    return resultado;
}

static int fallo_trabajador(void)
{
    struct aplicacion creador = {0}, worker = {0};
    FILE *config = configurar(3, 8, 6, 1);
    char *texto = NULL;
    int resultado = 1;
    EXIGIR(config != NULL);
    char ruta[64];
    ruta_temporal(config, ruta);
    char *args[] = {"./bin/apps/matrices", "creador", ruta, "33", "37", "41", "2", "15", NULL};
    EXIGIR(iniciar(&creador, "creador ante error de trabajador", args) == 0);
    EXIGIR(esperar_marca(&creador, "CONTROL id=", 3) == 0);
    texto = registro(&creador);
    EXIGIR(texto != NULL);
    const char *pos = strstr(texto, "CONTROL id=");
    uint32_t control;
    EXIGIR(pos != NULL && sscanf(pos, "CONTROL id=%" SCNu32, &control) == 1 && control != 0);
    char id[32];
    snprintf(id, sizeof id, "%" PRIu32, control);
    char *args_worker[] = {"./bin/apps/matrices", "trabajador", ruta, "1", id, "2", "1", NULL};
    EXIGIR(iniciar(&worker, "trabajador que vence antes de DATOS", args_worker) == 0);
    EXIGIR(esperar_marca(&worker, "ALTA nodo=1", 3) == 0);
    /* El nodo 2 no se inicia: el nodo 1 anuncia su error por el control DSM. */
    EXIGIR(esperar_fin(&worker, 5) == 0 && codigo(&worker, 0));
    EXIGIR(esperar_fin(&creador, 5) == 0 && codigo(&creador, 0));
    EXIGIR(contiene(&worker, "DSM=-4") && contiene(&creador, "ERROR creador DSM=-4"));
    EXIGIR(!contiene(&creador, "MATRIZ A id=") && !contiene(&creador, "VERIFICACION OK"));
    puts("APP MATRICES error de trabajador: plazo comunicado por DSM aborta al creador sin verificar C incompleta");
    resultado = 0;
terminar:
    free(texto);
    terminar(&worker);
    terminar(&creador);
    if (config != NULL) fclose(config);
    return resultado;
}

static int errores(FILE *entrada_a, FILE *incompatible)
{
    struct aplicacion app = {0};
    FILE *config = configurar(2, 4, 4, 0);
    int resultado = 1;
    EXIGIR(config != NULL);
    char ruta[64], ra[64], rb[64];
    ruta_temporal(config, ruta);
    ruta_temporal(entrada_a, ra);
    ruta_temporal(incompatible, rb);
    char *cero[] = {"./bin/apps/matrices", "creador", ruta, "0", "4", "4", "1", "1", NULL};
    char *overflow[] = {"./bin/apps/matrices", "creador", ruta, "18446744073709551615", "4", "4", "1", "1", NULL};
    char *hilos[] = {"./bin/apps/matrices", "trabajador", ruta, "1", "1", "0", "1", NULL};
    char *archivos[] = {"./bin/apps/matrices", "archivos", ruta, ra, rb, "1", "1", NULL};
    char **casos[] = {cero, overflow, hilos, archivos};
    for (size_t i = 0; i < sizeof casos / sizeof casos[0]; i++) {
        app = (struct aplicacion){0};
        EXIGIR(iniciar(&app, "argumentos invalidos", casos[i]) == 0);
        EXIGIR(esperar_fin(&app, 3) == 0 && codigo(&app, 0));
        EXIGIR(!contiene(&app, "VERIFICACION OK"));
        terminar(&app);
    }
    app = (struct aplicacion){0};
    char *plazo[] = {"./bin/apps/matrices", "creador", ruta, "4", "4", "4", "1", "1", NULL};
    EXIGIR(iniciar(&app, "trabajador ausente", plazo) == 0);
    EXIGIR(esperar_marca(&app, "CONTROL id=", 3) == 0);
    EXIGIR(esperar_fin(&app, 5) == 0 && codigo(&app, 0));
    EXIGIR(contiene(&app, "ERROR creador DSM=-4") && !contiene(&app, "VERIFICACION OK"));
    puts("APP MATRICES errores: dimensiones, overflow, hilos, TSV incompatible y plazo sin trabajador rechazados");
    resultado = 0;
terminar:
    terminar(&app);
    if (config != NULL) {
        fclose(config);
    }
    return resultado;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);
    FILE *a = temporal(), *b = temporal(), *incompatible = temporal();
    int resultado = 1;
    if (a == NULL || b == NULL || incompatible == NULL) {
        goto terminar;
    }
    if (fputs("1.5\t-2\t3\n4\t0.5\t-1\n", a) == EOF ||
        fputs("2\t1\n3\t-2\n0.5\t4\n", b) == EOF ||
        fputs("1\t2\n3\t4\n", incompatible) == EOF ||
        fflush(a) != 0 || fflush(b) != 0 || fflush(incompatible) != 0) {
        goto terminar;
    }
    if (referencia_original(a, b) != 0 ||
        sesion("pequena", 4, 4, 4, 1, 0, NULL, NULL, 0) != 0 ||
        sesion("multipagina multihogar", 33, 37, 41, 2, 1, NULL, NULL, 0) != 0 ||
        sesion("TSV conocido", 2, 3, 2, 1, 0, a, b, 0) != 0 ||
        sesion("sin capacidad", 33, 37, 41, 2, 1, NULL, NULL, 1) != 0 ||
        errores(a, incompatible) != 0 || fallo_trabajador() != 0) {
        goto terminar;
    }
    puts("Aplicacion de matrices: procesos TCP reales, Pthreads, matematicas, capacidad y errores verificados");
    resultado = 0;
terminar:
    if (a != NULL) fclose(a);
    if (b != NULL) fclose(b);
    if (incompatible != NULL) fclose(incompatible);
    return resultado;
}
