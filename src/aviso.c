#include "aviso.h"
#include "dsm.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { AVISOS_MAX = 128 };

struct aviso {
    int usado, en_vuelo;
    uint16_t autor, largo, banderas;
    uint64_t sesion, generacion, pendientes;
    char clave[DSM_ANUNCIO_CLAVE];
    uint8_t datos[DSM_ANUNCIO_DATOS];
};

struct dsm_avisos {
    dsm_coherencia_t *coherencia;
    uint16_t self, cantidad;
    uint64_t sesion, secuencia, conectados;
    uint64_t sesiones[DSM_CONFIG_MAX_NODOS];
    uint64_t enlaces[DSM_CONFIG_MAX_NODOS];
    uint64_t recibidos[DSM_CONFIG_MAX_NODOS];
    struct aviso tabla[AVISOS_MAX];
    size_t cursor;
    pthread_mutex_t mutex;
    pthread_cond_t cambio;
    pthread_t hilo;
    int activo, cerrando, saturacion;
};

static int clave_valida(const char *clave)
{
    return clave != NULL && clave[0] != '\0' &&
           strnlen(clave, DSM_ANUNCIO_CLAVE) < DSM_ANUNCIO_CLAVE;
}

static struct aviso *buscar(dsm_avisos_t *s, uint16_t autor, const char *clave)
{
    for (size_t i = 0; i < AVISOS_MAX; i++) {
        if (s->tabla[i].usado && s->tabla[i].autor == autor &&
            strcmp(s->tabla[i].clave, clave) == 0) {
            return &s->tabla[i];
        }
    }
    return NULL;
}

static struct aviso *libre(dsm_avisos_t *s)
{
    for (size_t i = 0; i < AVISOS_MAX; i++) {
        if (!s->tabla[i].usado) {
            return &s->tabla[i];
        }
    }
    return NULL;
}

static void olvidar(dsm_avisos_t *s, uint16_t par)
{
    uint64_t bit = UINT64_C(1) << par;
    s->conectados &= ~bit;
    s->sesiones[par] = 0;
    s->enlaces[par] = 0;
    s->recibidos[par] = 0;
    for (size_t i = 0; i < AVISOS_MAX; i++) {
        s->tabla[i].pendientes &= ~bit;
        if (s->tabla[i].usado && s->tabla[i].autor == par) {
            memset(&s->tabla[i], 0, sizeof s->tabla[i]);
        } else if (s->tabla[i].usado && s->tabla[i].autor == s->self &&
                   s->tabla[i].banderas == DSM_ANUNCIO_RETIRADO &&
                   s->tabla[i].pendientes == 0 && !s->tabla[i].en_vuelo) {
            memset(&s->tabla[i], 0, sizeof s->tabla[i]);
        }
    }
}

static void conectado(dsm_avisos_t *s, uint16_t par, uint64_t sesion, uint64_t enlace)
{
    uint64_t bit = UINT64_C(1) << par;
    if ((s->conectados & bit) != 0 && s->sesiones[par] == sesion && s->enlaces[par] == enlace) {
        return;
    }
    olvidar(s, par);
    s->sesiones[par] = sesion;
    s->enlaces[par] = enlace;
    s->conectados |= bit;
    /* Solo el autor repite su estado actual, incluidas las retiradas. */
    for (size_t i = 0; i < AVISOS_MAX; i++) {
        if (s->tabla[i].usado && s->tabla[i].autor == s->self) {
            s->tabla[i].pendientes |= bit;
        }
    }
    pthread_cond_signal(&s->cambio);
}

static int tomar_envio(dsm_avisos_t *s, struct aviso *copia, uint16_t *par)
{
    for (uint16_t j = 0; j < s->cantidad; j++) {
        uint16_t p = (uint16_t)((s->cursor + j) % s->cantidad);
        uint64_t bit = UINT64_C(1) << p;
        struct aviso *primero = NULL;
        /* Orden global por autor/sesion en cada stream. Permite una marca de
         * agua acotada por autor, sin retener una tumba por cada clave usada. */
        for (size_t i = 0; i < AVISOS_MAX; i++) {
            struct aviso *a = &s->tabla[i];
            if (a->usado && a->autor == s->self && (a->pendientes & bit) != 0 &&
                (primero == NULL || a->generacion < primero->generacion)) {
                primero = a;
            }
        }
        if (primero != NULL) {
            primero->pendientes &= ~bit;
            primero->en_vuelo = 1;
            *copia = *primero;
            *par = p;
            s->cursor = (p + 1u) % s->cantidad;
            return 1;
        }
    }
    return 0;
}

