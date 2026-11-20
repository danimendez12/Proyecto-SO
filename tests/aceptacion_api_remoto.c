#include "api_prueba.h"

#define EXIGIR(condicion) do { \
    if (!(condicion)) { \
        fprintf(stderr, "aceptacion_api_remoto:%d: %s\n", __LINE__, #condicion); \
        goto terminar; \
    } \
} while (0)

static int probar_remoto(void)
{
    struct api_proceso hogar = {0}, cliente = {0};
    struct api_orden orden = {0};
    struct api_respuesta respuesta;
    dsm_reserva_id_t reserva;
    uintptr_t ar_cliente;
    int resultado = 1;
    int cierre_cliente, cierre_hogar;
    EXIGIR(api_iniciar_proceso(&hogar, "config/lab.ini", 0) == 0);
    EXIGIR(api_iniciar_proceso(&cliente, "config/lab.ini", 1) == 0);
    EXIGIR(api_operar(&hogar, API_INICIAR, DSM_OK) == 0);
    EXIGIR(api_operar(&cliente, API_INICIAR, DSM_OK) == 0);
    orden = (struct api_orden){.operacion = API_RESERVAR, .bytes = 4096};
    EXIGIR(api_ejecutar(&hogar, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK &&
           respuesta.ar != 0);
    reserva = respuesta.id;
    EXIGIR(api_operar(&hogar, API_REGISTRAR, DSM_OK) == 0);
    orden = (struct api_orden){.operacion = API_ESCRIBIR_AR, .bytes = sizeof "Hola TEC"};
    memcpy(orden.datos, "Hola TEC", orden.bytes);
    EXIGIR(api_ejecutar(&hogar, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK);
    orden = (struct api_orden){.operacion = API_ABRIR, .id = reserva};
    EXIGIR(api_ejecutar(&cliente, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK &&
           respuesta.bytes == 4096 && respuesta.id == reserva && respuesta.ar != 0);
    ar_cliente = respuesta.ar;
    EXIGIR(api_ejecutar(&cliente, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK &&
           respuesta.ar == ar_cliente);
    orden = (struct api_orden){.operacion = API_ABRIR, .id = UINT32_C(65535)};
    EXIGIR(api_ejecutar(&cliente, &orden, &respuesta) == 0 &&
           respuesta.resultado == DSM_ERR_INVAL);
    EXIGIR(api_operar(&cliente, API_REGISTRAR, DSM_OK) == 0);
    orden = (struct api_orden){.operacion = API_PERMISOS, .bytes = 1};
    EXIGIR(api_ejecutar(&cliente, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK &&
           strcmp(respuesta.permisos[0], "---s") == 0);
    orden = (struct api_orden){.operacion = API_LEER_AR, .bytes = sizeof "Hola TEC"};
    EXIGIR(api_ejecutar(&cliente, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK &&
           memcmp(respuesta.datos, "Hola TEC", orden.bytes) == 0);
    orden = (struct api_orden){.operacion = API_PERMISOS, .bytes = 1};
    EXIGIR(api_ejecutar(&hogar, &orden, &respuesta) == 0 &&
           strcmp(respuesta.permisos[0], "r--s") == 0);
    EXIGIR(api_ejecutar(&cliente, &orden, &respuesta) == 0 &&
           strcmp(respuesta.permisos[0], "r--s") == 0);
    orden = (struct api_orden){.operacion = API_ESCRIBIR_AR, .bytes = sizeof "Adios TEC"};
    memcpy(orden.datos, "Adios TEC", orden.bytes);
    EXIGIR(api_ejecutar(&cliente, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK);
    orden = (struct api_orden){.operacion = API_PERMISOS, .bytes = 1};
    EXIGIR(api_ejecutar(&hogar, &orden, &respuesta) == 0 &&
           strcmp(respuesta.permisos[0], "---s") == 0);
    EXIGIR(api_ejecutar(&cliente, &orden, &respuesta) == 0 &&
           strcmp(respuesta.permisos[0], "rw-s") == 0);
    orden = (struct api_orden){.operacion = API_LEER_AR, .bytes = sizeof "Adios TEC"};
    EXIGIR(api_ejecutar(&hogar, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK &&
           memcmp(respuesta.datos, "Adios TEC", orden.bytes) == 0);
    orden = (struct api_orden){.operacion = API_PERMISOS, .bytes = 1};
    EXIGIR(api_ejecutar(&hogar, &orden, &respuesta) == 0 &&
           strcmp(respuesta.permisos[0], "r--s") == 0);
    EXIGIR(api_ejecutar(&cliente, &orden, &respuesta) == 0 &&
           strcmp(respuesta.permisos[0], "r--s") == 0);
    EXIGIR(api_tiene_traza(&cliente, "fallo SIGSEGV lectura") &&
           api_tiene_traza(&cliente, "fallo SIGSEGV escritura") &&
           api_tiene_traza(&cliente, "MSI I -> S version 1") &&
           api_tiene_traza(&cliente, "MSI S -> M version 2") &&
           api_tiene_traza(&hogar, "MSI I -> S version 3") &&
           api_tiene_traza(&hogar, "INVALIDATE recibido") &&
           api_tiene_traza(&cliente, "INVALIDATE_ACK recibido") &&
           api_tiene_traza(&cliente, "PAGE_DOWNGRADE recibido"));
    EXIGIR(api_operar(&cliente, API_DESREGISTRAR, DSM_OK) == 0);
    for (size_t modo = 0; modo < 2; modo++) {
        size_t concesiones = api_contar_traza(&cliente, "MSI S -> M");
        EXIGIR(concesiones != SIZE_MAX);
        orden = (struct api_orden){.operacion = API_CONCURRENCIA, .bytes = modo};
        EXIGIR(api_ejecutar(&cliente, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK);
        EXIGIR(api_contar_traza(&cliente, "MSI S -> M") == concesiones + 1);
        orden = (struct api_orden){.operacion = API_LEER_AR, .bytes = 1};
        EXIGIR(api_ejecutar(&hogar, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK &&
               respuesta.datos[0] == 0x41);
        orden.offset = 64;
        EXIGIR(api_ejecutar(&hogar, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK &&
               respuesta.datos[0] == 0x42);
    }
    EXIGIR(api_operar(&cliente, API_REGISTRAR, DSM_OK) == 0);
    EXIGIR(api_operar(&hogar, API_CERRAR, DSM_ERR_OCUPADA) == 0);
    orden = (struct api_orden){.operacion = API_ESCRIBIR_AR, .bytes = sizeof "Ultimo TEC"};
    memcpy(orden.datos, "Ultimo TEC", orden.bytes);
    EXIGIR(api_ejecutar(&cliente, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK);
    orden = (struct api_orden){.operacion = API_DESVINCULAR, .id = reserva};
    EXIGIR(api_ejecutar(&cliente, &orden, &respuesta) == 0 &&
           respuesta.resultado == DSM_ERR_OCUPADA);
    EXIGIR(api_operar(&cliente, API_DESREGISTRAR, DSM_OK) == 0);
    orden = (struct api_orden){.operacion = API_DESVINCULAR, .id = reserva};
    EXIGIR(api_ejecutar(&cliente, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK);
    EXIGIR(api_ejecutar(&cliente, &orden, &respuesta) == 0 &&
           respuesta.resultado == DSM_ERR_INVAL);
    orden = (struct api_orden){.operacion = API_LEER_AR, .bytes = sizeof "Ultimo TEC"};
    EXIGIR(api_ejecutar(&hogar, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK &&
           memcmp(respuesta.datos, "Ultimo TEC", orden.bytes) == 0);
    EXIGIR(api_operar(&hogar, API_DESREGISTRAR, DSM_OK) == 0);
    orden = (struct api_orden){.operacion = API_LIBERAR, .id = reserva};
    EXIGIR(api_ejecutar(&hogar, &orden, &respuesta) == 0 && respuesta.resultado == DSM_OK);
    resultado = 0;
terminar:
    cierre_cliente = api_detener_proceso(&cliente);
    cierre_hogar = api_detener_proceso(&hogar);
    if (cierre_cliente != 0 || cierre_hogar != 0) {
        resultado = 1;
    }
    return resultado;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(40);
    signal(SIGPIPE, SIG_IGN);
    if (probar_remoto() != 0) {
        return 1;
    }
    puts("API remoto: dos procesos, Ar I/S/M, escritura y relectura Adios TEC verificadas");
    return 0;
}
