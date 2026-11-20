#ifndef MATRICES_ENTRADA_H
#define MATRICES_ENTRADA_H

#include "calculo.h"

/* Las copias de los archivos permanecen en el creador como referencia. */
typedef struct {
    double *a;
    double *b;
} entradas_t;

int entradas_cargar(const char *ruta_a, const char *ruta_b, size_t limite,
                    matrices_t *m, entradas_t *e);
int entradas_publicar(dsm_context_t *ctx, const matrices_t *m, const entradas_t *e);
int entradas_verificar(dsm_context_t *ctx, const matrices_t *m, const entradas_t *e);
void entradas_cerrar(entradas_t *e);
int entradas_exportar(const char *ruta, size_t filas, size_t columnas, int es_b);

#endif