static void *difundir(void *ctx)
{
    dsm_avisos_t *s = ctx;
    pthread_mutex_lock(&s->mutex);
    while (!s->cerrando) {
        struct aviso a;
        uint16_t par;
        if (!s->activo || !tomar_envio(s, &a, &par)) {
            pthread_cond_wait(&s->cambio, &s->mutex);
            continue;
        }
        uint64_t enlace = s->enlaces[par];
        pthread_mutex_unlock(&s->mutex);
        dsm_cabecera_t cab = {
            .version = DSM_PROTO_VERSION, .tipo = DSM_TIPO_ANUNCIO,
            .origen = s->self, .destino = par,
            .id_msg = dsm_coherencia_nuevo_id(s->coherencia),
        };
        dsm_anuncio_t msg = {
            .autor = a.autor, .banderas = a.banderas, .sesion = a.sesion,
            .generacion = a.generacion, .largo = a.largo, .datos = a.datos,
        };
        memcpy(msg.clave, a.clave, sizeof msg.clave);
        uint8_t buf[DSM_ANUNCIO_VERSIONADO_BASE + DSM_ANUNCIO_DATOS];
        size_t bytes = 0;
        if (dsm_anuncio_versionado_codificar(buf, sizeof buf, &cab, &msg, &bytes) == 0) {
            (void)dsm_coherencia_enviar_enlace(s->coherencia, par, enlace, buf, bytes);
        }
        /* La siguiente alta repite la ultima version, sin reintentos ciegos
         * contra un par detenido. No se bloquea el lector TCP. */
        pthread_mutex_lock(&s->mutex);
        struct aviso *actual = buscar(s, a.autor, a.clave);
        if (actual != NULL && actual->generacion == a.generacion) {
            actual->en_vuelo = 0;
            if (actual->banderas == DSM_ANUNCIO_RETIRADO && actual->pendientes == 0) {
                memset(actual, 0, sizeof *actual);
            }
        }
    }
    pthread_mutex_unlock(&s->mutex);
    return NULL;
}

int dsm_avisos_crear(dsm_avisos_t **out, dsm_coherencia_t *c,
                      uint16_t self, uint16_t cantidad, uint64_t sesion)
{
    if (out == NULL || c == NULL || cantidad > DSM_CONFIG_MAX_NODOS ||
        self >= cantidad || sesion == 0) {
        errno = EINVAL;
        return -1;
    }
    *out = NULL;
    dsm_avisos_t *s = calloc(1, sizeof *s);
    if (s == NULL) {
        return -1;
    }
    s->coherencia = c;
    s->self = self;
    s->cantidad = cantidad;
    s->sesion = sesion;
    int error = pthread_mutex_init(&s->mutex, NULL);
    if (error != 0) {
        free(s);
        errno = error;
        return -1;
    }
    error = pthread_cond_init(&s->cambio, NULL);
    if (error == 0) {
        error = pthread_create(&s->hilo, NULL, difundir, s);
        if (error != 0) {
            pthread_cond_destroy(&s->cambio);
        }
    }
    if (error != 0) {
        pthread_mutex_destroy(&s->mutex);
        free(s);
        errno = error;
        return -1;
    }
    *out = s;
    return 0;
}

void dsm_avisos_activar(dsm_avisos_t *s)
{
    pthread_mutex_lock(&s->mutex);
    s->activo = 1;
    pthread_cond_signal(&s->cambio);
    pthread_mutex_unlock(&s->mutex);
}

void dsm_avisos_parar(dsm_avisos_t *s)
{
    if (s == NULL) {
        return;
    }
    pthread_mutex_lock(&s->mutex);
    int unir = !s->cerrando;
    s->cerrando = 1;
    pthread_cond_signal(&s->cambio);
    pthread_mutex_unlock(&s->mutex);
    if (unir) {
        pthread_join(s->hilo, NULL);
    }
}

void dsm_avisos_destruir(dsm_avisos_t *s)
{
    if (s != NULL) {
        dsm_avisos_parar(s);
        pthread_cond_destroy(&s->cambio);
        pthread_mutex_destroy(&s->mutex);
        free(s);
    }
}

