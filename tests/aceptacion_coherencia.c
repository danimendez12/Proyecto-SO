#include "directory.h"
#include "network.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char hola[] = "Hola TEC";
static const char adios[] = "Adios TEC";

struct nodo {
    uint16_t id;
    const dsm_config_t *cfg;
    dsm_directorio_t dir;
    dsm_red_t *red;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int lista;
    int fallo;
    uint32_t espera_id;
    uint8_t imagen[4096];
    dsm_page_data_t datos;
    int vivo;
};

static int fallar(const char *mensaje)
{
    fprintf(stderr, "fallo: %s (%s)\n", mensaje, strerror(errno));
    return 1;
}

static void armar_cfg(dsm_config_t *cfg, uint16_t n, uint16_t puerto, uint32_t latido, uint32_t rpc)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->page_size = 4096;
    cfg->memoria_inicial = 4096;
    cfg->max_nodos = n;
    cfg->max_hilos = 4;
    cfg->heartbeat_intervalo_ms = latido;
    cfg->heartbeat_fallos = 3;
    cfg->rpc_timeout_ms = rpc;
    cfg->puerto_rango_inicio = 27200;
    cfg->puerto_rango_fin = 27299;
    cfg->cantidad_nodos = n;
    for (uint16_t i = 0; i < n; i++) {
        cfg->nodos[i].id = i;
        cfg->nodos[i].puerto = (uint16_t)(puerto + i);
        cfg->nodos[i].ipv4_host = 0x7f000001u;
        cfg->nodos[i].memoria = 4096;
        snprintf(cfg->nodos[i].nombre, sizeof cfg->nodos[i].nombre, "nodo-%u", i);
    }
}

static int enviar_nodo(void *ctx, uint16_t destino, const uint8_t *buf, size_t n)
{
    struct nodo *s = ctx;
    return dsm_red_enviar(s->red, destino, buf, n);
}

static uint32_t id_nodo(void *ctx)
{
    return dsm_red_id(((struct nodo *)ctx)->red);
}

static void estado_nodo(void *ctx, uint16_t par, uint8_t estado)
{
    struct nodo *s = ctx;
    if (estado == DSM_NODO_FUERA) {
        dsm_directorio_par_fuera(&s->dir, par);
    } else if (estado == DSM_NODO_EN_LINEA) {
        dsm_directorio_par_linea(&s->dir, par);
    }
}

