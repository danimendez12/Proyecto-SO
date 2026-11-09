#include "config.h"
#include "dsm.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

static void uso(const char *argv0)
{
    fprintf(stderr, "uso: %s <config> <id-nodo>\n", argv0);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        uso(argv[0]);
        return 1;
    }
    dsm_config_t cfg;
    if (dsm_config_cargar(argv[1], &cfg) != 0) {
        return 1;
    }
    errno = 0;
    char *fin = NULL;
    unsigned long id = strtoul(argv[2], &fin, 10);
    if (errno != 0 || fin == argv[2] || *fin != '\0' || id > 65535ul) {
        fprintf(stderr, "id de nodo invalido\n");
        return 1;
    }
    const dsm_nodo_cfg_t *nodo = dsm_config_nodo(&cfg, (uint16_t)id);
    if (nodo == NULL) {
        fprintf(stderr, "el nodo %lu no esta en la configuracion\n", id);
        return 1;
    }
    printf("nodo %u\n", (unsigned)nodo->id);
    printf("puerto %u\n", (unsigned)nodo->puerto);
    printf("memoria %llu\n", (unsigned long long)cfg.memoria_inicial);
    return 0;
}
