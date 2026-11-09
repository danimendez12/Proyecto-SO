#include "directory.h"
#include "network.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct trabajo {
    pthread_mutex_t mu;
    pthread_cond_t cv_escucha;
    pthread_cond_t cv_reserva;
    pthread_cond_t cv_hecho;
    int escuchando;
    int reserva_lista;
    int comparado;
    uint32_t reserva_id;
    int ok0;
    int ok1;
    void *ubicacion0;
    char texto0[1024];
    char texto1[1024];
    const dsm_config_t *cfg;
};

static int fallar(const char *mensaje)
{
    fprintf(stderr, "fallo: %s\n", mensaje);
    return 1;
}

static void avisar_escucha(struct trabajo *tx, int estado)
{
    pthread_mutex_lock(&tx->mu);
    tx->escuchando = estado;
    pthread_cond_signal(&tx->cv_escucha);
    pthread_mutex_unlock(&tx->mu);
}

static void avisar_reserva(struct trabajo *tx, int estado, uint32_t id)
{
    pthread_mutex_lock(&tx->mu);
    tx->reserva_id = id;
    tx->reserva_lista = estado;
    pthread_cond_signal(&tx->cv_reserva);
    pthread_mutex_unlock(&tx->mu);
}

static void avisar_hecho(struct trabajo *tx)
{
    pthread_mutex_lock(&tx->mu);
    tx->comparado = 1;
    pthread_cond_signal(&tx->cv_hecho);
    pthread_mutex_unlock(&tx->mu);
}

static void *nodo_cero(void *arg)
{
    struct trabajo *tx = arg;
    const dsm_nodo_cfg_t *par = dsm_config_nodo(tx->cfg, 1);
    pthread_mutex_lock(&tx->mu);
    while (tx->escuchando == 0) {
        pthread_cond_wait(&tx->cv_escucha, &tx->mu);
    }
    int escuchando = tx->escuchando;
    pthread_mutex_unlock(&tx->mu);
    if (escuchando < 0 || par == NULL) {
        avisar_reserva(tx, -1, 0);
        return NULL;
    }

    int conn = -1;
    if (dsm_conectar(par->ipv4_host, par->puerto, &conn) != 0) {
        avisar_reserva(tx, -1, 0);
        return NULL;
    }
    dsm_alta_resultado_t alta;
    if (dsm_alta_par(conn, tx->cfg, 0, 1, &alta) != 0) {
        close(conn);
        avisar_reserva(tx, -1, 0);
        return NULL;
    }

    dsm_directorio_t dir;
    dsm_pagina_desc_t desc;
    memset(&dir, 0, sizeof dir);
    if (dsm_directorio_init(&dir, tx->cfg, 0) != 0) {
        close(conn);
        avisar_reserva(tx, -1, 0);
        return NULL;
    }
    if (dsm_directorio_reservar_pagina(&dir, &desc) != 0) {
        dsm_directorio_cerrar(&dir);
        close(conn);
        avisar_reserva(tx, -1, 0);
        return NULL;
    }
    uint8_t *gestor = dsm_directorio_c_r(&dir);
    if (gestor == NULL) {
        dsm_directorio_cerrar(&dir);
        close(conn);
        avisar_reserva(tx, -1, 0);
        return NULL;
    }
    gestor[0] = 0xab;
    pthread_mutex_lock(&tx->mu);
    tx->ubicacion0 = desc.ubicacion_local;
    pthread_mutex_unlock(&tx->mu);
    avisar_reserva(tx, 1, desc.reserva);

    uint32_t seq = alta.siguiente_msg;
    int publicado = dsm_directorio_publicar(&dir, conn, &seq);
    snprintf(tx->texto0, sizeof tx->texto0,
             "nodo 0 reservas_globales %u siguiente %u libres %u gpn %llu hogar %u poseedor %u "
             "reserva %u msi %u version %u orden %u marca_local %02x",
             dir.reservas_globales, dir.siguiente_local, dir.paginas_libres,
             (unsigned long long)desc.gpn, desc.hogar, desc.poseedor, desc.reserva, desc.msi,
             desc.version, desc.orden, gestor[0]);
    if (publicado == 0 && dir.reservas_globales == 1 && dir.siguiente_local == 2 &&
        dir.paginas_libres == 255 && desc.gpn == 0 && desc.hogar == 0 && desc.poseedor == 0 &&
        desc.reserva == 1 && desc.msi == DSM_MSI_M && desc.version == 0 && desc.orden == 0 &&
        desc.ubicacion_local != NULL) {
        tx->ok0 = 1;
    }
    pthread_mutex_lock(&tx->mu);
    while (!tx->comparado) {
        pthread_cond_wait(&tx->cv_hecho, &tx->mu);
    }
    pthread_mutex_unlock(&tx->mu);
    dsm_directorio_cerrar(&dir);
    close(conn);
    return NULL;
}