void dsm_aviso_estado(dsm_coherencia_t *c, uint16_t par, uint8_t estado)
{
    dsm_avisos_t *s = dsm_coherencia_avisos(c);
    if (s == NULL || par >= s->cantidad || par == s->self) {
        return;
    }
    uint64_t sesion = 0, enlace = 0;
    pthread_mutex_lock(&s->mutex);
    /* Revalida tambien callbacks de una conexion anterior. */
    if (dsm_coherencia_enlace(c, par, &sesion, &enlace) == 0) {
        conectado(s, par, sesion, enlace);
    } else if (estado == DSM_NODO_FUERA || estado == DSM_NODO_EN_LINEA) {
        olvidar(s, par);
    }
    pthread_mutex_unlock(&s->mutex);
}

static int publicar(dsm_coherencia_t *c, uint16_t autor, const char *clave,
                     const void *datos, size_t n, uint16_t banderas)
{
    dsm_avisos_t *s = dsm_coherencia_avisos(c);
    if (s == NULL || autor != s->self || !clave_valida(clave) ||
        n > DSM_ANUNCIO_DATOS || (n != 0 && datos == NULL)) {
        return DSM_ERR_INVAL;
    }
    pthread_mutex_lock(&s->mutex);
    struct aviso *a = buscar(s, autor, clave);
    if (banderas == DSM_ANUNCIO_RETIRADO && a == NULL) {
        pthread_mutex_unlock(&s->mutex);
        return DSM_OK;
    }
    if (a != NULL && a->banderas == banderas && a->largo == n &&
        (n == 0 || memcmp(a->datos, datos, n) == 0)) {
        pthread_mutex_unlock(&s->mutex);
        return DSM_OK;
    }
    if (a == NULL) {
        a = libre(s);
    }
    if (a == NULL || s->secuencia == UINT64_MAX || s->cerrando) {
        pthread_mutex_unlock(&s->mutex);
        return a == NULL ? DSM_ERR_NOMEM : DSM_ERR_ESTADO;
    }
    memset(a, 0, sizeof *a);
    a->usado = 1;
    a->autor = autor;
    a->sesion = s->sesion;
    a->generacion = ++s->secuencia;
    a->banderas = banderas;
    a->largo = (uint16_t)n;
    memcpy(a->clave, clave, strlen(clave) + 1);
    if (n != 0) {
        memcpy(a->datos, datos, n);
    }
    a->pendientes = s->conectados;
    if (banderas == DSM_ANUNCIO_RETIRADO && a->pendientes == 0) {
        memset(a, 0, sizeof *a);
    }
    pthread_cond_signal(&s->cambio);
    pthread_mutex_unlock(&s->mutex);
    return DSM_OK;
}

int dsm_aviso_emitir(dsm_coherencia_t *c, uint16_t autor, const char *clave,
                     const void *datos, size_t n)
{
    return publicar(c, autor, clave, datos, n, 0);
}

int dsm_aviso_quitar(dsm_coherencia_t *c, const char *clave)
{
    return publicar(c, dsm_coherencia_nodo(c), clave, NULL, 0, DSM_ANUNCIO_RETIRADO);
}