static int despachar(void *ctx, uint16_t par, const dsm_cabecera_t *cab, const uint8_t *buf, size_t n)
{
    struct nodo *s = ctx;
    if (cab->origen != par || cab->destino != s->id) {
        errno = EPROTO;
        return -1;
    }
    if ((cab->banderas & DSM_FLAG_RESPUESTA) != 0 && cab->id_corr != 0) {
        if (dsm_directorio_completar(&s->dir, par, buf, n) == 0) {
            return 0;
        }
        if (cab->tipo == DSM_TIPO_PAGE_DATA && cab->estado == DSM_ESTADO_OK) {
            dsm_cabecera_t copia;
            dsm_page_data_t datos;
            if (dsm_page_data_decodificar(buf, n, &copia, &datos) != 0) {
                errno = EPROTO;
                return -1;
            }
            if (datos.hogar == s->id) {
                return 0;
            }
            if (datos.page_nbytes != 0) {
                if (datos.page_nbytes > sizeof s->imagen) {
                    errno = EPROTO;
                    return -1;
                }
                memcpy(s->imagen, datos.datos, datos.page_nbytes);
                datos.datos = s->imagen;
            }
            if (dsm_directorio_aplicar_pagina(&s->dir, &datos) != 0) {
                return -1;
            }
            pthread_mutex_lock(&s->mu);
            if (s->espera_id == cab->id_corr && !s->lista) {
                s->datos = datos;
                s->lista = 1;
                s->fallo = 0;
                pthread_cond_signal(&s->cv);
            }
            pthread_mutex_unlock(&s->mu);
            return 0;
        }
        if (cab->tipo == DSM_TIPO_PAGE_DATA) {
            pthread_mutex_lock(&s->mu);
            if (s->espera_id == cab->id_corr && !s->lista) {
                s->lista = 1;
                s->fallo = cab->estado != 0 ? cab->estado : EPROTO;
                pthread_cond_signal(&s->cv);
            }
            pthread_mutex_unlock(&s->mu);
        }
        return 0;
    }
    if (cab->banderas != 0 || cab->id_corr != 0 || cab->estado != DSM_ESTADO_OK) {
        errno = EPROTO;
        return -1;
    }
    if (cab->tipo == DSM_TIPO_PAGE_REQUEST || cab->tipo == DSM_TIPO_INVALIDATE ||
        cab->tipo == DSM_TIPO_PAGE_DOWNGRADE) {
        return dsm_directorio_ofrecer(&s->dir, par, buf, n);
    }
    if (cab->tipo == DSM_TIPO_REGION_LOOKUP) {
        dsm_region_lookup_t consulta;
        dsm_cabecera_t pedido;
        uint8_t respuesta[512];
        size_t bytes = 0;
        if (dsm_region_lookup_decodificar(buf, n, &pedido, &consulta) != 0 ||
            dsm_directorio_codificar_info(&s->dir, &pedido, &consulta, dsm_red_id(s->red), respuesta,
                                          sizeof respuesta, &bytes) != 0) {
            return -1;
        }
        return dsm_red_enviar(s->red, par, respuesta, bytes);
    }
    errno = EPROTO;
    return -1;
}

static int subir(struct nodo *s)
{
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) != 0 || pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) != 0 ||
        pthread_cond_init(&s->cv, &attr) != 0) {
        pthread_condattr_destroy(&attr);
        return -1;
    }
    pthread_condattr_destroy(&attr);
    if (pthread_mutex_init(&s->mu, NULL) != 0 || dsm_directorio_init(&s->dir, s->cfg, s->id) != 0 ||
        dsm_directorio_activar(&s->dir, enviar_nodo, id_nodo, s) != 0) {
        return -1;
    }
    if (dsm_red_iniciar(&s->red, s->cfg, s->id, despachar, s) != 0) {
        dsm_directorio_desactivar(&s->dir);
        dsm_directorio_cerrar(&s->dir);
        pthread_cond_destroy(&s->cv);
        pthread_mutex_destroy(&s->mu);
        return -1;
    }
    dsm_red_observar(s->red, estado_nodo, s);
    s->vivo = 1;
    return 0;
}

static void bajar(struct nodo *s)
{
    if (!s->vivo) {
        return;
    }
    dsm_directorio_desactivar(&s->dir);
    if (s->red != NULL) {
        dsm_red_cerrar(s->red);
        s->red = NULL;
    }
    dsm_directorio_cerrar(&s->dir);
    pthread_cond_destroy(&s->cv);
    pthread_mutex_destroy(&s->mu);
    s->vivo = 0;
}

static int esperar_pares(struct nodo *s)
{
    for (uint16_t i = 0; i < s->cfg->cantidad_nodos; i++) {
        if (i != s->id && dsm_red_esperar(s->red, i) != 0) {
            return -1;
        }
    }
    return 0;
}

static int plazo(struct nodo *s, struct timespec *ts)
{
    if (clock_gettime(CLOCK_MONOTONIC, ts) != 0) {
        return -1;
    }
    uint32_t ms = s->cfg->rpc_timeout_ms;
    ts->tv_sec += ms / 1000u;
    ts->tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
    return 0;
}

