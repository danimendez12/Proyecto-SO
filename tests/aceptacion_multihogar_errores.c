#include "api_prueba.h"
#include "protocol.h"

#include <stdatomic.h>
#include <sys/socket.h>

#define EXIGIR(condicion) do { \
    if (!(condicion)) { \
        fprintf(stderr, "aceptacion_multihogar_errores:%d: %s\n", __LINE__, #condicion); \
        goto terminar; \
    } \
} while (0)

_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "La inyeccion requiere un indicador lock-free");
static atomic_int perder_ack;

static void armar_perdida(int signo)
{
    (void)signo;
    atomic_store_explicit(&perder_ack, 1, memory_order_relaxed);
}

ssize_t __real_send(int fd, const void *buf, size_t bytes, int flags);

/* Se pierde una respuesta real del hogar. No se fabrica ninguna concesion. */
ssize_t __wrap_send(int fd, const void *buf, size_t bytes, int flags)
{
    const unsigned char *trama = buf;
    if (bytes == DSM_CAPACIDAD_BASE_BYTES && trama[5] == DSM_TIPO_CAP_RELEASE_ACK &&
        atomic_load_explicit(&perder_ack, memory_order_relaxed)) {
        dsm_cabecera_t cab;
        if (dsm_cabecera_decodificar(trama, bytes, &cab) == 0 && cab.origen == 1 &&
            cab.destino == 0 && cab.estado == DSM_ESTADO_OK &&
            atomic_exchange_explicit(&perder_ack, 0, memory_order_relaxed)) {
            printf("prueba descartar CAP_RELEASE_ACK origen 1 destino 0 corr %" PRIu32 "\n",
                   cab.id_corr);
            return (ssize_t)bytes;
        }
    }
    return __real_send(fd, buf, bytes, flags);
}

static int esperar_red(struct api_proceso *nodo, unsigned propio)
{
    for (unsigned par = 0; par < 3; par++) {
        if (par == propio) {
            continue;
        }
        char frase[80];
        snprintf(frase, sizeof frase, "par %u hello 1 welcome 1 estado 2", par);
        unsigned i;
        for (i = 0; i < 100 && !api_tiene_traza(nodo, frase); i++) {
            const struct timespec pausa = {.tv_nsec = 50000000};
            nanosleep(&pausa, NULL);
        }
        if (i == 100) {
            return -1;
        }
    }
    return 0;
}

struct asignacion {
    struct api_proceso *proceso;
    pthread_barrier_t *inicio;
    size_t bytes;
    struct api_respuesta respuesta;
    int transporte;
};

static void *reservar(void *arg)
{
    struct asignacion *tarea = arg;
    struct api_orden orden = {.operacion = API_RESERVAR, .bytes = tarea->bytes};
    pthread_barrier_wait(tarea->inicio);
    tarea->transporte = api_ejecutar(tarea->proceso, &orden, &tarea->respuesta);
    return NULL;
}

static int simultaneas(struct api_proceso *nodos, size_t bytes, struct asignacion tareas[2])
{
    pthread_barrier_t inicio;
    if (pthread_barrier_init(&inicio, NULL, 2) != 0) {
        return -1;
    }
    for (size_t i = 0; i < 2; i++) {
        tareas[i] = (struct asignacion){.proceso = &nodos[i], .inicio = &inicio, .bytes = bytes};
    }
    pthread_t hilo;
    if (pthread_create(&hilo, NULL, reservar, &tareas[0]) != 0) {
        pthread_barrier_destroy(&inicio);
        return -1;
    }
    reservar(&tareas[1]);
    int unido = pthread_join(hilo, NULL);
    pthread_barrier_destroy(&inicio);
    return unido == 0 && tareas[0].transporte == 0 && tareas[1].transporte == 0 ? 0 : -1;
}

static int verificar_mapa(struct api_proceso *nodo, size_t paginas, uint64_t *gpns)
{
    for (size_t i = 0; i < paginas; i++) {
        struct api_orden orden = {.operacion = API_UBICACION, .offset = i};
        struct api_respuesta r;
        if (api_ejecutar(nodo, &orden, &r) != 0 || r.resultado != DSM_OK || r.hogar > 1 ||
            (r.hogar == 0 ? r.gpn >= 2 : r.gpn < 2 || r.gpn >= 4)) {
            return -1;
        }
        gpns[i] = r.gpn;
        for (size_t j = 0; j < i; j++) {
            if (gpns[i] == gpns[j]) {
                return -1;
            }
        }
    }
    return 0;
}

