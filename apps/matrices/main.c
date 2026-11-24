#include "demanda.h"
#include "entrada.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int numero(const char *s, size_t max, size_t *valor)
{
    char *fin;
    errno = 0;
    uintmax_t n = strtoumax(s, &fin, 10);
    if (*s < '0' || *s > '9' || *fin || errno || !n || n > max) {
        return -1;
    }
    *valor = (size_t)n;
    return 0;
}

static int uso(const char *nombre)
{
    fprintf(stderr, "Uso:\n  %s INI FILAS INTERIOR COLUMNAS\n"
                    "  %s exportar A|B FILAS COLUMNAS ARCHIVO.tsv\n",
            nombre, nombre);
    return 1;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc == 6 && strcmp(argv[1], "exportar") == 0) {
        size_t filas, columnas;
        if ((strcmp(argv[2], "A") && strcmp(argv[2], "B")) ||
            numero(argv[3], SIZE_MAX, &filas) || numero(argv[4], SIZE_MAX, &columnas)) {
            return uso(argv[0]);
        }
        return entradas_exportar(argv[5], filas, columnas, *argv[2] == 'B') == DSM_OK ? 0 : 1;
    }
    if (argc == 5 && access(argv[1], R_OK) == 0) {
        size_t filas, interior, columnas;
        if (numero(argv[2], SIZE_MAX, &filas) || numero(argv[3], SIZE_MAX, &interior) ||
            numero(argv[4], SIZE_MAX, &columnas)) {
            return uso(argv[0]);
        }
        int rc = matrices_demanda(argv[1], filas, interior, columnas);
        if (rc != DSM_OK) {
            fprintf(stderr, "ERROR demanda DSM=%d\n", rc);
        }
        return rc == DSM_OK ? 0 : 1;
    }
    return uso(argv[0]);
}