static int pedir(struct nodo *s, uint16_t hogar, uint64_t gpn, uint8_t acceso, uint8_t msi)
{
    dsm_cabecera_t cab;
    memset(&cab, 0, sizeof cab);
    cab.version = DSM_PROTO_VERSION;
    cab.tipo = DSM_TIPO_PAGE_REQUEST;
    cab.origen = s->id;
    cab.destino = hogar;
    cab.id_msg = dsm_red_id(s->red);
    cab.estado = DSM_ESTADO_OK;
    dsm_page_request_t msg = {
        .gpn = gpn,
        .proc_id = ((uint32_t)s->id << 16) | 1u,
        .acceso = acceso,
        .msi = msi,
    };
    uint8_t buf[DSM_PAGE_REQUEST_BYTES];
    size_t n = 0;
    if (dsm_page_request_codificar(buf, sizeof buf, &cab, &msg, &n) != 0) {
        errno = EPROTO;
        return -1;
    }
    struct timespec ts;
    pthread_mutex_lock(&s->mu);
    s->espera_id = cab.id_msg;
    s->lista = 0;
    s->fallo = 0;
    pthread_mutex_unlock(&s->mu);
    if (dsm_red_enviar(s->red, hogar, buf, n) != 0 || plazo(s, &ts) != 0) {
        return -1;
    }
    pthread_mutex_lock(&s->mu);
    int error = 0;
    while (!s->lista && error == 0) {
        error = pthread_cond_timedwait(&s->cv, &s->mu, &ts);
    }
    int fallo_pagina = s->fallo;
    int lista = s->lista;
    pthread_mutex_unlock(&s->mu);
    if (!lista || fallo_pagina != 0) {
        errno = error != 0 ? error : EPROTO;
        return -1;
    }
    return 0;
}

static int abrir_remota(struct nodo *s, uint32_t reserva, uint64_t gpn, uint16_t hogar)
{
    dsm_region_info_t info;
    memset(&info, 0, sizeof info);
    info.reserva_id = reserva;
    info.total_tramos = 1;
    info.tamano_bytes = s->cfg->page_size;
    info.cantidad_tramos = 1;
    info.total_paginas = 1;
    info.tramos[0].cantidad = 1;
    info.tramos[0].gpn_inicial = gpn;
    (void)hogar;
    return dsm_directorio_instalar(&s->dir, &info);
}

static int texto_igual(const uint8_t *pagina, const char *texto)
{
    return memcmp(pagina, texto, strlen(texto) + 1) == 0;
}

