#include "dsm.h"

#include "config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <unistd.h>

static int candado = -1;

static int direccion_local(uint32_t ip)
{
    if (ip == 0x7f000001u) {
        return 1;
    }
    struct ifaddrs *lista = NULL;
    if (getifaddrs(&lista) != 0) {
        return 0;
    }
    int local = 0;
    for (struct ifaddrs *ifa = lista; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        uint32_t actual = ntohl(((struct sockaddr_in *)ifa->ifa_addr)->sin_addr.s_addr);
        if (actual == ip) {
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
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reutilizar, sizeof reutilizar);
    struct sockaddr_in dir = {
        .sin_family = AF_INET,
        .sin_port = htons(puerto),
        .sin_addr.s_addr = htonl(ip),
    };
    int libre = bind(fd, (struct sockaddr *)&dir, sizeof dir) == 0;
    close(fd);
    return libre;
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
    for (uint16_t i = 0; i < cfg.cantidad_nodos; i++) {
        const dsm_nodo_cfg_t *nodo = &cfg.nodos[i];
        if (!direccion_local(nodo->ipv4_host)) {
            continue;
        }
        char camino[64];
        snprintf(camino, sizeof camino, "/tmp/dsm-puesto-%u.lock", nodo->puerto);
        int fd = open(camino, O_RDWR | O_CREAT, 0600);
        if (fd < 0) {
            return DSM_ERR_ESTADO;
        }
        if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
            close(fd);
            continue;
        }
        if (!puerto_libre(nodo->ipv4_host, nodo->puerto)) {
            flock(fd, LOCK_UN);
            close(fd);
            continue;
        }
        if (candado >= 0) {
            close(candado);
        }
        candado = fd;
        *id = nodo->id;
        return DSM_OK;
    }
    return DSM_ERR_OCUPADA;
}