int dsm_aviso_recibir(dsm_coherencia_t *c, const uint8_t *buf, size_t n)
{
    dsm_avisos_t *s = dsm_coherencia_avisos(c);
    dsm_cabecera_t cab;
    dsm_anuncio_t msg;
    if (s == NULL) {
        return -1;
    }
    if (dsm_anuncio_versionado_decodificar(buf, n, &cab, &msg) != 0) {
        /* El wire previo sigue siendo valido, pero sin sesion no participa
         * en el descubrimiento de aplicaciones de esta revision. */
        uint16_t autor;
        char clave[DSM_ANUNCIO_CLAVE];
        uint8_t datos[DSM_ANUNCIO_DATOS];
        size_t largo;
        return dsm_anuncio_decodificar(buf, n, &cab, &autor, clave,
                                      datos, sizeof datos, &largo);
    }
    if (msg.autor != cab.origen || msg.autor >= s->cantidad || msg.autor == s->self) {
        return -1;
    }
    pthread_mutex_lock(&s->mutex);
    uint64_t sesion = 0, enlace = 0;
    if (dsm_coherencia_enlace(c, msg.autor, &sesion, &enlace) != 0 || sesion != msg.sesion) {
        pthread_mutex_unlock(&s->mutex);
        return 0;
    }
    conectado(s, msg.autor, sesion, enlace);
    if (msg.generacion <= s->recibidos[msg.autor]) {
        pthread_mutex_unlock(&s->mutex);
        return 0;
    }
    s->recibidos[msg.autor] = msg.generacion;
    struct aviso *a = buscar(s, msg.autor, msg.clave);
    if (msg.banderas == DSM_ANUNCIO_RETIRADO) {
        if (a != NULL) {
            memset(a, 0, sizeof *a);
        }
        pthread_mutex_unlock(&s->mutex);
        return 0;
    }
    if (a == NULL) {
        a = libre(s);
    }
    if (a == NULL) {
        if (!s->saturacion) {
            fprintf(stderr, "nodo %u tabla de avisos llena; aviso remoto omitido\n", s->self);
            s->saturacion = 1;
        }
        pthread_mutex_unlock(&s->mutex);
        return 0;
    }
    memset(a, 0, sizeof *a);
    a->usado = 1;
    a->autor = msg.autor;
    a->sesion = msg.sesion;
    a->generacion = msg.generacion;
    a->banderas = msg.banderas;
    a->largo = (uint16_t)msg.largo;
    memcpy(a->clave, msg.clave, sizeof a->clave);
    if (msg.largo != 0) {
        memcpy(a->datos, msg.datos, msg.largo);
    }
    pthread_mutex_unlock(&s->mutex);
    return 0;
}

static int vigente(dsm_avisos_t *s, const struct aviso *a)
{
    uint64_t actual;
    return a->usado && a->banderas != DSM_ANUNCIO_RETIRADO &&
           dsm_coherencia_sesion(s->coherencia, a->autor, &actual) == 0 && actual == a->sesion;
}

static int leer(dsm_coherencia_t *c, const char *clave, uint16_t *autor,
                 void *datos, size_t n, int exacto)
{
    dsm_avisos_t *s = dsm_coherencia_avisos(c);
    if (s == NULL || !clave_valida(clave) || autor == NULL || n > DSM_ANUNCIO_DATOS ||
        (n != 0 && datos == NULL) || (exacto && *autor >= s->cantidad)) {
        return DSM_ERR_INVAL;
    }
    struct aviso *mejor = NULL;
    pthread_mutex_lock(&s->mutex);
    for (size_t i = 0; i < AVISOS_MAX; i++) {
        struct aviso *a = &s->tabla[i];
        if (a->usado && strcmp(a->clave, clave) == 0 && a->largo == n &&
            (!exacto || a->autor == *autor) && vigente(s, a) &&
            (mejor == NULL || a->autor < mejor->autor)) {
            mejor = a;
        }
    }
    if (mejor != NULL) {
        *autor = mejor->autor;
        if (n != 0) {
            memcpy(datos, mejor->datos, n);
        }
    }
    pthread_mutex_unlock(&s->mutex);
    return mejor == NULL ? DSM_ERR_ESTADO : DSM_OK;
}

int dsm_aviso_leer(dsm_coherencia_t *c, const char *clave, uint16_t *autor,
                    void *datos, size_t n)
{
    return leer(c, clave, autor, datos, n, 0);
}

int dsm_aviso_leer_autor(dsm_coherencia_t *c, const char *clave, uint16_t autor,
                          void *datos, size_t n)
{
    return leer(c, clave, &autor, datos, n, 1);
}

int dsm_aviso_autores(dsm_coherencia_t *c, const char *clave, uint16_t *autores,
                       size_t cap, size_t *cantidad)
{
    dsm_avisos_t *s = dsm_coherencia_avisos(c);
    if (s == NULL || !clave_valida(clave) || cantidad == NULL || (cap != 0 && autores == NULL)) {
        return DSM_ERR_INVAL;
    }
    size_t n = 0;
    pthread_mutex_lock(&s->mutex);
    for (size_t i = 0; i < AVISOS_MAX; i++) {
        if (s->tabla[i].usado && strcmp(s->tabla[i].clave, clave) == 0 && vigente(s, &s->tabla[i])) {
            if (n < cap) {
                autores[n] = s->tabla[i].autor;
            }
            n++;
        }
    }
    pthread_mutex_unlock(&s->mutex);
    *cantidad = n;
    return n <= cap ? DSM_OK : DSM_ERR_NOMEM;
}