static int probar_formato(void)
{
    uint8_t buf[128];
    dsm_cabecera_t cab;
    memset(&cab, 0, sizeof cab);
    cab.version = DSM_PROTO_VERSION;
    cab.tipo = DSM_TIPO_PAGE_DOWNGRADE;
    cab.origen = 2;
    cab.destino = 0;
    cab.id_msg = 9;
    dsm_downgrade_t baja = {.gpn = 0x1122334455667788ull, .id_solicitud = 4, .solicitante = 1,
                            .motivo = DSM_MOTIVO_A_I};
    size_t n = 0;
    if (dsm_downgrade_codificar(buf, sizeof buf, &cab, &baja, &n) != 0 || n != 48 || buf[46] != 1) {
        return fallar("PAGE_DOWNGRADE");
    }
    dsm_downgrade_t vuelta;
    dsm_cabecera_t cab2;
    if (dsm_downgrade_decodificar(buf, n, &cab2, &vuelta) != 0 || vuelta.gpn != baja.gpn ||
        vuelta.id_solicitud != 4 || vuelta.solicitante != 1 || buf[32] != 0x11) {
        return fallar("PAGE_DOWNGRADE big-endian");
    }
    baja.motivo = 2;
    if (dsm_downgrade_codificar(buf, sizeof buf, &cab, &baja, &n) == 0) {
        return fallar("motivo invalido");
    }
    baja.motivo = 0;
    baja.id_solicitud = 0;
    if (dsm_downgrade_codificar(buf, sizeof buf, &cab, &baja, &n) == 0) {
        return fallar("id de solicitud vacio");
    }

    memset(&cab, 0, sizeof cab);
    cab.version = DSM_PROTO_VERSION;
    cab.tipo = DSM_TIPO_INVALIDATE;
    cab.origen = 0;
    cab.destino = 1;
    cab.id_msg = 3;
    dsm_invalidate_t inv = {.gpn = 7, .id_solicitud = 3, .escritor = 2};
    if (dsm_invalidate_codificar(buf, sizeof buf, &cab, &inv, &n) != 0) {
        return fallar("INVALIDATE");
    }
    cab.tipo = DSM_TIPO_INVALIDATE_ACK;
    cab.banderas = DSM_FLAG_RESPUESTA;
    cab.id_corr = 3;
    cab.id_msg = 8;
    dsm_invalidate_ack_t ack = {.gpn = 7, .id_solicitud = 3, .resultado = DSM_INVALIDA_YA_I};
    if (dsm_invalidate_ack_codificar(buf, sizeof buf, &cab, &ack, &n) != 0) {
        return fallar("INVALIDATE_ACK");
    }
    dsm_invalidate_ack_t ack2;
    if (dsm_invalidate_ack_decodificar(buf, n, &cab2, &ack2) != 0 || ack2.resultado != 1 ||
        cab2.id_corr != 3) {
        return fallar("INVALIDATE_ACK decodificado");
    }

    memset(&cab, 0, sizeof cab);
    cab.version = DSM_PROTO_VERSION;
    cab.tipo = DSM_TIPO_NODE_BYE;
    cab.origen = 1;
    cab.destino = 2;
    cab.id_msg = 5;
    dsm_aviso_t aviso = {.node_id = 1, .razon = 2};
    if (dsm_aviso_codificar(buf, sizeof buf, &cab, &aviso, &n) != 0 || n != 40) {
        return fallar("NODE_BYE");
    }
    cab.tipo = DSM_TIPO_NODE_DOWN_HINT;
    if (dsm_aviso_codificar(buf, sizeof buf, &cab, &aviso, &n) != 0 ||
        dsm_aviso_decodificar(buf, n, &cab2, &aviso) != 0 || aviso.node_id != 1 ||
        cab2.tipo != DSM_TIPO_NODE_DOWN_HINT) {
        return fallar("NODE_DOWN_HINT");
    }
    puts("formato MSI e incorporacion");
    return 0;
}

struct pedido_args {
    struct nodo *nodo;
    uint16_t hogar;
    uint64_t gpn;
    int rc;
};

static void *pedir_escritura(void *arg)
{
    struct pedido_args *p = arg;
    p->rc = pedir(p->nodo, p->hogar, p->gpn, DSM_ACCESO_ESCRITURA, DSM_MSI_S);
    return NULL;
}