static void *nodo_uno(void *arg)
{
    struct trabajo *tx = arg;
    const dsm_nodo_cfg_t *yo = dsm_config_nodo(tx->cfg, 1);
    int escucha = -1;
    if (yo == NULL || dsm_escuchar(yo->ipv4_host, yo->puerto, &escucha) != 0) {
        avisar_escucha(tx, -1);
        avisar_reserva(tx, -1, 0);
        return NULL;
    }
    avisar_escucha(tx, 1);

    int conn = -1;
    if (dsm_aceptar(escucha, &conn) != 0) {
        close(escucha);
        avisar_reserva(tx, -1, 0);
        return NULL;
    }
    close(escucha);
    dsm_alta_resultado_t alta;
    if (dsm_alta_par(conn, tx->cfg, 1, 0, &alta) != 0) {
        close(conn);
        avisar_reserva(tx, -1, 0);
        return NULL;
    }

    pthread_mutex_lock(&tx->mu);
    while (tx->reserva_lista == 0) {
        pthread_cond_wait(&tx->cv_reserva, &tx->mu);
    }
    int lista = tx->reserva_lista;
    uint32_t reserva = tx->reserva_id;
    void *ubicacion0 = tx->ubicacion0;
    pthread_mutex_unlock(&tx->mu);
    if (lista < 0) {
        close(conn);
        return NULL;
    }

    dsm_directorio_t dir;
    dsm_cabecera_t cab;
    dsm_region_info_t info;
    memset(&dir, 0, sizeof dir);
    uint32_t seq = alta.siguiente_msg;
    if (dsm_directorio_init(&dir, tx->cfg, 1) != 0 ||
        dsm_directorio_traer(&dir, conn, reserva, &seq, &cab, &info) != 0) {
        dsm_directorio_cerrar(&dir);
        close(conn);
        avisar_hecho(tx);
        return NULL;
    }
    uint8_t marca = 0xff;
    uint8_t *gestor = dsm_directorio_c_r(&dir);
    if (gestor != NULL) {
        marca = gestor[0];
    }
    dsm_pagina_desc_t desc = dir.paginas[0];
    int distintas = ubicacion0 != NULL && desc.ubicacion_local != NULL && ubicacion0 != desc.ubicacion_local;

    snprintf(tx->texto1, sizeof tx->texto1,
             "nodo 1 tipo 0x%02x longitud %u respuesta %d reserva %u tramos %u gpn %llu\n"
             "nodo 1 reservas_globales %u siguiente %u libres %u paginas %zu gpn %llu hogar %u "
             "poseedor %u reserva %u msi %u version %u orden %u\n"
             "nodo 1 marca_local %02x\n"
             "ubicaciones_distintas %d",
             cab.tipo, cab.longitud, (cab.banderas & DSM_FLAG_RESPUESTA) != 0, info.reserva_id,
             info.cantidad_tramos, (unsigned long long)info.tramos[0].gpn_inicial, dir.reservas_globales,
             dir.siguiente_local, dir.paginas_libres, dir.npaginas, (unsigned long long)desc.gpn,
             desc.hogar, desc.poseedor, desc.reserva, desc.msi, desc.version, desc.orden, marca,
             distintas);

    if (cab.tipo == DSM_TIPO_REGION_INFO && cab.longitud == 76 &&
        (cab.banderas & DSM_FLAG_RESPUESTA) != 0 && info.reserva_id == reserva &&
        info.cantidad_tramos == 1 && info.total_paginas == 1 && info.tamano_bytes == 4096 &&
        info.tramos[0].gpn_inicial == 0 && info.tramos[0].cantidad == 1 && dir.reservas_globales == 0 &&
        dir.siguiente_local == 1 && dir.paginas_libres == 256 && dir.npaginas == 1 && desc.gpn == 0 &&
        desc.hogar == 0 && desc.poseedor == DSM_POSEEDOR_NINGUNO && desc.reserva == reserva &&
        desc.msi == DSM_MSI_I && desc.version == 0 && desc.orden == 0 && marca == 0 && distintas) {
        tx->ok1 = 1;
    }
    dsm_directorio_cerrar(&dir);
    close(conn);
    avisar_hecho(tx);
    return NULL;
}

int main(void)
{
    alarm(20);
    dsm_config_t cfg;
    if (dsm_config_cargar("config/lab.ini", &cfg) != 0) {
        return fallar("config");
    }
    struct trabajo tx = {
        .mu = PTHREAD_MUTEX_INITIALIZER,
        .cv_escucha = PTHREAD_COND_INITIALIZER,
        .cv_reserva = PTHREAD_COND_INITIALIZER,
        .cv_hecho = PTHREAD_COND_INITIALIZER,
        .cfg = &cfg,
    };
    pthread_t h0;
    pthread_t h1;
    if (pthread_create(&h0, NULL, nodo_cero, &tx) != 0 || pthread_create(&h1, NULL, nodo_uno, &tx) != 0) {
        return fallar("hilos");
    }
    pthread_join(h0, NULL);
    pthread_join(h1, NULL);
    if (!tx.ok0 || !tx.ok1) {
        if (tx.texto0[0] != '\0') {
            fprintf(stderr, "%s\n", tx.texto0);
        }
        if (tx.texto1[0] != '\0') {
            fprintf(stderr, "%s\n", tx.texto1);
        }
        return fallar("REGION_INFO");
    }
    printf("%s\n%s\n", tx.texto0, tx.texto1);
    alarm(0);
    return 0;
}
