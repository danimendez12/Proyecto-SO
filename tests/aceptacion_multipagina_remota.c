#include "api_prueba.h"

#define EXIGIR(condicion) do { \
    if (!(condicion)) { \
        fprintf(stderr, "aceptacion_multipagina_remota:%d: %s\n", __LINE__, #condicion); \
        goto terminar; \
    } \
} while (0)

static int permisos(struct api_proceso *proceso, const char *primera,
                     const char *segunda, const char *tercera)
{
    const struct api_orden orden = {.operacion = API_PERMISOS, .bytes = 3};
    struct api_respuesta respuesta;
    return api_ejecutar(proceso, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK &&
           strcmp(respuesta.permisos[0], primera) == 0 &&
           strcmp(respuesta.permisos[1], segunda) == 0 &&
           strcmp(respuesta.permisos[2], tercera) == 0 ? 0 : -1;
}

static int probar(void)
{
    long pagina_so = sysconf(_SC_PAGESIZE);
    if (pagina_so <= 0 || (size_t)pagina_so > SIZE_MAX / 3) {
        return 1;
    }
    size_t pagina = (size_t)pagina_so;
    size_t logico = 3 * pagina - 17;
    unsigned char *esperada = calloc(logico, 1);
    if (esperada == NULL) {
        return 1;
    }
    struct api_proceso hogar = {0}, cliente = {0};
    struct api_orden orden;
    struct api_respuesta respuesta;
    dsm_reserva_id_t reserva;
    int resultado = 1, cierre_cliente, cierre_hogar;
    EXIGIR(api_iniciar_proceso(&hogar, "config/lab.ini", 0) == 0);
    EXIGIR(api_iniciar_proceso(&cliente, "config/lab.ini", 1) == 0);
    EXIGIR(api_operar(&hogar, API_INICIAR, DSM_OK) == 0);
    EXIGIR(api_operar(&cliente, API_INICIAR, DSM_OK) == 0);
    orden = (struct api_orden){.operacion = API_RESERVAR, .bytes = logico};
    EXIGIR(api_ejecutar(&hogar, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK);
    reserva = respuesta.id;
    EXIGIR(api_operar(&hogar, API_REGISTRAR, DSM_OK) == 0);
    for (size_t i = 0; i < 3; i++) {
        orden = (struct api_orden){.operacion = API_ESCRIBIR_AR, .offset = i * pagina, .bytes = 16};
        memset(orden.datos, (int)(0x31 + i), orden.bytes);
        memcpy(esperada + orden.offset, orden.datos, orden.bytes);
        EXIGIR(api_ejecutar(&hogar, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK);
    }
    orden = (struct api_orden){.operacion = API_ABRIR, .id = reserva};
    EXIGIR(api_ejecutar(&cliente, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK &&
           respuesta.bytes == logico && respuesta.id == reserva);
    EXIGIR(api_operar(&cliente, API_REGISTRAR, DSM_OK) == 0);
    EXIGIR(permisos(&cliente, "---s", "---s", "---s") == 0);
    orden = (struct api_orden){.operacion = API_LEER_AR, .bytes = 16};
    EXIGIR(api_ejecutar(&cliente, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK &&
           memcmp(respuesta.datos, esperada, orden.bytes) == 0);
    EXIGIR(permisos(&cliente, "r--s", "---s", "---s") == 0);
    EXIGIR(permisos(&hogar, "r--s", "rw-s", "rw-s") == 0);
    orden.offset = pagina;
    EXIGIR(api_ejecutar(&cliente, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK &&
           memcmp(respuesta.datos, esperada + pagina, orden.bytes) == 0);
    orden.operacion = API_ESCRIBIR_AR;
    memset(orden.datos, 0x91, orden.bytes);
    memcpy(esperada + pagina, orden.datos, orden.bytes);
    EXIGIR(api_ejecutar(&cliente, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK);
    EXIGIR(permisos(&cliente, "r--s", "rw-s", "---s") == 0);
    EXIGIR(permisos(&hogar, "r--s", "---s", "rw-s") == 0);
    EXIGIR(api_tiene_traza(&cliente, "MSI S -> M"));
    orden.operacion = API_LEER_AR;
    EXIGIR(api_ejecutar(&hogar, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK &&
           memcmp(respuesta.datos, esperada + pagina, orden.bytes) == 0);
    EXIGIR(permisos(&cliente, "r--s", "r--s", "---s") == 0);
    EXIGIR(permisos(&hogar, "r--s", "r--s", "rw-s") == 0);
    orden = (struct api_orden){.operacion = API_ESCRIBIR, .offset = pagina - 4, .bytes = 8};
    for (size_t i = 0; i < orden.bytes; i++) {
        orden.datos[i] = (unsigned char)(0xa0 + i);
    }
    memcpy(esperada + orden.offset, orden.datos, orden.bytes);
    EXIGIR(api_ejecutar(&cliente, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK);
    EXIGIR(permisos(&cliente, "rw-s", "rw-s", "---s") == 0);
    EXIGIR(permisos(&hogar, "---s", "---s", "rw-s") == 0);
    orden.operacion = API_LEER;
    EXIGIR(api_ejecutar(&hogar, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK &&
           memcmp(respuesta.datos, esperada + orden.offset, orden.bytes) == 0);
    EXIGIR(permisos(&cliente, "r--s", "r--s", "---s") == 0);
    for (size_t offset = 0; offset < logico; offset += sizeof orden.datos) {
        size_t restantes = logico - offset;
        orden = (struct api_orden){.operacion = API_LEER_AR, .offset = offset,
                                  .bytes = restantes < 64 ? restantes : 64};
        EXIGIR(api_ejecutar(&cliente, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK &&
               memcmp(respuesta.datos, esperada + offset, orden.bytes) == 0);
        EXIGIR(api_ejecutar(&hogar, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK &&
               memcmp(respuesta.datos, esperada + offset, orden.bytes) == 0);
    }
    EXIGIR(permisos(&cliente, "r--s", "r--s", "r--s") == 0);
    EXIGIR(permisos(&hogar, "r--s", "r--s", "r--s") == 0);
    orden = (struct api_orden){.operacion = API_ESCRIBIR, .offset = logico - 1, .bytes = 2};
    EXIGIR(api_ejecutar(&cliente, &orden, &respuesta) == 0 &&
           respuesta.resultado == DSM_ERR_INVAL);
    EXIGIR(api_operar(&cliente, API_DESREGISTRAR, DSM_OK) == 0);
    orden = (struct api_orden){.operacion = API_DESVINCULAR, .id = reserva};
    EXIGIR(api_ejecutar(&cliente, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK);
    EXIGIR(api_operar(&hogar, API_DESREGISTRAR, DSM_OK) == 0);
    orden = (struct api_orden){.operacion = API_LIBERAR, .id = reserva};
    EXIGIR(api_ejecutar(&hogar, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK);
    resultado = 0;
terminar:
    cierre_cliente = api_detener_proceso(&cliente);
    cierre_hogar = api_detener_proceso(&hogar);
    free(esperada);
    if (cierre_cliente != 0 || cierre_hogar != 0) {
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
    puts("Multipagina remota: MSI independiente, pagina central, cruces e integridad verificados");
    return 0;
}
