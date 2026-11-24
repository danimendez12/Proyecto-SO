#include "datos.h"
#include "demanda.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int numero(const char *s, uint64_t min, uint64_t max, uint64_t *valor)
{
    if (!s || *s < '0' || *s > '9') {
        return 0;
    }
    char *fin;
    errno = 0;
    uintmax_t v = strtoumax(s, &fin, 10);
    if (errno || *fin || v < min || v > max) {
        return 0;
    }
    *valor = v;
    return 1;
}

static int uso(const char *programa)
{
    fprintf(stderr, "Uso:\n  %s INI N PATRON SEMILLA\n"
                    "  %s secuencial N PATRON SEMILLA [ARCHIVO]\n"
                    "Patrones: aleatorio, inverso, ordenado, negativos, repetidos, iguales, archivo\n",
            programa, programa);
    return 2;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 2) {
        return uso(argv[0]);
    }
    if (strcmp(argv[1], "secuencial") == 0) {
        uint64_t n = 0, semilla = 0;
        if ((argc != 5 && argc != 6) || !numero(argv[2], 1, INT32_MAX, &n) ||
            n > SIZE_MAX / sizeof(int32_t) || !numero(argv[4], 0, UINT32_MAX, &semilla)) {
            return uso(argv[0]);
        }
        FILE *original = NULL;
        int rc = datos_preparar((size_t)n, argv[3], (uint32_t)semilla, argc == 6 ? argv[5] : NULL,
                                &original);
        if (rc == DSM_OK) {
            rc = datos_secuencial((size_t)n, original);
            if (fclose(original) && rc == DSM_OK) {
                rc = DSM_ERR_ESTADO;
            }
        }
        if (rc != DSM_OK) {
            fprintf(stderr, "ERROR secuencial DSM=%d\n", rc);
        }
        return rc == DSM_OK ? 0 : 1;
    }
    if (argc == 5 && access(argv[1], R_OK) == 0) {
        uint64_t n = 0, semilla = 0;
        if (!numero(argv[2], 1, INT32_MAX, &n) || n > SIZE_MAX / sizeof(int32_t) ||
            !numero(argv[4], 0, UINT32_MAX, &semilla)) {
            return uso(argv[0]);
        }
        int rc = mergesort_demanda(argv[1], (size_t)n, argv[3], (uint32_t)semilla);
        if (rc != DSM_OK) {
            fprintf(stderr, "ERROR demanda DSM=%d\n", rc);
        }
        return rc == DSM_OK ? 0 : 1;
    }
    return uso(argv[0]);
}