static int escenario_tres(void)
{
    dsm_config_t cfg;
    armar_cfg(&cfg, 3, 27270, 100, 2000);
    struct nodo nodos[3];
    memset(nodos, 0, sizeof nodos);
    for (uint16_t i = 0; i < 3; i++) {
        nodos[i].id = i;
        nodos[i].cfg = &cfg;
        if (subir(&nodos[i]) != 0) {
            return fallar("no arranca el nodo");
        }
    }
    int rc = 1;
    for (uint16_t i = 0; i < 3; i++) {
        if (esperar_pares(&nodos[i]) != 0) {
            fallar("alta entre tres nodos");
            goto salir;
        }
    }
    dsm_pagina_desc_t creada;
    if (dsm_directorio_reservar_pagina(&nodos[2].dir, &creada) != 0) {
        fallar("reserva del hogar 2");
        goto salir;
    }
    uint8_t *cr = dsm_directorio_c_r_reserva(&nodos[2].dir, creada.reserva);
    if (cr == NULL) {
        fallar("vista del hogar");
        goto salir;
    }
    memcpy(cr, hola, sizeof hola);
    for (uint16_t i = 0; i < 2; i++) {
        if (abrir_remota(&nodos[i], creada.reserva, creada.gpn, 2) != 0 ||
            pedir(&nodos[i], 2, creada.gpn, DSM_ACCESO_LECTURA, DSM_MSI_I) != 0 ||
            !texto_igual(nodos[i].imagen, hola)) {
            fallar("lectura inicial desde el hogar 2");
            goto salir;
        }
    }
    dsm_pagina_desc_t hogar;
    if (dsm_directorio_describir(&nodos[2].dir, creada.gpn, &hogar) != 0 || hogar.msi != DSM_MSI_S ||
        hogar.version != 1 || hogar.poseedor != DSM_POSEEDOR_NINGUNO ||
        (hogar.mascara_copias & 0x7u) != 0x7u) {
        fprintf(stderr, "directorio hogar msi %u version %u poseedor %u mascara %llx\n", hogar.msi,
                hogar.version, hogar.poseedor, (unsigned long long)hogar.mascara_copias);
        fallar("copias Shared de tres nodos");
        goto salir;
    }

    dsm_directorio_fijar_retardo(&nodos[1].dir, 600);
    if (pedir(&nodos[0], 2, creada.gpn, DSM_ACCESO_ESCRITURA, DSM_MSI_S) != 0) {
        fallar("concesion M con invalidacion demorada");
        goto salir;
    }
    uint8_t estado = DSM_NODO_VACIO;
    if (dsm_red_estado(nodos[0].red, 2, &estado) != 0 || estado != DSM_NODO_EN_LINEA) {
        fprintf(stderr, "estado del hogar visto por el escritor: %u\n", estado);
        fallar("el receptor se bloqueo durante la campana");
        goto salir;
    }
    uint8_t *cr0 = dsm_directorio_c_r_reserva(&nodos[0].dir, creada.reserva);
    if (cr0 == NULL) {
        fallar("vista del escritor");
        goto salir;
    }
    memcpy(cr0, adios, sizeof adios);
    if (dsm_directorio_describir(&nodos[2].dir, creada.gpn, &hogar) != 0 || hogar.msi != DSM_MSI_M ||
        hogar.poseedor != 0 || hogar.mascara_copias != 1u || hogar.version != 2) {
        fprintf(stderr, "tras M: msi %u poseedor %u version %u mascara %llx\n", hogar.msi, hogar.poseedor,
                hogar.version, (unsigned long long)hogar.mascara_copias);
        fallar("exclusividad de un solo poseedor");
        goto salir;
    }
    dsm_pagina_desc_t local1;
    if (dsm_directorio_describir(&nodos[1].dir, creada.gpn, &local1) != 0 || local1.msi != DSM_MSI_I) {
        fallar("la copia Shared no quedo invalidada");
        goto salir;
    }

    if (pedir(&nodos[1], 2, creada.gpn, DSM_ACCESO_LECTURA, DSM_MSI_I) != 0 ||
        !texto_igual(nodos[1].imagen, adios)) {
        fallar("no se recupero Adios TEC desde el poseedor remoto");
        goto salir;
    }
    dsm_pagina_desc_t tras_lectura = {0};
    int hogar_al_dia = 0;
    for (int i = 0; i < 50; i++) {
        if (dsm_directorio_describir(&nodos[2].dir, creada.gpn, &tras_lectura) == 0 &&
            tras_lectura.msi == DSM_MSI_S && tras_lectura.version >= 3) {
            hogar_al_dia = 1;
            break;
        }
        usleep(20000);
    }
    uint8_t recuperada[4096];
    if (!hogar_al_dia ||
        dsm_directorio_copiar(&nodos[2].dir, creada.gpn, recuperada, sizeof recuperada, &tras_lectura) !=
            0 ||
        !texto_igual(recuperada, adios)) {
        fprintf(stderr, "hogar msi %u version %u\n", tras_lectura.msi, tras_lectura.version);
        fallar("el hogar no guardo la version actualizada");
        goto salir;
    }

    dsm_directorio_fijar_retardo(&nodos[1].dir, 0);
    struct pedido_args a0 = {.nodo = &nodos[0], .hogar = 2, .gpn = creada.gpn};
    struct pedido_args a1 = {.nodo = &nodos[1], .hogar = 2, .gpn = creada.gpn};
    pthread_t h0;
    pthread_t h1;
    if (pthread_create(&h0, NULL, pedir_escritura, &a0) != 0 ||
        pthread_create(&h1, NULL, pedir_escritura, &a1) != 0) {
        fallar("hilos de escritura");
        goto salir;
    }
    pthread_join(h0, NULL);
    pthread_join(h1, NULL);
    if (a0.rc != 0 || a1.rc != 0 || dsm_directorio_describir(&nodos[2].dir, creada.gpn, &hogar) != 0 ||
        hogar.msi != DSM_MSI_M || (hogar.mascara_copias != 1u && hogar.mascara_copias != 2u)) {
        fprintf(stderr, "carrera rc %d %d msi %u mascara %llx\n", a0.rc, a1.rc, hogar.msi,
                (unsigned long long)hogar.mascara_copias);
        fallar("dos escritores obtuvieron M");
        goto salir;
    }
    printf("MSI con hogar 2: S en 0,1,2; M exclusivo; Adios TEC recuperado; carrera %u\n",
           hogar.poseedor);
    rc = 0;
salir:
    for (uint16_t i = 0; i < 3; i++) {
        bajar(&nodos[i]);
    }
    return rc;
}