static int probar(void)
{
    enum { PAGINA = 4096 };
    FILE *config = tmpfile();
    if (config == NULL) {
        return 1;
    }
    int escrito = fprintf(config,
        "page_size=4096\nmemoria_inicial=8192\nmax_nodos=3\nmax_hilos=8\n"
        "rpc_timeout_ms=500\nheartbeat_intervalo_ms=1000\nheartbeat_fallos=10\n"
        "puerto_rango_inicio=27200\npuerto_rango_fin=27299\n"
        "nodo=0 hogar0 127.0.0.1 27200 8192\n"
        "nodo=1 hogar1 127.0.0.1 27201 8192\n"
        "nodo=2 cliente 127.0.0.1 27202 4096\n");
    if (escrito < 0 || fflush(config) != 0) {
        fclose(config);
        return 1;
    }
    char ruta[64];
    snprintf(ruta, sizeof ruta, "/proc/self/fd/%d", fileno(config));
    struct api_proceso nodos[3] = {0};
    struct api_orden orden;
    struct api_respuesta r;
    uint32_t ocupada = 0, reserva = 0;
    uint64_t mapa[4];
    int detenido = 0, resultado = 1;
    for (unsigned i = 0; i < 3; i++) {
        EXIGIR(api_iniciar_proceso(&nodos[i], ruta, (uint16_t)i) == 0);
        EXIGIR(api_operar(&nodos[i], API_INICIAR, DSM_OK) == 0);
    }
    for (unsigned i = 0; i < 3; i++) {
        EXIGIR(esperar_red(&nodos[i], i) == 0);
    }
    orden = (struct api_orden){.operacion = API_RESERVAR, .bytes = PAGINA};
    EXIGIR(api_ejecutar(&nodos[2], &orden, &r) == 0 && r.resultado == DSM_OK);
    ocupada = r.id;
    /* Cabe nominalmente (5P), pero solo quedan 4P. Obliga a revertir aportes reales. */
    orden.bytes = 5 * PAGINA;
    EXIGIR(api_ejecutar(&nodos[0], &orden, &r) == 0 && r.resultado == DSM_ERR_NOMEM);
    EXIGIR(api_tiene_traza(&nodos[0], "CAP_CANCEL"));
    orden.bytes = 4 * PAGINA;
    EXIGIR(api_ejecutar(&nodos[0], &orden, &r) == 0 && r.resultado == DSM_OK);
    reserva = r.id;
    EXIGIR(verificar_mapa(&nodos[0], 4, mapa) == 0);
    orden = (struct api_orden){.operacion = API_LIBERAR, .id = reserva};
    EXIGIR(api_ejecutar(&nodos[0], &orden, &r) == 0 && r.resultado == DSM_OK);
    puts("Multihogar errores: insuficiencia real revierte y recupera las 4 paginas disponibles");

    EXIGIR(kill(nodos[1].pid, SIGSTOP) == 0);
    detenido = 1;
    int estado;
    EXIGIR(waitpid(nodos[1].pid, &estado, WUNTRACED) == nodos[1].pid && WIFSTOPPED(estado));
    orden = (struct api_orden){.operacion = API_RESERVAR, .bytes = 3 * PAGINA};
    EXIGIR(api_ejecutar(&nodos[0], &orden, &r) == 0 && r.resultado == DSM_ERR_TIEMPO);
    orden.bytes = 4 * PAGINA;
    EXIGIR(api_ejecutar(&nodos[0], &orden, &r) == 0 && r.resultado == DSM_ERR_TIEMPO);
    EXIGIR(kill(nodos[1].pid, SIGCONT) == 0);
    detenido = 0;
    EXIGIR(api_ejecutar(&nodos[0], &orden, &r) == 0 && r.resultado == DSM_OK);
    reserva = r.id;
    EXIGIR(verificar_mapa(&nodos[0], 4, mapa) == 0);
    puts("Multihogar errores: timeout de PREPARE conserva operacion y CANCEL recupera capacidad");
    orden = (struct api_orden){.operacion = API_ABRIR, .id = reserva};
    EXIGIR(api_ejecutar(&nodos[2], &orden, &r) == 0 && r.resultado == DSM_OK);
    orden = (struct api_orden){.operacion = API_LIBERAR, .id = reserva};
    EXIGIR(api_ejecutar(&nodos[0], &orden, &r) == 0 && r.resultado == DSM_ERR_OCUPADA);
    EXIGIR(api_operar(&nodos[1], API_CERRAR, DSM_ERR_OCUPADA) == 0);
    orden = (struct api_orden){.operacion = API_DESVINCULAR, .id = reserva};
    EXIGIR(api_ejecutar(&nodos[2], &orden, &r) == 0 && r.resultado == DSM_OK);
    EXIGIR(kill(nodos[1].pid, SIGUSR2) == 0);
    EXIGIR(api_operar(&nodos[1], API_CERRAR, DSM_ERR_OCUPADA) == 0);
    orden = (struct api_orden){.operacion = API_LIBERAR, .id = reserva};
    EXIGIR(api_ejecutar(&nodos[0], &orden, &r) == 0 && r.resultado == DSM_ERR_TIEMPO);
    EXIGIR(api_contar_traza(&nodos[1], "prueba descartar CAP_RELEASE_ACK") == 1);
    EXIGIR(api_ejecutar(&nodos[0], &orden, &r) == 0 && r.resultado == DSM_OK);
    puts("Multihogar errores: ACK RELEASE perdido y segundo liberar idempotente");

    struct asignacion tareas[2];
    EXIGIR(simultaneas(nodos, 3 * PAGINA, tareas) == 0);
    unsigned exitos = 0;
    for (unsigned i = 0; i < 2; i++) {
        int rc = tareas[i].respuesta.resultado;
        EXIGIR(rc == DSM_OK || rc == DSM_ERR_NOMEM);
        if (rc == DSM_OK) {
            exitos++;
            EXIGIR(verificar_mapa(&nodos[i], 3, mapa) == 0);
        }
    }
    EXIGIR(exitos <= 1);
    for (unsigned i = 0; i < 2; i++) {
        if (tareas[i].respuesta.resultado == DSM_OK) {
            orden = (struct api_orden){.operacion = API_LIBERAR, .id = tareas[i].respuesta.id};
            EXIGIR(api_ejecutar(&nodos[i], &orden, &r) == 0 && r.resultado == DSM_OK);
        }
    }
    EXIGIR(simultaneas(nodos, 2 * PAGINA, tareas) == 0);
    uint64_t coexistentes[2][2];
    for (unsigned i = 0; i < 2; i++) {
        EXIGIR(tareas[i].respuesta.resultado == DSM_OK);
        EXIGIR(verificar_mapa(&nodos[i], 2, coexistentes[i]) == 0);
    }
    EXIGIR(tareas[0].respuesta.id != tareas[1].respuesta.id);
    for (unsigned i = 0; i < 2; i++) {
        for (unsigned j = 0; j < 2; j++) {
            EXIGIR(coexistentes[0][i] != coexistentes[1][j]);
        }
    }
    for (unsigned i = 0; i < 2; i++) {
        orden = (struct api_orden){.operacion = API_LIBERAR, .id = tareas[i].respuesta.id};
        EXIGIR(api_ejecutar(&nodos[i], &orden, &r) == 0 && r.resultado == DSM_OK);
    }
    orden = (struct api_orden){.operacion = API_RESERVAR, .bytes = 4 * PAGINA};
    EXIGIR(api_ejecutar(&nodos[0], &orden, &r) == 0 && r.resultado == DSM_OK);
    EXIGIR(verificar_mapa(&nodos[0], 4, mapa) == 0);
    orden = (struct api_orden){.operacion = API_LIBERAR, .id = r.id};
    EXIGIR(api_ejecutar(&nodos[0], &orden, &r) == 0 && r.resultado == DSM_OK);
    orden.id = ocupada;
    EXIGIR(api_ejecutar(&nodos[2], &orden, &r) == 0 && r.resultado == DSM_OK);
    puts("Multihogar errores: concurrencia sin sobreasignar, dos mapas sin alias y reutilizacion completa");
    resultado = 0;
terminar:
    if (detenido) {
        kill(nodos[1].pid, SIGCONT);
    }
    for (int i = 2; i >= 0; i--) {
        if (api_detener_proceso(&nodos[i]) != 0) {
            resultado = 1;
        }
    }
    fclose(config);
    return resultado;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);
    struct sigaction accion = {.sa_handler = armar_perdida};
    sigemptyset(&accion.sa_mask);
    if (sigaction(SIGUSR2, &accion, NULL) != 0) {
        return 1;
    }
    alarm(55);
    if (probar() != 0) {
        return 1;
    }
    puts("Multihogar errores reales verificados en tres procesos TCP");
    return 0;
}
