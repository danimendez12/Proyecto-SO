#include "api_prueba.h"

#define EXIGIR(condicion) do { \
    if (!(condicion)) { \
        fprintf(stderr, "aceptacion_multihogar:%d: %s\n", __LINE__, #condicion); \
        goto terminar; \
    } \
} while (0)

static int permisos(struct api_proceso *proceso, const char *a, const char *b, const char *c)
{
    struct api_orden orden = {.operacion = API_PERMISOS, .bytes = 3};
    struct api_respuesta r;
    return api_ejecutar(proceso, &orden, &r) == 0 && r.resultado == DSM_OK &&
           strcmp(r.permisos[0], a) == 0 && strcmp(r.permisos[1], b) == 0 &&
           strcmp(r.permisos[2], c) == 0 ? 0 : -1;
}

static int esperar_pares(struct api_proceso *nodo, unsigned propio)
{
    for (unsigned par = 0; par < 3; par++) {
        if (par == propio) {
            continue;
        }
        char frase[80];
        snprintf(frase, sizeof frase, "par %u hello 1 welcome 1 estado 2", par);
        unsigned intento;
        for (intento = 0; intento < 100; intento++) {
            if (api_tiene_traza(nodo, frase)) {
                break;
            }
            const struct timespec pausa = {.tv_nsec = 50000000};
            nanosleep(&pausa, NULL);
        }
        if (intento == 100) {
            return -1;
        }
    }
    return 0;
}

static int version(struct api_proceso *nodo, uint32_t reserva, uint64_t gpn, unsigned *salida)
{
    char texto[32768];
    ssize_t n = pread(fileno(nodo->registro), texto, sizeof texto - 1, 0);
    if (n < 0 || n == (ssize_t)sizeof texto - 1) {
        return -1;
    }
    texto[n] = '\0';
    char marca[96];
    snprintf(marca, sizeof marca, "reserva %" PRIu32 " gpn %" PRIu64 " MSI ", reserva, gpn);
    char *posicion = texto;
    int encontrada = 0;
    while ((posicion = strstr(posicion, marca)) != NULL) {
        const char *v = strstr(posicion, " version ");
        const char *fin = strchr(posicion, '\n');
        if (v != NULL && (fin == NULL || v < fin) && sscanf(v, " version %u", salida) == 1) {
            encontrada = 1;
        }
        posicion += strlen(marca);
    }
    return encontrada ? 0 : -1;
}

struct escritura {
    struct api_proceso *proceso;
    pthread_barrier_t *barrera;
    struct api_orden orden;
    int resultado;
};

static void *escribir(void *arg)
{
    struct escritura *tarea = arg;
    struct api_respuesta r;
    pthread_barrier_wait(tarea->barrera);
    tarea->resultado = api_ejecutar(tarea->proceso, &tarea->orden, &r) == 0 &&
                       r.resultado == DSM_OK ? 0 : -1;
    return NULL;
}

