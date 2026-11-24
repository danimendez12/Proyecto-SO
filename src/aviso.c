#include "aviso.h"

#include "dsm.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

enum { AVISOS_MAX = 128 };

struct aviso {
    uint16_t autor;
    uint16_t largo;
    char clave[DSM_ANUNCIO_CLAVE];
    uint8_t datos[DSM_ANUNCIO_DATOS];
};

static struct aviso tabla[AVISOS_MAX];
static size_t usados;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint32_t siguiente_id = 1;

static int misma(const struct aviso *aviso, uint16_t autor, const char *clave,
                 const uint8_t *datos, size_t n)
{
    return aviso->autor == autor && strcmp(aviso->clave, clave) == 0 && aviso->largo == n &&
           (n == 0 || memcmp(aviso->datos, datos, n) == 0);
}

static void difundir(dsm_coherencia_t *coherencia, uint16_t autor, const char *clave,
                     const void *datos, size_t n)
{
    uint16_t destinos[DSM_CONFIG_MAX_NODOS];
    uint16_t cantidad = 0;
    if (dsm_coherencia_destinos(coherencia, destinos, DSM_CONFIG_MAX_NODOS, &cantidad) != 0) {
        return;
    }
    uint16_t origen = dsm_coherencia_nodo(coherencia);
    for (uint16_t i = 0; i < cantidad; i++) {
        dsm_cabecera_t cab = {
            .version = DSM_PROTO_VERSION,
            .tipo = DSM_TIPO_ANUNCIO,
            .origen = origen,
            .destino = destinos[i],
            .id_msg = atomic_fetch_add_explicit(&siguiente_id, 1, memory_order_relaxed),
        };
        if (cab.id_msg == 0) {
            cab.id_msg = atomic_fetch_add_explicit(&siguiente_id, 1, memory_order_relaxed);
        }
        uint8_t buf[DSM_ANUNCIO_BASE + DSM_ANUNCIO_DATOS];
        size_t bytes = 0;
        if (dsm_anuncio_codificar(buf, sizeof buf, &cab, autor, clave, datos, n, &bytes) == 0) {
            (void)dsm_coherencia_enviar(coherencia, destinos[i], buf, bytes);
        }
    }
}

static int guardar(uint16_t autor, const char *clave, const uint8_t *datos, size_t n)
{
    for (size_t i = 0; i < usados; i++) {
        if (strcmp(tabla[i].clave, clave) != 0 || tabla[i].autor != autor) {
            continue;
        }
        if (misma(&tabla[i], autor, clave, datos, n)) {
            return 0;
        }
        tabla[i].largo = (uint16_t)n;
        if (n != 0) {
            memcpy(tabla[i].datos, datos, n);
        }
        return 1;
    }
    if (usados == AVISOS_MAX) {
        return -1;
    }
    struct aviso *aviso = &tabla[usados++];
    aviso->autor = autor;
    aviso->largo = (uint16_t)n;
    memset(aviso->clave, 0, sizeof aviso->clave);
    memcpy(aviso->clave, clave, strnlen(clave, DSM_ANUNCIO_CLAVE - 1));
    if (n != 0) {
        memcpy(aviso->datos, datos, n);
    }
    return 1;
}

int dsm_aviso_emitir(dsm_coherencia_t *coherencia, uint16_t autor, const char *clave,
                     const void *datos, size_t n)
{
    if (coherencia == NULL || clave == NULL || clave[0] == '\0' || n > DSM_ANUNCIO_DATOS) {
        return DSM_ERR_INVAL;
    }
    pthread_mutex_lock(&mutex);
    int nuevo = guardar(autor, clave, datos, n);
    pthread_mutex_unlock(&mutex);
    if (nuevo < 0) {
        return DSM_ERR_NOMEM;
    }
    difundir(coherencia, autor, clave, datos, n);
    return DSM_OK;
}

int dsm_aviso_recibir(dsm_coherencia_t *coherencia, const uint8_t *buf, size_t n)
{
    dsm_cabecera_t cab;
    uint16_t autor = 0;
    char clave[DSM_ANUNCIO_CLAVE];
    uint8_t datos[DSM_ANUNCIO_DATOS];
    size_t largo = 0;
    if (dsm_anuncio_decodificar(buf, n, &cab, &autor, clave, datos, sizeof datos, &largo) != 0) {
        return -1;
    }
    pthread_mutex_lock(&mutex);
    int nuevo = guardar(autor, clave, datos, largo);
    pthread_mutex_unlock(&mutex);
    if (nuevo > 0) {
        difundir(coherencia, autor, clave, datos, largo);
    }
    return nuevo < 0 ? -1 : 0;
}

void dsm_aviso_reenviar(dsm_coherencia_t *coherencia, uint16_t par)
{
    if (coherencia == NULL) {
        return;
    }
    pthread_mutex_lock(&mutex);
    size_t n = usados;
    struct aviso copia[AVISOS_MAX];
    memcpy(copia, tabla, n * sizeof *copia);
    pthread_mutex_unlock(&mutex);
    uint16_t origen = dsm_coherencia_nodo(coherencia);
    for (size_t i = 0; i < n; i++) {
        dsm_cabecera_t cab = {
            .version = DSM_PROTO_VERSION,
            .tipo = DSM_TIPO_ANUNCIO,
            .origen = origen,
            .destino = par,
            .id_msg = atomic_fetch_add_explicit(&siguiente_id, 1, memory_order_relaxed),
        };
        if (cab.id_msg == 0) {
            cab.id_msg = atomic_fetch_add_explicit(&siguiente_id, 1, memory_order_relaxed);
        }
        uint8_t buf[DSM_ANUNCIO_BASE + DSM_ANUNCIO_DATOS];
        size_t bytes = 0;
        if (dsm_anuncio_codificar(buf, sizeof buf, &cab, copia[i].autor, copia[i].clave,
                                copia[i].datos, copia[i].largo, &bytes) == 0) {
            (void)dsm_coherencia_enviar(coherencia, par, buf, bytes);
        }
    }
}

int dsm_aviso_leer(const char *clave, uint16_t *autor, void *datos, size_t n)
{
    if (clave == NULL || autor == NULL || (n != 0 && datos == NULL)) {
        return DSM_ERR_INVAL;
    }
    int hallado = 0;
    uint16_t mejor = UINT16_MAX;
    pthread_mutex_lock(&mutex);
    for (size_t i = 0; i < usados; i++) {
        if (strcmp(tabla[i].clave, clave) != 0 || tabla[i].largo != n || tabla[i].autor >= mejor) {
            continue;
        }
        mejor = tabla[i].autor;
        if (n != 0) {
            memcpy(datos, tabla[i].datos, n);
        }
        hallado = 1;
    }
    pthread_mutex_unlock(&mutex);
    if (!hallado) {
        return DSM_ERR_ESTADO;
    }
    *autor = mejor;
    return DSM_OK;
}

int dsm_aviso_autores(const char *clave, uint16_t *autores, size_t cap, size_t *cantidad)
{
    if (clave == NULL || cantidad == NULL || (cap != 0 && autores == NULL)) {
        return DSM_ERR_INVAL;
    }
    size_t n = 0;
    pthread_mutex_lock(&mutex);
    for (size_t i = 0; i < usados; i++) {
        if (strcmp(tabla[i].clave, clave) != 0) {
            continue;
        }
        if (n < cap) {
            autores[n] = tabla[i].autor;
        }
        n++;
    }
    pthread_mutex_unlock(&mutex);
    *cantidad = n;
    return n <= cap ? DSM_OK : DSM_ERR_NOMEM;
}
