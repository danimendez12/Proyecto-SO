#include "dsm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void ruta_nodo(const char *argv0, char *destino, size_t cap)
{
    const char *barra = strrchr(argv0, '/');
    if (barra == NULL) {
        snprintf(destino, cap, "dsm_node");
        return;
    }
    int prefijo = (int)(barra - argv0 + 1);
    snprintf(destino, cap, "%.*s%s", prefijo, argv0, "dsm_node");
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "uso: %s <pool.ini> [cantidad]\n", argv[0]);
        return 1;
    }
    int cantidad = 1;
    if (argc == 3) {
        char *fin = NULL;
        long n = strtol(argv[2], &fin, 10);
        if (fin == argv[2] || *fin != '\0' || n < 1 || n > 8) {
            fprintf(stderr, "cantidad fuera de rango\n");
            return 1;
        }
        cantidad = (int)n;
    }
    char nodo[512];
    ruta_nodo(argv[0], nodo, sizeof nodo);
    for (int i = 0; i < cantidad; i++) {
        int ultimo = i + 1 == cantidad;
        pid_t pid = ultimo ? 0 : fork();
        if (pid < 0) {
            perror("fork");
            return 1;
        }
        if (pid > 0) {
            continue;
        }
        uint16_t id = 0;
        int rc = dsm_pool_reclamar(argv[1], &id);
        if (rc != DSM_OK) {
            fprintf(stderr, "no hay puesto libre en %s (DSM %d)\n", argv[1], rc);
            return 1;
        }
        char texto[16];
        snprintf(texto, sizeof texto, "%u", id);
        if (!ultimo) {
            if (setsid() < 0) {
                perror("setsid");
                return 1;
            }
            execl(nodo, nodo, argv[1], texto, "servir", (char *)NULL);
        } else {
            execl(nodo, nodo, argv[1], texto, "interactivo", (char *)NULL);
        }
        perror(nodo);
        return 1;
    }
    return 0;
}