static int escenario_tarde(void)
{
    dsm_config_t cfg;
    armar_cfg(&cfg, 2, 27280, 100, 1000);
    struct nodo mayor = {.id = 1, .cfg = &cfg};
    struct nodo menor = {.id = 0, .cfg = &cfg};
    if (subir(&mayor) != 0) {
        return fallar("nodo tardio mayor");
    }
    usleep(150000);
    if (subir(&menor) != 0 || esperar_pares(&menor) != 0 || esperar_pares(&mayor) != 0) {
        bajar(&menor);
        bajar(&mayor);
        return fallar("el nodo que arranca despues no entra");
    }
    uint8_t estado = DSM_NODO_VACIO;
    int rc = dsm_red_estado(mayor.red, 0, &estado);
    bajar(&menor);
    uint8_t fuera = DSM_NODO_EN_LINEA;
    for (int i = 0; i < 50 && fuera != DSM_NODO_FUERA; i++) {
        if (dsm_red_estado(mayor.red, 0, &fuera) != 0) {
            break;
        }
        usleep(20000);
    }
    if (rc != 0 || estado != DSM_NODO_EN_LINEA || fuera != DSM_NODO_FUERA) {
        fprintf(stderr, "alta %u baja %u\n", estado, fuera);
        bajar(&mayor);
        return fallar("no se detecto la conexion perdida");
    }
    memset(&menor, 0, sizeof menor);
    menor.id = 0;
    menor.cfg = &cfg;
    if (subir(&menor) != 0 || esperar_pares(&menor) != 0 || esperar_pares(&mayor) != 0) {
        bajar(&menor);
        bajar(&mayor);
        return fallar("reconexion");
    }
    estado = DSM_NODO_VACIO;
    rc = dsm_red_estado(mayor.red, 0, &estado);
    bajar(&menor);
    bajar(&mayor);
    if (rc != 0 || estado != DSM_NODO_EN_LINEA) {
        return fallar("el nodo reincorporado no volvio a linea");
    }
    puts("latido, baja y reincorporacion en 127.0.0.1");
    return 0;
}

int main(void)
{
    alarm(25);
    if (probar_formato() != 0 || escenario_tres() != 0 || escenario_tarde() != 0) {
        return 1;
    }
    alarm(0);
    return 0;
}
