#include "api_prueba.h"

#define EXIGIR(condicion) do { \
    if (!(condicion)) { \
        fprintf(stderr, "aceptacion_api_errores:%d: %s\n", __LINE__, #condicion); \
        goto terminar; \
    } \
} while (0)

static int probar(void)
{
    FILE *config = tmpfile();
    if (config == NULL) {
        return 1;
    }
    int n = fprintf(config,
        "page_size = 4096\nmemoria_inicial = 1048576\nmax_nodos = 2\n"
        "rpc_timeout_ms = 400\nheartbeat_intervalo_ms = 1000\nheartbeat_fallos = 5\n"
        "puerto_rango_inicio = 27200\npuerto_rango_fin = 27299\n"
        "nodo = 0 cliente 127.0.0.1 27200\nnodo = 1 creador 127.0.0.1 27201\n");
    if (n < 0 || fflush(config) != 0) {
        fclose(config);
        return 1;
    }
    char ruta[64];
    snprintf(ruta, sizeof ruta, "/proc/self/fd/%d", fileno(config));
    struct api_proceso cliente = {0}, creador = {0};
    struct api_orden orden;
    struct api_respuesta respuesta;
    int detenido = 0, resultado = 1, cierre_cliente, cierre_creador;
    dsm_reserva_id_t reserva = (UINT32_C(1) << 16) | 1;
    EXIGIR(api_iniciar_proceso(&cliente, ruta, 0) == 0);
    EXIGIR(api_operar(&cliente, API_INICIAR, DSM_OK) == 0);
    orden = (struct api_orden){.operacion = API_ABRIR, .id = reserva};
    EXIGIR(api_ejecutar(&cliente, &orden, &respuesta) == 0 &&
           respuesta.resultado == DSM_ERR_NODO);
    puts("API error: nodo configurado desconectado -> DSM_ERR_NODO");

    EXIGIR(api_iniciar_proceso(&creador, ruta, 1) == 0);
    EXIGIR(api_operar(&creador, API_INICIAR, DSM_OK) == 0);
    orden = (struct api_orden){.operacion = API_RESERVAR, .bytes = 64};
    EXIGIR(api_ejecutar(&creador, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK &&
           respuesta.id == reserva);
    for (unsigned i = 0; i < 100; i++) {
        if (api_tiene_traza(&cliente, "par 1 hello 1 welcome 1 estado 2")) {
            break;
        }
        const struct timespec pausa = {.tv_nsec = 50000000};
        nanosleep(&pausa, NULL);
    }
    EXIGIR(api_tiene_traza(&cliente, "par 1 hello 1 welcome 1 estado 2"));
    orden = (struct api_orden){.operacion = API_ABRIR,
                              .id = (UINT32_C(1) << 16) | UINT32_C(65535)};
    EXIGIR(api_ejecutar(&cliente, &orden, &respuesta) == 0 &&
           respuesta.resultado == DSM_ERR_INVAL);
    EXIGIR(api_tiene_traza(&cliente, "par 1 hello 1 welcome 1 estado 2"));
    puts("API error: reserva inexistente con creador conectado -> DSM_ERR_INVAL");
    EXIGIR(kill(creador.pid, SIGSTOP) == 0);
    detenido = 1;
    int estado_stop;
    EXIGIR(waitpid(creador.pid, &estado_stop, WUNTRACED) == creador.pid &&
           WIFSTOPPED(estado_stop));
    orden = (struct api_orden){.operacion = API_ABRIR, .id = reserva};
    EXIGIR(api_ejecutar(&cliente, &orden, &respuesta) == 0 &&
           respuesta.resultado == DSM_ERR_TIEMPO);
    EXIGIR(kill(creador.pid, SIGCONT) == 0);
    detenido = 0;
    puts("API error: RPC sin respuesta tras SIGSTOP -> DSM_ERR_TIEMPO");

    /* La solicitud pudo llegar al creador antes del timeout. Retirar su alta
     * antes de reintentar conserva la semantica de una apertura unica. */
    orden = (struct api_orden){.operacion = API_DESVINCULAR, .id = reserva};
    EXIGIR(api_ejecutar(&cliente, &orden, &respuesta) == 0 &&
           (respuesta.resultado == DSM_OK || respuesta.resultado == DSM_ERR_INVAL));
    orden = (struct api_orden){.operacion = API_ABRIR, .id = reserva};
    EXIGIR(api_ejecutar(&cliente, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK &&
           respuesta.bytes == 64);
    uintptr_t ar = respuesta.ar;
    EXIGIR(api_ejecutar(&cliente, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK &&
           respuesta.ar == ar);
    EXIGIR(kill(creador.pid, SIGSTOP) == 0);
    detenido = 1;
    EXIGIR(waitpid(creador.pid, &estado_stop, WUNTRACED) == creador.pid &&
           WIFSTOPPED(estado_stop));
    orden = (struct api_orden){.operacion = API_LEER, .bytes = 1};
    EXIGIR(api_ejecutar(&cliente, &orden, &respuesta) == 0 &&
           respuesta.resultado == DSM_ERR_TIEMPO);
    orden = (struct api_orden){.operacion = API_ESCRIBIR, .bytes = 1, .datos = {0x65}};
    EXIGIR(api_ejecutar(&cliente, &orden, &respuesta) == 0 &&
           respuesta.resultado == DSM_ERR_TIEMPO);
    orden = (struct api_orden){.operacion = API_PERMISOS, .bytes = 1};
    EXIGIR(api_ejecutar(&cliente, &orden, &respuesta) == 0 &&
           strcmp(respuesta.permisos[0], "---s") == 0);
    orden = (struct api_orden){.operacion = API_LEER, .bytes = 1};
    for (unsigned i = 0; i < 20; i++) {
        EXIGIR(api_ejecutar(&cliente, &orden, &respuesta) == 0);
        if (respuesta.resultado == DSM_ERR_NODO) {
            break;
        }
        EXIGIR(respuesta.resultado == DSM_ERR_TIEMPO);
        const struct timespec pausa = {.tv_nsec = 200000000};
        nanosleep(&pausa, NULL);
    }
    EXIGIR(respuesta.resultado == DSM_ERR_NODO);
    puts("API errores: leer/escribir conservan proceso vivo y Ar protegida ante timeout/desconexion");
    EXIGIR(kill(creador.pid, SIGCONT) == 0);
    detenido = 0;
    for (unsigned i = 0; i < 100; i++) {
        if (api_contar_traza(&cliente, "par 1 hello 1 welcome 1 estado 2") >= 2) {
            break;
        }
        const struct timespec pausa = {.tv_nsec = 50000000};
        nanosleep(&pausa, NULL);
    }
    EXIGIR(api_contar_traza(&cliente, "par 1 hello 1 welcome 1 estado 2") >= 2);
    EXIGIR(api_ejecutar(&cliente, &orden, &respuesta) == 0 &&
           respuesta.resultado == DSM_ERR_NODO);
    orden = (struct api_orden){.operacion = API_PERMISOS, .bytes = 1};
    EXIGIR(api_ejecutar(&cliente, &orden, &respuesta) == 0 &&
           strcmp(respuesta.permisos[0], "---s") == 0);
    puts("API error: una nueva alta no reactiva Ar de una pagina aislada");
    orden = (struct api_orden){.operacion = API_DESVINCULAR, .id = reserva};
    EXIGIR(api_ejecutar(&cliente, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK);
    orden = (struct api_orden){.operacion = API_RESERVAR, .bytes = 64};
    EXIGIR(api_ejecutar(&creador, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK &&
           respuesta.id != reserva);
    dsm_reserva_id_t independiente = respuesta.id;
    orden = (struct api_orden){.operacion = API_ESCRIBIR, .bytes = 1, .datos = {0x76}};
    EXIGIR(api_ejecutar(&creador, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK);
    orden = (struct api_orden){.operacion = API_ABRIR, .id = independiente};
    EXIGIR(api_ejecutar(&cliente, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK);
    orden = (struct api_orden){.operacion = API_LEER, .bytes = 1};
    EXIGIR(api_ejecutar(&cliente, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK &&
           respuesta.datos[0] == 0x76);
    puts("API recuperacion: reserva independiente mantiene sus bytes tras reconexion");
    orden = (struct api_orden){.operacion = API_LIBERAR, .id = independiente};
    EXIGIR(api_ejecutar(&creador, &orden, &respuesta) == 0 &&
           respuesta.resultado == DSM_ERR_OCUPADA);
    EXIGIR(api_operar(&creador, API_CERRAR, DSM_ERR_OCUPADA) == 0);
    puts("API error: creador sin trabajadores conserva reserva remota -> DSM_ERR_OCUPADA");
    orden = (struct api_orden){.operacion = API_DESVINCULAR, .id = independiente};
    EXIGIR(api_ejecutar(&cliente, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK);
    EXIGIR(api_ejecutar(&cliente, &orden, &respuesta) == 0 &&
           respuesta.resultado == DSM_ERR_INVAL);
    orden = (struct api_orden){.operacion = API_LIBERAR, .id = independiente};
    EXIGIR(api_ejecutar(&creador, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK);
    orden.id = reserva;
    EXIGIR(api_ejecutar(&creador, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK);
    resultado = 0;
terminar:
    if (detenido) {
        kill(creador.pid, SIGCONT);
    }
    cierre_cliente = api_detener_proceso(&cliente);
    cierre_creador = api_detener_proceso(&creador);
    fclose(config);
    if (cierre_cliente != 0 || cierre_creador != 0) {
        resultado = 1;
    }
    return resultado;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);
    alarm(40);
    if (probar() != 0) {
        return 1;
    }
    puts("API errores reales: desconexion, timeout, IDs y ciclo de vida verificados");
    return 0;
}
