#include "api_prueba.h"
#include "directory.h"
#include "consola_consultas.h"

#define EXIGIR(c) do { if (!(c)) { \
    fprintf(stderr, "inventario:%d: %s\n", __LINE__, #c); goto terminar; } } while (0)

static int inyectar_servicio;
static uint64_t gpn_servicio;
static uint32_t reserva_servicio;

int __real_dsm_directorio_init(dsm_directorio_t *, const dsm_config_t *, uint16_t);
int __wrap_dsm_directorio_init(dsm_directorio_t *dir, const dsm_config_t *cfg, uint16_t id)
{
    int rc = __real_dsm_directorio_init(dir, cfg, id);
    if (rc == 0 && inyectar_servicio && id == 0) {
        dsm_pagina_desc_t p;
        inyectar_servicio = 0;
        rc = dsm_directorio_reservar_pagina(dir, &p);
        if (rc == 0) { gpn_servicio = p.gpn; reserva_servicio = p.reserva; }
    }
    return rc;
}

static int codecs(void)
{
    int resultado = 1;
    uint8_t buf[8192]; /* Incluye las 60 fichas; nunca una trama de 1 MiB en pila. */
    size_t n;
    dsm_cabecera_t cab = {.version = DSM_PROTO_VERSION, .tipo = DSM_TIPO_REGION_INFO,
        .banderas = DSM_FLAG_RESPUESTA, .origen = 0, .destino = 1, .id_msg = 2, .id_corr = 1};
    dsm_cabecera_t leida;
    dsm_region_info_t info = {.reserva_id = 1, .total_tramos = 1, .cantidad_tramos = 1,
        .total_paginas = 1, .tamano_bytes = 4096, .tramos = {{.cantidad = 1}}}, copia;
    dsm_stats_reply_t recibida = {0};
    EXIGIR(dsm_region_info_codificar(buf, sizeof buf, &cab, &info, &n) == 0 && n == 76);
    EXIGIR(dsm_region_info_decodificar(buf, n, &leida, &copia) == 0 && copia.publica == 0);
    info.publica = 1;
    EXIGIR(dsm_region_info_codificar(buf, sizeof buf, &cab, &info, &n) == 0 && n == 80);
    EXIGIR(dsm_region_info_decodificar(buf, n, &leida, &copia) == 0 && copia.publica == 1);
    buf[n - 1] ^= 1;
    EXIGIR(dsm_region_info_decodificar(buf, n, &leida, &copia) == -1);
    buf[n - 1] ^= 1;
    EXIGIR(dsm_region_info_decodificar(buf, n - 1, &leida, &copia) == -1);
    memset(buf + 52, 0xff, 4);
    EXIGIR(dsm_region_info_decodificar(buf, n, &leida, &copia) == -1);
    dsm_capacidad_t capacidad = {.reserva_id = 1, .token = 99, .info = info}, cap_leida;
    cab.tipo = DSM_TIPO_CAP_COMMIT; cab.banderas = 0; cab.id_corr = 0;
    EXIGIR(dsm_capacidad_codificar(buf, sizeof buf, &cab, &capacidad, &n) == 0 && n == 104);
    EXIGIR(dsm_capacidad_decodificar(buf, n, &leida, &cap_leida) == 0 && cap_leida.info.publica);
    buf[n - 1] ^= 1;
    EXIGIR(dsm_capacidad_decodificar(buf, n, &leida, &cap_leida) == -1);
    capacidad.info.publica = 0;
    EXIGIR(dsm_capacidad_codificar(buf, sizeof buf, &cab, &capacidad, &n) == 0 && n == 100);
    EXIGIR(dsm_capacidad_decodificar(buf, n, &leida, &cap_leida) == 0 && !cap_leida.info.publica);
    dsm_stats_request_t pedido = {.seleccion = DSM_STATS_INVENTARIO,
        .limite = DSM_STATS_RANGO_MAX, .desde = UINT64_MAX}, consulta;
    cab.tipo = DSM_TIPO_STATS_REQUEST;
    EXIGIR(dsm_stats_request_codificar(buf, sizeof buf, &cab, &pedido, &n) == 0 && n == 48);
    EXIGIR(dsm_stats_request_decodificar(buf, n, &leida, &consulta) == 0 &&
           consulta.desde == UINT64_MAX && consulta.limite == DSM_STATS_RANGO_MAX);
    buf[33] = 2;
    EXIGIR(dsm_stats_request_decodificar(buf, n, &leida, &consulta) == -1);
    buf[33] = 1; memset(buf + 36, 0, 4);
    EXIGIR(dsm_stats_request_decodificar(buf, n, &leida, &consulta) == -1);
    dsm_pagina_consulta_t paginas[2] = {
        {.gpn = 40, .poseedor = DSM_POSEEDOR_NINGUNO, .orden = UINT32_MAX, .banderas = DSM_PAGINA_LIBRE},
        {.gpn = 41, .reserva = 1, .orden = 0, .poseedor = DSM_POSEEDOR_NINGUNO,
         .banderas = DSM_PAGINA_PREPARADA | DSM_PAGINA_PUBLICA},
    };
    dsm_stats_reply_t respuesta = {.emisor = 0, .n = 1, .p = 2, .paginas = paginas,
        .rango = 1, .mas = 1, .siguiente = 42};
    cab.tipo = DSM_TIPO_STATS_REPLY; cab.banderas = DSM_FLAG_RESPUESTA; cab.id_corr = 1;
    EXIGIR(dsm_stats_reply_codificar(buf, sizeof buf, &cab, &respuesta, &n) == 0 && n == 304);
    EXIGIR(dsm_stats_reply_decodificar(buf, n, &leida, &recibida) == 0 && recibida.rango &&
           recibida.mas && recibida.siguiente == 42 && recibida.paginas[1].banderas == paginas[1].banderas);
    free(recibida.paginas); recibida.paginas = NULL;
    buf[112] = 2;
    EXIGIR(dsm_stats_reply_decodificar(buf, n, &leida, &recibida) == -1);
    buf[112] = 1; buf[DSM_STATS_RANGO_PREF + 104 + 34] = DSM_PAGINA_LIBRE | DSM_PAGINA_PUBLICA;
    EXIGIR(dsm_stats_reply_decodificar(buf, n, &leida, &recibida) == -1);
    dsm_pagina_consulta_t alta = {.gpn = 983039, .mascara = UINT64_C(1) << 59,
        .proc = (UINT32_C(58) << 16) | 1u, .reserva = (UINT32_C(59) << 16) | 7u,
        .hogar = 59, .poseedor = 59, .msi = DSM_MSI_M, .permiso = DSM_PAGE_PERMISO_ESCRIBIR,
        .banderas = DSM_PAGINA_PUBLICA, .copias = 1, .version = UINT32_MAX};
    respuesta.emisor = 59; respuesta.fichas[0].id = 59;
    respuesta.p = 1; respuesta.paginas = &alta; respuesta.mas = 0; respuesta.siguiente = 983040;
    EXIGIR(dsm_stats_reply_codificar(buf, sizeof buf, &cab, &respuesta, &n) == 0);
    EXIGIR(dsm_stats_reply_decodificar(buf, n, &leida, &recibida) == 0 &&
           recibida.paginas[0].mascara == (UINT64_C(1) << 59) &&
           recibida.paginas[0].proc == alta.proc && recibida.paginas[0].version == UINT32_MAX);
    free(recibida.paginas); recibida.paginas = NULL;
    respuesta.rango = 0; respuesta.p = 0; respuesta.paginas = NULL; respuesta.n = DSM_MAX_FICHAS;
    for (uint16_t i = 0; i < DSM_MAX_FICHAS; i++) respuesta.fichas[i].id = i;
    EXIGIR(dsm_stats_reply_codificar(buf, sizeof buf, &cab, &respuesta, &n) == 0);
    EXIGIR(dsm_stats_reply_decodificar(buf, n, &leida, &recibida) == 0 &&
           recibida.n == DSM_MAX_FICHAS && recibida.fichas[59].id == 59);
    buf[34] = 0; buf[35] = DSM_MAX_FICHAS + 1;
    EXIGIR(dsm_stats_reply_decodificar(buf, n, &leida, &recibida) == -1);
    puts("inventario codecs: legacy/publica PUB1, CAP coherente, rangos y errores sin ambiguedad");
    resultado = 0;
terminar:
    free(recibida.paginas);
    return resultado;
}

static int preparaciones(const char *ruta)
{
    int resultado = 1, inicializado = 0;
    dsm_config_t cfg;
    dsm_directorio_t dir;
    dsm_stats_reply_t filas = {0};
    EXIGIR(dsm_config_cargar(ruta, &cfg) == 0 && dsm_directorio_init(&dir, &cfg, 1) == 0);
    inicializado = 1;
    dsm_stats_request_t q = {.seleccion = DSM_STATS_INVENTARIO, .desde = 4, .limite = 4};
    dsm_region_info_t parcial;
    EXIGIR(dsm_directorio_preparar(&dir, 1, 99, 2, 2, 16, &parcial) == 0 && parcial.publica);
    EXIGIR(dir.npaginas == 0 && dsm_directorio_consulta_rango(&dir, &q, &filas) == 0 &&
           filas.p == 4 && filas.fichas[0].libres == 2 && !filas.mas && filas.siguiente == 8);
    for (size_t i = 0; i < 4; i++) {
        EXIGIR((filas.paginas[i].banderas & DSM_PAGINA_ESTADO) ==
               (i < 2 ? DSM_PAGINA_PREPARADA : DSM_PAGINA_LIBRE));
        if (i < 2) EXIGIR(filas.paginas[i].reserva == 1 && filas.paginas[i].orden == i + 2);
    }
    free(filas.paginas); filas.paginas = NULL;
    EXIGIR(dsm_directorio_cancelar(&dir, 1, 99) == 0);
    EXIGIR(dsm_directorio_consulta_rango(&dir, &q, &filas) == 0 && filas.fichas[0].libres == 4);
    for (size_t i = 0; i < 4; i++) EXIGIR(filas.paginas[i].banderas == DSM_PAGINA_LIBRE);
    free(filas.paginas); filas.paginas = NULL;
    EXIGIR(dsm_directorio_preparar(&dir, 2, 100, 0, 1, 16, &parcial) == 0);
    parcial.total_tramos = parcial.cantidad_tramos;
    dsm_region_info_t alterada = parcial; alterada.publica = 0;
    EXIGIR(dsm_directorio_confirmar(&dir, 2, 100, &alterada) == -1);
    EXIGIR(dsm_directorio_confirmar(&dir, 2, 100, &parcial) == 0);
    q.seleccion = DSM_STATS_COPIAS_RANGO;
    EXIGIR(dsm_directorio_consulta_rango(&dir, &q, &filas) == 0 && filas.p == 0);
    free(filas.paginas); filas.paginas = NULL;
    EXIGIR(dsm_directorio_instalar(&dir, &parcial) == 0);
    EXIGIR(dsm_directorio_consulta_rango(&dir, &q, &filas) == 0 && filas.p == 1 &&
           (filas.paginas[0].banderas & DSM_PAGINA_PUBLICA));
    EXIGIR(dsm_directorio_instalar(&dir, &alterada) == -1);
    puts("inventario preparado: bitmap ocupado sin descriptor, cancelar libera; donante sin ventana excluido, apertura lo incluye");
    resultado = 0;
terminar:
    free(filas.paginas);
    if (inicializado) dsm_directorio_cerrar(&dir);
    return resultado;
}

int main(void)
{
    alarm(60);
    FILE *config = tmpfile();
    struct api_proceso cliente = {0};
    dsm_context_t *ctx = NULL;
    dsm_reserva_id_t reserva = 0;
    dsm_reserva_id_t vinculada = 0;
    dsm_proc_id_t proc;
    void *ar;
    int abierto = 0, resultado = 1;
    char *texto = NULL;
    FILE *salida = NULL;
    size_t largo = 0;
    EXIGIR(config && fprintf(config, "page_size=4096\nmemoria_inicial=16384\nmax_nodos=3\n"
        "rpc_timeout_ms=500\nheartbeat_intervalo_ms=200\nheartbeat_fallos=5\n"
        "puerto_rango_inicio=27200\npuerto_rango_fin=27299\n"
        "nodo=0 creador 127.0.0.1 27290\nnodo=1 donante 127.0.0.1 27291\n"
        "nodo=2 ausente 127.0.0.1 27292\n") > 0 && fflush(config) == 0);
    char ruta[64]; snprintf(ruta, sizeof ruta, "/proc/self/fd/%d", fileno(config));
    EXIGIR(codecs() == 0 && preparaciones(ruta) == 0);
    EXIGIR(fflush(NULL) == 0);
    EXIGIR(api_iniciar_proceso(&cliente, ruta, 1) == 0 && api_operar(&cliente, API_INICIAR, DSM_OK) == 0);
    inyectar_servicio = 1;
    EXIGIR(dsm_iniciar(ruta, 0, &ctx) == DSM_OK && dsm_proc_abrir(ctx, &proc) == DSM_OK);
    for (unsigned i = 0; i < 150 && !api_tiene_traza(&cliente, "par 0 hello 1 welcome 1 estado 2"); i++) usleep(20000);
    EXIGIR(api_tiene_traza(&cliente, "par 0 hello 1 welcome 1 estado 2"));
    EXIGIR(dsm_reservar(ctx, proc, 6 * 4096 + 17, &reserva, &ar) == DSM_OK);
    dsm_pagina_info_t p[16];
    size_t n = 0;
    EXIGIR(dsm_consultar_paginas(ctx, DSM_HOGAR_TODOS, p, 16, &n) == DSM_ERR_PARCIAL && n == 8);
    unsigned publicas = 0, historicas = 0;
    for (size_t i = 0; i < n; i++) {
        if (p[i].reserva == reserva) { EXIGIR(p[i].publica && p[i].propietario == proc && p[i].creador == 0); publicas++; }
        if (p[i].reserva == reserva_servicio) { EXIGIR(!p[i].publica && !p[i].propietario && p[i].gpn == gpn_servicio); historicas++; }
    }
    EXIGIR(publicas == 7 && historicas == 1);
    uint64_t siguiente; int mas;
    EXIGIR(dsm_consultar_rango(ctx, 0, 1, 0, 16, p, &n, &siguiente, &mas) == DSM_OK && n == 8);
    unsigned invalidas = 0;
    for (size_t i = 0; i < n; i++) if (p[i].hogar == 1) {
        EXIGIR(p[i].memoria_libre == UINT32_MAX && p[i].permiso == 0 && p[i].propietario == proc); invalidas++;
    }
    EXIGIR(invalidas == 4);
    struct api_orden orden = {.operacion = API_ABRIR, .id = reserva_servicio};
    struct api_respuesta r;
    EXIGIR(api_ejecutar(&cliente, &orden, &r) == 0 && r.resultado == DSM_OK);
    abierto = 1; vinculada = reserva_servicio;
    orden = (struct api_orden){.operacion = API_ESCRIBIR, .bytes = 1, .datos = {0x35}};
    EXIGIR(api_ejecutar(&cliente, &orden, &r) == 0 && r.resultado == DSM_OK);
    EXIGIR(dsm_consultar_rango(ctx, 0, 0, gpn_servicio, 1, p, &n, &siguiente, &mas) == DSM_OK &&
           n == 1 && !p[0].publica && !p[0].propietario && p[0].proc == ((UINT32_C(1) << 16) | 1u));
    orden = (struct api_orden){.operacion = API_DESVINCULAR, .id = reserva_servicio};
    EXIGIR(api_ejecutar(&cliente, &orden, &r) == 0 && r.resultado == DSM_OK); abierto = 0;
    orden = (struct api_orden){.operacion = API_ABRIR, .id = reserva};
    EXIGIR(api_ejecutar(&cliente, &orden, &r) == 0 && r.resultado == DSM_OK);
    abierto = 1; vinculada = reserva;
    orden = (struct api_orden){.operacion = API_ESCRIBIR, .bytes = 1, .datos = {0x7c}};
    EXIGIR(api_ejecutar(&cliente, &orden, &r) == 0 && r.resultado == DSM_OK);
    EXIGIR(dsm_consultar_rango(ctx, 0, 0, gpn_servicio + 1, 1, p, &n, &siguiente, &mas) == DSM_OK &&
           n == 1 && p[0].propietario == proc && p[0].publica && p[0].proc == ((UINT32_C(1) << 16) | 1u));
    unsigned char dato;
    EXIGIR(dsm_leer(ctx, reserva, 0, &dato, 1) == DSM_OK && dato == 0x7c);
    EXIGIR(dsm_consultar_rango(ctx, 1, 0, 4, 2, p, &n, &siguiente, &mas) == DSM_OK && n == 2 && mas && siguiente == 6);
    EXIGIR(dsm_consultar_rango(ctx, 1, 0, siguiente, 2, p, &n, &siguiente, &mas) == DSM_OK && n == 2 && !mas && siguiente == 8);
    EXIGIR(dsm_consultar_rango(ctx, DSM_HOGAR_TODOS, 0, 0, 2, p, &n, &siguiente, &mas) == DSM_ERR_PARCIAL && n == 2 && mas);
    salida = open_memstream(&texto, &largo); EXIGIR(salida);
    char *estado[] = {"estado"};
    char *nodos[] = {"nodos"};
    char *paginas[] = {"paginas", "nodo", "1", "4", "1"};
    char *copias[] = {"copias", "local", "0", "16"};
    EXIGIR(consola_consultar(ctx, &(dsm_config_t){0}, 0, 1, estado, salida) == DSM_ERR_INVAL);
    dsm_config_t cfg; EXIGIR(dsm_config_cargar(ruta, &cfg) == 0);
    EXIGIR(consola_consultar(ctx, &cfg, 0, 1, estado, salida) == 1 &&
           consola_consultar(ctx, &cfg, 0, 1, nodos, salida) == 1 &&
           consola_consultar(ctx, &cfg, 0, 5, paginas, salida) == 1 &&
           consola_consultar(ctx, &cfg, 0, 4, copias, salida) == 1);
    EXIGIR(fclose(salida) == 0); salida = NULL;
    EXIGIR(strstr(texto, "paginas_totales=8 paginas_ocupadas=8 paginas_libres=0") &&
           strstr(texto, "ocupadas=? libres=?") && strstr(texto, "PAGINAS tabla=HOGAR desde=4 filas=1") &&
           strstr(texto, "libre_hogar=?") && strstr(texto, "procedencia=PUBLICA"));
    fputs(texto, stdout);
    orden = (struct api_orden){.operacion = API_DESVINCULAR, .id = reserva};
    EXIGIR(api_ejecutar(&cliente, &orden, &r) == 0 && r.resultado == DSM_OK); abierto = 0;
    EXIGIR(dsm_liberar(ctx, reserva) == DSM_OK); reserva = 0;
    dsm_nodo_info_t nodo;
    EXIGIR(dsm_consultar_nodo(ctx, 1, &nodo) == DSM_OK && nodo.respondio && nodo.libres == 4);
    EXIGIR(dsm_consultar_rango(ctx, 1, 0, 4, 4, p, &n, &siguiente, &mas) == DSM_OK && n == 4);
    for (size_t i = 0; i < n; i++) EXIGIR(p[i].asignacion == DSM_ASIGNACION_LIBRE && !p[i].propietario);
    EXIGIR(api_operar(&cliente, API_CERRAR, DSM_OK) == 0);
    EXIGIR(dsm_cerrar(ctx) == DSM_OK); ctx = NULL;
    puts("inventario publico: propietario estable tras store remoto, servicio sin propietario ficticio, rangos y recuperacion total");
    resultado = 0;
terminar:
    if (salida) fclose(salida);
    free(texto);
    if (abierto) {
        struct api_orden cierre = {.operacion = API_DESVINCULAR, .id = vinculada}; struct api_respuesta r;
        (void)api_ejecutar(&cliente, &cierre, &r);
    }
    if (ctx && reserva) (void)dsm_liberar(ctx, reserva);
    if (ctx && dsm_cerrar(ctx) != DSM_OK) resultado = 1;
    if (api_detener_proceso(&cliente) != 0) resultado = 1;
    if (config) fclose(config);
    return resultado;
}