static int probar(void)
{
    enum { PAGINA = 4096, LOGICO = 3 * PAGINA - 11 };
    FILE *config = tmpfile();
    if (config == NULL) {
        return 1;
    }
    int rc = fprintf(config,
        "page_size=4096\nmemoria_inicial=8192\nmax_nodos=3\nmax_hilos=8\n"
        "rpc_timeout_ms=1500\nheartbeat_intervalo_ms=100\nheartbeat_fallos=10\n"
        "puerto_rango_inicio=27200\npuerto_rango_fin=27299\n"
        "nodo=0 hogar0 127.0.0.1 27200 8192\n"
        "nodo=1 hogar1 127.0.0.1 27201 8192\n"
        "nodo=2 cliente 127.0.0.1 27202 4096\n");
    if (rc < 0 || fflush(config) != 0) {
        fclose(config);
        return 1;
    }
    char ruta[64];
    snprintf(ruta, sizeof ruta, "/proc/self/fd/%d", fileno(config));
    struct api_proceso nodos[3] = {0};
    struct api_orden orden;
    struct api_respuesta r;
    uint32_t reserva = 0, ocupada = 0, relleno = 0;
    uintptr_t base_creador, base_cliente;
    uint64_t gpns[3];
    unsigned versiones[3] = {0};
    unsigned char esperada[LOGICO];
    memset(esperada, 0, sizeof esperada);
    int resultado = 1;
    for (unsigned i = 0; i < 3; i++) {
        EXIGIR(api_iniciar_proceso(&nodos[i], ruta, (uint16_t)i) == 0);
        EXIGIR(api_operar(&nodos[i], API_INICIAR, DSM_OK) == 0);
    }
    for (unsigned i = 0; i < 3; i++) {
        EXIGIR(esperar_pares(&nodos[i], i) == 0);
    }
    orden = (struct api_orden){.operacion = API_RESERVAR, .bytes = PAGINA};
    EXIGIR(api_ejecutar(&nodos[2], &orden, &r) == 0 && r.resultado == DSM_OK);
    ocupada = r.id;
    EXIGIR(api_ejecutar(&nodos[1], &orden, &r) == 0 && r.resultado == DSM_OK);
    relleno = r.id;
    orden = (struct api_orden){.operacion = API_UBICACION, .offset = 0};
    EXIGIR(api_ejecutar(&nodos[1], &orden, &r) == 0 && r.resultado == DSM_OK &&
           r.hogar == 1 && r.gpn == 2);
    orden = (struct api_orden){.operacion = API_RESERVAR, .bytes = LOGICO};
    EXIGIR(api_ejecutar(&nodos[0], &orden, &r) == 0 && r.resultado == DSM_OK);
    reserva = r.id;
    base_creador = r.ar;
    orden = (struct api_orden){.operacion = API_LIBERAR, .id = relleno};
    EXIGIR(api_ejecutar(&nodos[1], &orden, &r) == 0 && r.resultado == DSM_OK);
    EXIGIR(api_operar(&nodos[1], API_CERRAR, DSM_ERR_OCUPADA) == 0);
    for (size_t i = 0; i < 3; i++) {
        orden = (struct api_orden){.operacion = API_UBICACION, .offset = i};
        EXIGIR(api_ejecutar(&nodos[0], &orden, &r) == 0 && r.resultado == DSM_OK &&
               r.hogar == (i < 2 ? 0 : 1));
        gpns[i] = r.gpn;
        EXIGIR(gpns[i] == (i < 2 ? i : 3));
        EXIGIR((r.hogar == 0 && r.gpn < 2) || (r.hogar == 1 && r.gpn >= 2 && r.gpn < 4));
        for (size_t j = 0; j < i; j++) {
            EXIGIR(gpns[i] != gpns[j]);
        }
        printf("reserva %" PRIu32 " pagina %zu hogar %u gpn %" PRIu64 "\n",
               reserva, i, r.hogar, r.gpn);
        orden = (struct api_orden){.operacion = API_TRADUCIR, .offset = i * PAGINA};
        EXIGIR(api_ejecutar(&nodos[0], &orden, &r) == 0 && r.resultado == DSM_OK &&
               r.traducida == base_creador + i * PAGINA);
    }
    orden = (struct api_orden){.operacion = API_UBICACION, .offset = 3};
    EXIGIR(api_ejecutar(&nodos[0], &orden, &r) == 0 && r.resultado == DSM_ERR_INVAL);
    EXIGIR(api_operar(&nodos[0], API_REGISTRAR, DSM_OK) == 0);
    for (size_t i = 0; i < 3; i++) {
        orden = (struct api_orden){.operacion = API_ESCRIBIR_AR, .offset = i * PAGINA, .bytes = 16};
        memset(orden.datos, (int)(0x31 + i), orden.bytes);
        memcpy(esperada + orden.offset, orden.datos, orden.bytes);
        EXIGIR(api_ejecutar(&nodos[0], &orden, &r) == 0 && r.resultado == DSM_OK);
    }
    orden = (struct api_orden){.operacion = API_ABRIR, .id = reserva};
    EXIGIR(api_ejecutar(&nodos[2], &orden, &r) == 0 && r.resultado == DSM_OK && r.bytes == LOGICO);
    base_cliente = r.ar;
    printf("reserva %" PRIu32 " bytes %zu Ar creador 0x%" PRIxPTR " cliente 0x%" PRIxPTR
           " contiguas por traduccion publica\n", reserva, (size_t)LOGICO, base_creador, base_cliente);
    for (size_t i = 0; i < 3; i++) {
        orden = (struct api_orden){.operacion = API_UBICACION, .offset = i};
        EXIGIR(api_ejecutar(&nodos[2], &orden, &r) == 0 && r.resultado == DSM_OK &&
               r.gpn == gpns[i] && r.hogar == (i < 2 ? 0 : 1));
        orden = (struct api_orden){.operacion = API_TRADUCIR, .offset = i * PAGINA};
        EXIGIR(api_ejecutar(&nodos[2], &orden, &r) == 0 && r.resultado == DSM_OK &&
               r.traducida == base_cliente + i * PAGINA);
    }
    EXIGIR(api_operar(&nodos[2], API_REGISTRAR, DSM_OK) == 0);
    EXIGIR(permisos(&nodos[2], "---s", "---s", "---s") == 0);
    for (size_t i = 0; i < 3; i += 2) {
        orden = (struct api_orden){.operacion = API_LEER_AR, .offset = i * PAGINA, .bytes = 16};
        EXIGIR(api_ejecutar(&nodos[2], &orden, &r) == 0 && r.resultado == DSM_OK &&
               memcmp(r.datos, esperada + orden.offset, orden.bytes) == 0);
        EXIGIR(version(&nodos[2], reserva, gpns[i], &versiones[i]) == 0);
    }
    EXIGIR(permisos(&nodos[0], "r--s", "rw-s", "r--s") == 0);
    EXIGIR(permisos(&nodos[2], "r--s", "---s", "r--s") == 0);
    for (size_t i = 0; i < 3; i += 2) {
        orden = (struct api_orden){.operacion = API_ESCRIBIR_AR, .offset = i * PAGINA, .bytes = 16};
        memset(orden.datos, (int)(0x81 + i), orden.bytes);
        memcpy(esperada + orden.offset, orden.datos, orden.bytes);
        EXIGIR(api_ejecutar(&nodos[2], &orden, &r) == 0 && r.resultado == DSM_OK);
        unsigned nueva;
        EXIGIR(version(&nodos[2], reserva, gpns[i], &nueva) == 0 && nueva > versiones[i]);
        versiones[i] = nueva;
        size_t vecina = 2 - i;
        unsigned intacta;
        EXIGIR(version(&nodos[2], reserva, gpns[vecina], &intacta) == 0 &&
               intacta == versiones[vecina]);
    }
    EXIGIR(permisos(&nodos[0], "---s", "rw-s", "---s") == 0);
    EXIGIR(permisos(&nodos[2], "rw-s", "---s", "rw-s") == 0);
    printf("versiones independientes gpn %" PRIu64 "=%u gpn %" PRIu64 "=%u; "
           "creador I/M/I, cliente M/I/M\n", gpns[0], versiones[0], gpns[2], versiones[2]);
    EXIGIR(api_tiene_traza(&nodos[2], "PAGE_REQUEST enviado hogar 0") &&
           api_tiene_traza(&nodos[2], "PAGE_REQUEST enviado hogar 1") &&
           api_tiene_traza(&nodos[2], "fallo SIGSEGV escritura"));
    pthread_barrier_t barrera;
    EXIGIR(pthread_barrier_init(&barrera, NULL, 2) == 0);
    struct escritura tareas[2] = {
        {.proceso = &nodos[0], .barrera = &barrera,
         .orden = {.operacion = API_ESCRIBIR_AR, .offset = 64, .bytes = 1, .datos = {0xa0}}},
        {.proceso = &nodos[2], .barrera = &barrera,
         .orden = {.operacion = API_ESCRIBIR_AR, .offset = 2 * PAGINA + 64,
                   .bytes = 1, .datos = {0xa2}}},
    };
    pthread_t hilo;
    if (pthread_create(&hilo, NULL, escribir, &tareas[0]) != 0) {
        pthread_barrier_destroy(&barrera);
        goto terminar;
    }
    escribir(&tareas[1]);
    int unido = pthread_join(hilo, NULL);
    pthread_barrier_destroy(&barrera);
    EXIGIR(unido == 0 && tareas[0].resultado == 0 && tareas[1].resultado == 0);
    esperada[64] = 0xa0;
    esperada[2 * PAGINA + 64] = 0xa2;
    orden = (struct api_orden){.operacion = API_ESCRIBIR, .offset = 2 * PAGINA - 4, .bytes = 8};
    for (size_t i = 0; i < orden.bytes; i++) {
        orden.datos[i] = (unsigned char)(0xc0 + i);
    }
    memcpy(esperada + orden.offset, orden.datos, orden.bytes);
    EXIGIR(api_ejecutar(&nodos[2], &orden, &r) == 0 && r.resultado == DSM_OK);
    EXIGIR(permisos(&nodos[0], "rw-s", "---s", "---s") == 0);
    EXIGIR(permisos(&nodos[2], "---s", "rw-s", "rw-s") == 0);
    orden.operacion = API_LEER;
    EXIGIR(api_ejecutar(&nodos[0], &orden, &r) == 0 && r.resultado == DSM_OK &&
           memcmp(r.datos, esperada + orden.offset, orden.bytes) == 0);
    for (size_t offset = 0; offset < LOGICO; offset += 64) {
        size_t n = LOGICO - offset < 64 ? LOGICO - offset : 64;
        orden = (struct api_orden){.operacion = API_LEER_AR, .offset = offset, .bytes = n};
        EXIGIR(api_ejecutar(&nodos[0], &orden, &r) == 0 && r.resultado == DSM_OK &&
               memcmp(r.datos, esperada + offset, n) == 0);
        EXIGIR(api_ejecutar(&nodos[2], &orden, &r) == 0 && r.resultado == DSM_OK &&
               memcmp(r.datos, esperada + offset, n) == 0);
    }
    EXIGIR(permisos(&nodos[0], "r--s", "r--s", "r--s") == 0);
    EXIGIR(permisos(&nodos[2], "r--s", "r--s", "r--s") == 0);
    /* Dos procesos compiten por el mismo GPN no contiguo del hogar 1. */
    EXIGIR(pthread_barrier_init(&barrera, NULL, 2) == 0);
    tareas[0].orden = (struct api_orden){.operacion = API_ESCRIBIR_AR,
        .offset = 2 * PAGINA + 128, .bytes = 1, .datos = {0xd0}};
    tareas[1].orden = (struct api_orden){.operacion = API_ESCRIBIR_AR,
        .offset = 2 * PAGINA + 129, .bytes = 1, .datos = {0xd2}};
    if (pthread_create(&hilo, NULL, escribir, &tareas[0]) != 0) {
        pthread_barrier_destroy(&barrera);
        goto terminar;
    }
    escribir(&tareas[1]);
    unido = pthread_join(hilo, NULL);
    pthread_barrier_destroy(&barrera);
    EXIGIR(unido == 0 && tareas[0].resultado == 0 && tareas[1].resultado == 0);
    struct api_respuesta protecciones[2];
    orden = (struct api_orden){.operacion = API_PERMISOS, .bytes = 3};
    EXIGIR(api_ejecutar(&nodos[0], &orden, &protecciones[0]) == 0 &&
           protecciones[0].resultado == DSM_OK);
    EXIGIR(api_ejecutar(&nodos[2], &orden, &protecciones[1]) == 0 &&
           protecciones[1].resultado == DSM_OK);
    for (size_t i = 0; i < 2; i++) {
        EXIGIR(strcmp(protecciones[i].permisos[0], "r--s") == 0 &&
               strcmp(protecciones[i].permisos[1], "r--s") == 0);
    }
    EXIGIR((strcmp(protecciones[0].permisos[2], "rw-s") == 0 &&
            strcmp(protecciones[1].permisos[2], "---s") == 0) ||
           (strcmp(protecciones[0].permisos[2], "---s") == 0 &&
            strcmp(protecciones[1].permisos[2], "rw-s") == 0));
    printf("escritores mismo gpn %" PRIu64 " permisos creador %s cliente %s: unico M\n",
           gpns[2], protecciones[0].permisos[2], protecciones[1].permisos[2]);
    esperada[2 * PAGINA + 128] = 0xd0;
    esperada[2 * PAGINA + 129] = 0xd2;
    orden = (struct api_orden){.operacion = API_LEER_AR, .offset = 2 * PAGINA + 120, .bytes = 32};
    for (size_t i = 0; i < 3; i += 2) {
        EXIGIR(api_ejecutar(&nodos[i], &orden, &r) == 0 && r.resultado == DSM_OK &&
               memcmp(r.datos, esperada + orden.offset, orden.bytes) == 0);
    }
    EXIGIR(permisos(&nodos[0], "r--s", "r--s", "r--s") == 0);
    EXIGIR(permisos(&nodos[2], "r--s", "r--s", "r--s") == 0);
    EXIGIR(api_operar(&nodos[0], API_DESREGISTRAR, DSM_OK) == 0);
    orden = (struct api_orden){.operacion = API_LIBERAR, .id = reserva};
    EXIGIR(api_ejecutar(&nodos[0], &orden, &r) == 0 && r.resultado == DSM_ERR_OCUPADA);
    EXIGIR(api_operar(&nodos[2], API_DESREGISTRAR, DSM_OK) == 0);
    orden = (struct api_orden){.operacion = API_DESVINCULAR, .id = reserva};
    EXIGIR(api_ejecutar(&nodos[2], &orden, &r) == 0 && r.resultado == DSM_OK);
    orden = (struct api_orden){.operacion = API_ABRIR, .id = reserva};
    EXIGIR(api_ejecutar(&nodos[1], &orden, &r) == 0 && r.resultado == DSM_OK && r.bytes == LOGICO);
    EXIGIR(api_operar(&nodos[1], API_REGISTRAR, DSM_OK) == 0);
    for (size_t i = 0; i < 3; i += 2) {
        orden = (struct api_orden){.operacion = API_LEER_AR, .offset = i * PAGINA, .bytes = 16};
        EXIGIR(api_ejecutar(&nodos[1], &orden, &r) == 0 && r.resultado == DSM_OK &&
               memcmp(r.datos, esperada + orden.offset, orden.bytes) == 0);
    }
    orden = (struct api_orden){.operacion = API_LEER_AR, .offset = 2 * PAGINA + 120, .bytes = 32};
    EXIGIR(api_ejecutar(&nodos[1], &orden, &r) == 0 && r.resultado == DSM_OK &&
           memcmp(r.datos, esperada + orden.offset, orden.bytes) == 0);
    EXIGIR(api_operar(&nodos[1], API_DESREGISTRAR, DSM_OK) == 0);
    orden = (struct api_orden){.operacion = API_DESVINCULAR, .id = reserva};
    EXIGIR(api_ejecutar(&nodos[1], &orden, &r) == 0 && r.resultado == DSM_OK);
    EXIGIR(api_operar(&nodos[0], API_REGISTRAR, DSM_OK) == 0);
    orden = (struct api_orden){.operacion = API_LEER_AR, .offset = 2 * PAGINA, .bytes = 16};
    EXIGIR(api_ejecutar(&nodos[0], &orden, &r) == 0 && r.resultado == DSM_OK &&
           memcmp(r.datos, esperada + orden.offset, orden.bytes) == 0);
    EXIGIR(api_operar(&nodos[0], API_DESREGISTRAR, DSM_OK) == 0);
    orden = (struct api_orden){.operacion = API_LIBERAR, .id = reserva};
    EXIGIR(api_ejecutar(&nodos[0], &orden, &r) == 0 && r.resultado == DSM_OK);
    orden = (struct api_orden){.operacion = API_ABRIR, .id = reserva};
    EXIGIR(api_ejecutar(&nodos[2], &orden, &r) == 0 && r.resultado == DSM_ERR_INVAL);
    for (int propio = 1; propio >= 0; propio--) {
        orden = (struct api_orden){.operacion = API_RESERVAR, .bytes = 2 * PAGINA};
        EXIGIR(api_ejecutar(&nodos[propio], &orden, &r) == 0 && r.resultado == DSM_OK);
        uint32_t recuperada = r.id;
        for (size_t i = 0; i < 2; i++) {
            orden = (struct api_orden){.operacion = API_UBICACION, .offset = i};
            EXIGIR(api_ejecutar(&nodos[propio], &orden, &r) == 0 && r.resultado == DSM_OK &&
                   r.hogar == (unsigned)propio);
        }
        orden = (struct api_orden){.operacion = API_LIBERAR, .id = recuperada};
        EXIGIR(api_ejecutar(&nodos[propio], &orden, &r) == 0 && r.resultado == DSM_OK);
    }
    orden = (struct api_orden){.operacion = API_LIBERAR, .id = ocupada};
    EXIGIR(api_ejecutar(&nodos[2], &orden, &r) == 0 && r.resultado == DSM_OK);
    resultado = 0;
terminar:
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
    alarm(55);
    if (probar() != 0) {
        return 1;
    }
    puts("Multihogar real: hogares 0/1, cliente 2, Ar contigua, MSI y capacidad recuperada");
    return 0;
}
