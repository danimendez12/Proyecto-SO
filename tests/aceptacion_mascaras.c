#include "api_prueba.h"
#include "directory.h"

#define EXIGIR(c) do { if (!(c)) { \
    fprintf(stderr, "aceptacion_mascaras:%d: %s\n", __LINE__, #c); \
    goto terminar; } } while (0)

static const uint16_t ids[] = {0, 31, 32, 39};

static int esperar_red(struct api_proceso *nodos)
{
    for (size_t a = 0; a < sizeof ids / sizeof ids[0]; a++) {
        for (size_t b = 0; b < sizeof ids / sizeof ids[0]; b++) {
            if (a == b) {
                continue;
            }
            char texto[80];
            snprintf(texto, sizeof texto, "par %u hello 1 welcome 1 estado 2", ids[b]);
            unsigned intentos;
            for (intentos = 0; intentos < 200 && !api_tiene_traza(&nodos[a], texto); intentos++) {
                const struct timespec pausa = {.tv_nsec = 25000000};
                nanosleep(&pausa, NULL);
            }
            if (intentos == 200) {
                return -1;
            }
        }
    }
    return 0;
}

/* La asignacion puede esperar los 36 puestos ausentes, uno por uno; el plazo
 * del controlador es mayor que ese recorrido, sin cambiar los plazos RPC. */
static int reservar(struct api_proceso *nodo, size_t bytes, struct api_respuesta *r)
{
    struct api_orden orden = {.operacion = API_RESERVAR, .bytes = bytes};
    if (api_transferir(nodo->entrada, &orden, sizeof orden, 1) != 0) {
        return -1;
    }
    struct pollfd espera = {.fd = nodo->salida, .events = POLLIN};
    int rc;
    do {
        rc = poll(&espera, 1, 30000);
    } while (rc < 0 && errno == EINTR);
    return rc > 0 ? api_transferir(nodo->salida, r, sizeof *r, 0) : -1;
}

static int verificar_fuera(const char *ruta)
{
    dsm_config_t cfg;
    dsm_directorio_t dir;
    if (dsm_config_cargar(ruta, &cfg) != 0 || dsm_directorio_init(&dir, &cfg, 0) != 0) {
        return -1;
    }
    uint64_t mascara = 0;
    int resultado = -1;
    for (size_t i = 1; i < sizeof ids / sizeof ids[0]; i++) {
        mascara |= UINT64_C(1) << ids[i];
        dsm_directorio_par_fuera(&dir, ids[i]);
        if (dir.fuera != mascara) {
            goto terminar;
        }
    }
    dsm_directorio_par_fuera(&dir, 0);
    dsm_directorio_par_fuera(&dir, 40);
    dsm_directorio_par_linea(&dir, UINT16_MAX);
    if (dir.fuera != mascara) {
        goto terminar;
    }
    for (size_t i = 1; i < sizeof ids / sizeof ids[0]; i++) {
        mascara &= ~(UINT64_C(1) << ids[i]);
        dsm_directorio_par_linea(&dir, ids[i]);
        if (dir.fuera != mascara) {
            goto terminar;
        }
    }
    resultado = 0;
terminar:
    dsm_directorio_cerrar(&dir);
    return resultado;
}

static int probar_reserva(struct api_proceso *nodos, size_t p)
{
    struct api_respuesta r;
    if (reservar(&nodos[0], 4 * p, &r) != 0 || r.resultado != DSM_OK) {
        return -1;
    }
    dsm_reserva_id_t reserva = r.id;
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        struct api_orden orden = {.operacion = API_UBICACION, .offset = i};
        if (api_ejecutar(&nodos[0], &orden, &r) != 0 || r.resultado != DSM_OK ||
            r.hogar != ids[i] || r.gpn != ids[i]) {
            return -1;
        }
        printf("mascaras reserva %" PRIu32 " pagina %zu hogar %u gpn %" PRIu64 "\n",
               reserva, i, r.hogar, r.gpn);
        orden = (struct api_orden){.operacion = API_ESCRIBIR, .offset = i * p,
                                   .bytes = 1, .datos = {(unsigned char)(0x40 + i)}};
        if (api_ejecutar(&nodos[0], &orden, &r) != 0 || r.resultado != DSM_OK) {
            return -1;
        }
    }
    /* Un donante de ID alto abre toda la vista, toma M y la devuelve a los
     * cuatro hogares antes de quitar su pin en el creador. */
    struct api_orden orden = {.operacion = API_ABRIR, .id = reserva};
    if (api_ejecutar(&nodos[3], &orden, &r) != 0 || r.resultado != DSM_OK || r.bytes != 4 * p) {
        return -1;
    }
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        orden = (struct api_orden){.operacion = API_LEER, .offset = i * p, .bytes = 1};
        if (api_ejecutar(&nodos[3], &orden, &r) != 0 || r.resultado != DSM_OK ||
            r.datos[0] != (unsigned char)(0x40 + i)) {
            return -1;
        }
        orden = (struct api_orden){.operacion = API_ESCRIBIR, .offset = i * p,
                                   .bytes = 1, .datos = {(unsigned char)(0x70 + i)}};
        if (api_ejecutar(&nodos[3], &orden, &r) != 0 || r.resultado != DSM_OK) {
            return -1;
        }
    }
    orden = (struct api_orden){.operacion = API_LIBERAR, .id = reserva};
    if (api_ejecutar(&nodos[0], &orden, &r) != 0 || r.resultado != DSM_ERR_OCUPADA) {
        return -1;
    }
    orden.operacion = API_DESVINCULAR;
    if (api_ejecutar(&nodos[3], &orden, &r) != 0 || r.resultado != DSM_OK) {
        return -1;
    }
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        orden = (struct api_orden){.operacion = API_LEER, .offset = i * p, .bytes = 1};
        if (api_ejecutar(&nodos[0], &orden, &r) != 0 || r.resultado != DSM_OK ||
            r.datos[0] != (unsigned char)(0x70 + i)) {
            return -1;
        }
    }
    orden = (struct api_orden){.operacion = API_LIBERAR, .id = reserva};
    return api_ejecutar(&nodos[0], &orden, &r) == 0 && r.resultado == DSM_OK ? 0 : -1;
}

