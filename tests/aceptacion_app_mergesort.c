#include "../apps/mergesort/control.h"
#include "../apps/mergesort/datos.h"
#include "../apps/mergesort/orden.h"

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

/* Coordina ejecutables reales y comprueba los modulos mediante el API DSM. */
#define EXIGIR(condicion) do { \
    if (!(condicion)) { \
        fprintf(stderr, "aceptacion_app_mergesort:%d: %s\n", __LINE__, #condicion); \
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
        rc = fprintf(config, "nodo=%u orden-%u 127.0.0.1 %u %zu\n",
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

static int comparar(const void *a, const void *b)
{
    int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;
    return (x > y) - (x < y);
}

static int verificar_modulos(void)
{
    FILE *config = configurar(3, 16, 1, 1), *original = tmpfile();
    dsm_context_t *ctx = NULL;
    dsm_reserva_id_t control = 0, a = 0, b = 0, resultado_id = 0, capacidad = 0;
    int resultado = 1;
    EXIGIR(config != NULL && original != NULL);
    int32_t entrada[] = {7, 12, 19, 3, 18, 4, 2, -5, 6, 15, 8, INT32_MIN, INT32_MAX, 0, 0};
    int32_t referencia[sizeof entrada / sizeof entrada[0]], auxiliar[sizeof entrada / sizeof entrada[0]];
    memcpy(referencia, entrada, sizeof entrada);
    qsort(referencia, sizeof entrada / sizeof entrada[0], sizeof entrada[0], comparar);
    orden_secuencial(entrada, auxiliar, sizeof entrada / sizeof entrada[0]);
    EXIGIR(memcmp(entrada, referencia, sizeof entrada) == 0);
    char ruta[64];
    ruta_temporal(config, ruta);
    EXIGIR(dsm_iniciar(ruta, 0, &ctx) == DSM_OK);
    dsm_proc_id_t proceso;
    EXIGIR(dsm_proc_abrir(ctx, &proceso) == DSM_OK);
    mergesort_control_datos_t datos = {.elementos = 2049, .bloque = 1024,
        .trabajadores = 1, .hilos = 2, .page_size = 4096};
    EXIGIR(mergesort_control_crear(ctx, proceso, &datos, &control) == DSM_OK);
    EXIGIR(mergesort_control_marcar(ctx, control, 0, 0, DSM_OK) == DSM_OK);
    void *base;
    EXIGIR(dsm_reservar(ctx, proceso, 2049 * sizeof(int32_t), &a, &base) == DSM_OK);
    EXIGIR(dsm_reservar(ctx, proceso, 2049 * sizeof(int32_t), &b, &base) == DSM_OK);
    datos.a = a;
    datos.b = b;
    EXIGIR(mergesort_control_publicar_datos(ctx, control, &datos) == DSM_OK);
    mergesort_control_fase_t fase = {.generacion = 1, .ancho = 1024,
        .tipo = MERGESORT_CONTROL_ORDENAR, .origen = 0};
    EXIGIR(mergesort_control_publicar_fase(ctx, control, &fase) == DSM_OK);
    fase = (mergesort_control_fase_t){.generacion = 2, .ancho = 1024,
        .tipo = MERGESORT_CONTROL_FUSIONAR, .origen = 0};
    EXIGIR(mergesort_control_publicar_fase(ctx, control, &fase) == DSM_ERR_OCUPADA);
    fase.generacion = 3;
    EXIGIR(mergesort_control_publicar_fase(ctx, control, &fase) == DSM_ERR_PROTO);
    EXIGIR(mergesort_control_marcar(ctx, control, 0, 2, DSM_OK) == DSM_ERR_PROTO);
    struct timespec plazo;
    EXIGIR(mergesort_control_plazo(2, &plazo) == DSM_OK);
    mergesort_control_fase_t actual;
    EXIGIR(mergesort_control_esperar_fase(ctx, control, 0, &actual, &plazo) == DSM_OK &&
           actual.generacion == 1 && actual.tipo == MERGESORT_CONTROL_ORDENAR);
    EXIGIR(mergesort_control_marcar(ctx, control, 0, 1, DSM_OK) == DSM_OK);
    EXIGIR(mergesort_control_marcar(ctx, control, 0, 1, DSM_OK) == DSM_OK);
    EXIGIR(mergesort_control_marcar(ctx, control, 0, 0, DSM_OK) == DSM_ERR_PROTO);
    EXIGIR(mergesort_control_marcar(ctx, control, 0, 1, DSM_ERR_TIEMPO) == DSM_ERR_ESTADO);
    fase = (mergesort_control_fase_t){.generacion = 2, .tipo = MERGESORT_CONTROL_CERRAR};
    EXIGIR(mergesort_control_publicar_fase(ctx, control, &fase) == DSM_ERR_PROTO);
    fase = (mergesort_control_fase_t){.generacion = 2, .ancho = 1024,
        .tipo = MERGESORT_CONTROL_FUSIONAR, .origen = 0};
    EXIGIR(mergesort_control_publicar_fase(ctx, control, &fase) == DSM_OK);
    EXIGIR(mergesort_control_marcar(ctx, control, 0, 2, DSM_OK) == DSM_OK);
    fase = (mergesort_control_fase_t){.generacion = 3, .ancho = 2048,
        .tipo = MERGESORT_CONTROL_FUSIONAR, .origen = 1};
    EXIGIR(mergesort_control_publicar_fase(ctx, control, &fase) == DSM_OK);
    EXIGIR(mergesort_control_marcar(ctx, control, 0, 3, DSM_OK) == DSM_OK);
    fase = (mergesort_control_fase_t){.generacion = 4, .ancho = 4096,
        .tipo = MERGESORT_CONTROL_FUSIONAR, .origen = 0};
    EXIGIR(mergesort_control_publicar_fase(ctx, control, &fase) == DSM_ERR_PROTO);
    fase = (mergesort_control_fase_t){.generacion = 4, .tipo = MERGESORT_CONTROL_CERRAR};
    EXIGIR(mergesort_control_publicar_fase(ctx, control, &fase) == DSM_OK);
    EXIGIR(dsm_liberar(ctx, b) == DSM_OK); b = 0;
    EXIGIR(dsm_liberar(ctx, a) == DSM_OK); a = 0;
    EXIGIR(mergesort_control_marcar(ctx, control, 0, 4, DSM_OK) == DSM_OK);
    EXIGIR(dsm_liberar(ctx, control) == DSM_OK); control = 0;
    puts("APP MERGESORT barrera: fase adelantada, saltos, ACK viejo/futuro y cierre prematuro rechazados sin avanzar");

    int32_t esperado[] = {INT32_MIN, 0, 0, INT32_MAX};
    int32_t corrupto[] = {INT32_MIN, 0, INT32_MAX, INT32_MAX};
    EXIGIR(fwrite(esperado, sizeof esperado, 1, original) == 1);
    EXIGIR(dsm_reservar(ctx, proceso, sizeof esperado, &resultado_id, &base) == DSM_OK);
    EXIGIR(dsm_escribir(ctx, resultado_id, 0, corrupto, sizeof corrupto) == DSM_OK);
    for (size_t i = 1; i < 4; i++) EXIGIR(corrupto[i - 1] <= corrupto[i]);
    EXIGIR(datos_verificar(ctx, resultado_id, 4, original) != DSM_OK);
    EXIGIR(dsm_escribir(ctx, resultado_id, 0, esperado, sizeof esperado) == DSM_OK);
    EXIGIR(datos_verificar(ctx, resultado_id, 4, original) == DSM_OK);
    EXIGIR(dsm_liberar(ctx, resultado_id) == DSM_OK); resultado_id = 0;
    puts("APP MERGESORT verificador: resultado ordenado con duplicado incorrecto detectado; restaurado coincide completo");
    EXIGIR(dsm_reservar(ctx, proceso, 16 * 4096, &capacidad, &base) == DSM_OK);
    for (size_t i = 0; i < 16; i++) {
        dsm_node_id_t hogar;
        uint64_t gpn;
        EXIGIR(dsm_ubicacion_pagina(ctx, capacidad, i, &hogar, &gpn) == DSM_OK && hogar == 0);
    }
    EXIGIR(dsm_liberar(ctx, capacidad) == DSM_OK); capacidad = 0;
    puts("APP MERGESORT capacidad: mismo contexto vuelve a reservar las 16 paginas locales completas");
    resultado = 0;
terminar:
    if (ctx != NULL) {
        const dsm_reserva_id_t ids[] = {capacidad, resultado_id, b, a, control};
        for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) if (ids[i]) dsm_liberar(ctx, ids[i]);
        if (dsm_cerrar(ctx) != DSM_OK) resultado = 1;
    }
    if (config != NULL) fclose(config);
    if (original != NULL) fclose(original);
    return resultado;
}

static int integrar(const char *n, const char *patron, const char *semilla, unsigned copias)
{
    struct aplicacion apps[2] = {0};
    FILE *config = configurar(copias, copias > 1 ? 80 : 32, 80, 1);
    int resultado = 1;
    EXIGIR(config != NULL && copias >= 1 && copias <= 2);
    char ruta[64], marca[160];
    ruta_temporal(config, ruta);
    double inicio = ahora();
    for (unsigned i = 0; i < copias; i++) {
        char *args[] = {"./bin/apps/mergesort", ruta, (char *)n, (char *)patron, (char *)semilla, NULL};
        EXIGIR(iniciar(&apps[i], patron, args) == 0);
    }
    for (unsigned i = 0; i < copias; i++) {
        EXIGIR(esperar_fin(&apps[i], 60) == 0 && codigo(&apps[i], 1));
    }
    snprintf(marca, sizeof marca, "VERIFICACION OK elementos=%s comparados=%s ordenado=si coincide=si", n, n);
    int verifico = 0;
    for (unsigned i = 0; i < copias; i++) verifico += contiene(&apps[i], marca);
    EXIGIR(verifico == 1);
    if (copias > 1 && strtoull(n, NULL, 10) > 10000) {
        int hogares = 0;
        for (unsigned i = 0; i < copias; i++) hogares += contiene(&apps[i], "hogar=1");
        EXIGIR(hogares >= 1);
    }
    printf("APP MERGESORT demanda: n=%s patron=%s copias=%u segundos=%.3f\n",
           n, patron, copias, ahora() - inicio);
    resultado = 0;
terminar:
    for (unsigned i = 0; i < copias; i++) terminar(&apps[i]);
    if (config != NULL) fclose(config);
    return resultado;
}

static int errores(FILE *entrada)
{
    struct aplicacion app = {0};
    FILE *config = configurar(3, 3, 128, 1);
    int resultado = 1;
    EXIGIR(config != NULL);
    char ruta[64], archivo[64];
    ruta_temporal(config, ruta);
    ruta_temporal(entrada, archivo);
    char *cero[] = {"./bin/apps/mergesort", ruta, "0", "aleatorio", "1", NULL};
    char *overflow[] = {"./bin/apps/mergesort", ruta, "18446744073709551615", "aleatorio", "1", NULL};
    char *patron[] = {"./bin/apps/mergesort", "secuencial", "16", "inexistente", "1", NULL};
    char *semilla[] = {"./bin/apps/mergesort", "secuencial", "16", "aleatorio", "1x", NULL};
    char *faltantes[] = {"./bin/apps/mergesort", "secuencial", "10", "archivo", "1", archivo, NULL};
    char **casos[] = {cero, overflow, patron, semilla, faltantes};
    for (size_t i = 0; i < sizeof casos / sizeof casos[0]; i++) {
        app = (struct aplicacion){0};
        EXIGIR(iniciar(&app, "entrada o configuracion invalida", casos[i]) == 0);
        EXIGIR(esperar_fin(&app, 4) == 0 && codigo(&app, 0));
        EXIGIR(!contiene(&app, "VERIFICACION OK"));
        terminar(&app);
    }
    puts("APP MERGESORT errores: tamanos, semilla, patron y entrada rechazados");
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
    FILE *entrada = temporal();
    if (entrada == NULL) return 1;
    int resultado = 1;
    if (fputs("3\t-1\n3 0 -2147483648 2147483647 0 -1 5\n", entrada) == EOF || fflush(entrada) != 0) goto terminar;
    char archivo[64];
    ruta_temporal(entrada, archivo);
    if (verificar_modulos() != 0 ||
        integrar("16", "aleatorio", "7", 1) != 0 ||
        integrar("1001", "inverso", "7", 1) != 0 ||
        integrar("1001", "repetidos", "7", 2) != 0 ||
        integrar("65539", "aleatorio", "12345", 2) != 0 ||
        errores(entrada) != 0) goto terminar;
    puts("Merge Sort: algoritmo, barreras, comparacion exhaustiva, TCP real, multihogar y errores verificados");
    resultado = 0;
terminar:
    fclose(entrada);
    return resultado;
}

