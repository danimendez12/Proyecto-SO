#include "dsm.h"
#include "config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stddef.h>
#include <unistd.h>

static int candado = -1;
static uint32_t ip_tomada;
static uint16_t puerto_tomado;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t una_vez = PTHREAD_ONCE_INIT;
static int error_fork;

static void preparar_fork(void) { pthread_mutex_lock(&mutex); }
static void padre_fork(void) { pthread_mutex_unlock(&mutex); }
static void hijo_fork(void)
{
    /* Solo se descarta la copia del hijo: el padre conserva el socket guardia.
     * Estos hooks protegen el pool, no los hilos/mutex de un contexto DSM. */
    if (candado >= 0) {
        close(candado);
        candado = -1;
    }
    pthread_mutex_unlock(&mutex);
}

static void iniciar_fork(void)
{
    error_fork = pthread_atfork(preparar_fork, padre_fork, hijo_fork);
}

static int direccion_local(uint32_t ip)
{
    /* Linux enruta todo 127/8 por loopback, incluso sus alias no enumerados. */
    if ((ip >> 24) == 127) {
        return 1;
    }
    struct ifaddrs *lista = NULL;
    if (getifaddrs(&lista) != 0) {
        return 0;
    }
    int local = 0;
    for (struct ifaddrs *ifa = lista; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr != NULL && ifa->ifa_addr->sa_family == AF_INET &&
            ntohl(((struct sockaddr_in *)ifa->ifa_addr)->sin_addr.s_addr) == ip) {
            local = 1;
            break;
        }
    }
    freeifaddrs(lista);
    return local;
}

static int puerto_libre(uint32_t ip, uint16_t puerto)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return 0;
    }
    int reutilizar = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reutilizar, sizeof reutilizar);
    struct sockaddr_in dir = {
        .sin_family = AF_INET, .sin_port = htons(puerto),
        .sin_addr.s_addr = htonl(ip),
    };
    int libre = bind(fd, (struct sockaddr *)&dir, sizeof dir) == 0;
    close(fd);
    return libre;
}

static int reclamar_endpoint(uint32_t ip, uint16_t puerto)
{
    /* Linux: nombres abstractos compartidos entre UIDs pero separados por
     * namespace de red; sin ficheros, permisos ni carreras por unlink. */
    struct sockaddr_un direccion = {.sun_family = AF_UNIX};
    int n = snprintf(direccion.sun_path + 1, sizeof direccion.sun_path - 1,
                     "dsm-pool-%08" PRIx32 "-%u", ip, puerto);
    if (n < 0 || (size_t)n >= sizeof direccion.sun_path - 1) {
        errno = EINVAL;
        return -1;
    }
    /* Sin CLOEXEC, un exec conserva este descriptor, pero no adopta el estado
     * C del pool en la imagen nueva. El dsm_unir actual ejecuta la consola y
     * es ella quien reclama su propio puesto después de ese exec. */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    socklen_t largo = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1u + (size_t)n);
    if (bind(fd, (struct sockaddr *)&direccion, largo) != 0) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

int dsm_pool_reclamar(const char *ruta, uint16_t *id)
{
    if (ruta == NULL || id == NULL) {
        return DSM_ERR_INVAL;
    }
    dsm_config_t cfg;
    if (dsm_config_cargar(ruta, &cfg) != 0) {
        return DSM_ERR_CONFIG;
    }
    /* Instalar una vez los hooks que serializan fork con este mutex local;
     * es distinto del mutex del API y aquí no se inicializa ningún contexto. */
    pthread_once(&una_vez, iniciar_fork);
    if (error_fork != 0) {
        return DSM_ERR_ESTADO;
    }
    pthread_mutex_lock(&mutex);
    /* Idempotencia por endpoint: puede devolverse su ID en esta configuración,
     * pero no se cambia de reclamo mientras el guardia siga retenido. */
    if (candado >= 0) {
        for (uint16_t i = 0; i < cfg.cantidad_nodos; i++) {
            if (cfg.nodos[i].ipv4_host == ip_tomada && cfg.nodos[i].puerto == puerto_tomado) {
                *id = cfg.nodos[i].id;
                pthread_mutex_unlock(&mutex);
                return DSM_OK;
            }
        }
        pthread_mutex_unlock(&mutex);
        return DSM_ERR_OCUPADA;
    }
    int rc = DSM_ERR_OCUPADA;
    for (uint16_t i = 0; i < cfg.cantidad_nodos; i++) {
        const dsm_nodo_cfg_t *nodo = &cfg.nodos[i];
        if (!direccion_local(nodo->ipv4_host)) {
            continue;
        }
        /* El bind UNIX abstracto excluye a otros usuarios cooperantes del pool
         * para esta IPv4/puerto y namespace. La sonda TCP es temporal; iniciar
         * deberá obtener después el listener real y todavía puede fallar. */
        int fd = reclamar_endpoint(nodo->ipv4_host, nodo->puerto);
        if (fd < 0) {
            if (errno == EADDRINUSE) continue;
            rc = DSM_ERR_ESTADO;
            break;
        }
        if (!puerto_libre(nodo->ipv4_host, nodo->puerto)) {
            close(fd);
            continue;
        }
        candado = fd;
        ip_tomada = nodo->ipv4_host;
        puerto_tomado = nodo->puerto;
        *id = nodo->id;
        rc = DSM_OK;
        break;
    }
    pthread_mutex_unlock(&mutex);
    return rc;
}

void dsm_pool_liberar(void)
{
    /* Retirar únicamente el descriptor que conoce este pool. No se desmontan
     * vistas ni se cierra TCP: dsm_cerrar() completa esas capas antes de llegar
     * aquí. Sin guardia es una operación local idempotente, sin semáforos. */
    pthread_mutex_lock(&mutex);
    if (candado >= 0) {
        close(candado);
        candado = -1;
    }
    pthread_mutex_unlock(&mutex);
}