int main(void)
{
    alarm(120);
    FILE *config = tmpfile();
    struct api_proceso nodos[4] = {{0}};
    int resultado = 1;
    long pagina_so = sysconf(_SC_PAGESIZE);
    EXIGIR(config != NULL && pagina_so > 0);
    size_t p = (size_t)pagina_so;
    EXIGIR(fprintf(config, "page_size = %zu\nmemoria_inicial = %zu\nmax_nodos = 40\n"
                   "max_hilos = 8\nrpc_timeout_ms = 150\n"
                   "heartbeat_intervalo_ms = 250\nheartbeat_fallos = 10\n"
                   "puerto_rango_inicio = 27200\npuerto_rango_fin = 27299\n", p, p) > 0);
    for (unsigned i = 0; i < 40; i++) {
        EXIGIR(fprintf(config, "nodo = %u puesto%u 127.0.0.1 %u\n", i, i, 27240 + i) > 0);
    }
    EXIGIR(fflush(config) == 0);
    char ruta[64];
    snprintf(ruta, sizeof ruta, "/proc/self/fd/%d", fileno(config));
    EXIGIR(verificar_fuera(ruta) == 0);
    puts("mascaras: fuera/linea conserva los bits independientes 31, 32 y 39");
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        EXIGIR(api_iniciar_proceso(&nodos[i], ruta, ids[i]) == 0 &&
               api_operar(&nodos[i], API_INICIAR, DSM_OK) == 0);
    }
    EXIGIR(esperar_red(nodos) == 0);
    struct api_respuesta r;
    EXIGIR(reservar(&nodos[0], 5 * p, &r) == 0 &&
           (r.resultado == DSM_ERR_NODO || r.resultado == DSM_ERR_NOMEM));
    for (size_t i = 1; i < sizeof ids / sizeof ids[0]; i++) {
        char texto[80];
        snprintf(texto, sizeof texto, "CAP_CANCEL hogar %u", ids[i]);
        EXIGIR(api_tiene_traza(&nodos[0], texto));
    }
    EXIGIR(probar_reserva(nodos, p) == 0);
    puts("mascaras: cancelacion, confirmacion, cierre de copias y liberacion en hogares 0/31/32/39");
    /* No quedan reservas antiguas: se prueba una nueva sesion real del mismo
     * puesto, sin prometer recuperar paginas aisladas de una sesion perdida. */
    size_t altas_antes = api_contar_traza(&nodos[0], "par 39 hello 1 welcome 1 estado 2");
    EXIGIR(altas_antes != SIZE_MAX && altas_antes > 0);
    EXIGIR(api_operar(&nodos[3], API_CERRAR, DSM_OK) == 0);
    EXIGIR(api_operar(&nodos[3], API_INICIAR, DSM_OK) == 0);
    unsigned intentos;
    for (intentos = 0; intentos < 200; intentos++) {
        size_t altas = api_contar_traza(&nodos[0], "par 39 hello 1 welcome 1 estado 2");
        if (altas != SIZE_MAX && altas > altas_antes) {
            break;
        }
        const struct timespec pausa = {.tv_nsec = 25000000};
        nanosleep(&pausa, NULL);
    }
    EXIGIR(intentos < 200 && esperar_red(nodos) == 0);
    EXIGIR(probar_reserva(nodos, p) == 0);
    puts("mascaras: nodo 39 se reincorpora y se reutiliza toda la capacidad multihogar");
    resultado = 0;
terminar:
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        if (api_detener_proceso(&nodos[i]) != 0) {
            resultado = 1;
        }
    }
    if (config != NULL) {
        fclose(config);
    }
    return resultado;
}
