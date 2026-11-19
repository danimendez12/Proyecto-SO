#include "entrada.h"

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cargar(const char *ruta, size_t limite, double **datos,
                  size_t *filas, size_t *columnas)
{
    FILE *f = fopen(ruta, "r");
    if (!f) {
        perror(ruta);
        return DSM_ERR_INVAL;
    }
    char *linea = NULL;
    size_t capacidad_linea = 0, usados = 0, capacidad = 0;
    int rc = DSM_OK;
    ssize_t leidos;
    while (rc == DSM_OK && (leidos = getline(&linea, &capacidad_linea, f)) >= 0) {
        char *p = linea;
        size_t inicio = usados;
        if (memchr(linea, '\0', (size_t)leidos) != NULL) {
            rc = DSM_ERR_INVAL;
            break;
        }
        while (*p) {
            while (isspace((unsigned char)*p)) p++;
            if (!*p) break;
            char *fin;
            errno = 0;
            double v = strtod(p, &fin);
            if (errno || !isfinite(v) || fin == p || (*fin && !isspace((unsigned char)*fin))) {
                rc = DSM_ERR_INVAL;
                break;
            }
            if (usados == capacidad) {
                size_t max = limite / sizeof(double);
                if (capacidad == max) {
                    rc = DSM_ERR_NOMEM;
                    break;
                }
                size_t nueva = capacidad ? 2 * capacidad : 256;
                if (nueva > max) nueva = max;
                double *buf = realloc(*datos, nueva * sizeof *buf);
                if (!buf) {
                    rc = DSM_ERR_NOMEM;
                    break;
                }
                *datos = buf;
                capacidad = nueva;
            }
            (*datos)[usados++] = v;
            p = fin;
        }
        size_t ancho = usados - inicio;
        if (rc == DSM_OK && (ancho == 0 || (*filas && ancho != *columnas))) {
            rc = DSM_ERR_INVAL;
        }
        if (rc == DSM_OK) {
            *columnas = ancho;
            (*filas)++;
        }
    }
    if (rc == DSM_OK && (!feof(f) || ferror(f) || !*filas)) rc = DSM_ERR_INVAL;
    if (rc != DSM_OK) fprintf(stderr, "Entrada invalida o demasiado grande en %s, fila %zu\n", ruta, *filas + 1);
    free(linea);
    fclose(f);
    return rc;
}

int entradas_cargar(const char *ruta_a, const char *ruta_b, size_t limite,
                    matrices_t *m, entradas_t *e)
{
    size_t filas_b = 0;
    int rc = cargar(ruta_a, limite, &e->a, &m->filas, &m->interior);
    if (rc == DSM_OK) rc = cargar(ruta_b, limite, &e->b, &filas_b, &m->columnas);
    if (rc == DSM_OK && m->interior != filas_b) {
        fprintf(stderr, "Dimensiones incompatibles: A tiene %zu columnas y B tiene %zu filas\n",
                m->interior, filas_b);
        rc = DSM_ERR_INVAL;
    }
    return rc;
}

int entradas_publicar(dsm_context_t *ctx, const matrices_t *m, const entradas_t *e)
{
    size_t bytes[3];
    int rc = matrices_tamanos(m, bytes);
    double *bloque = malloc(m->page_size);
    if (!bloque) return DSM_ERR_NOMEM;
    const dsm_reserva_id_t ids[] = {m->a, m->b, m->c};
    for (size_t t = 0; t < 3 && rc == DSM_OK; t++) {
        for (size_t off = 0; off < bytes[t] && rc == DSM_OK; off += m->page_size) {
            size_t n = bytes[t] - off;
            if (n > m->page_size) n = m->page_size;
            for (size_t j = 0; j < n / sizeof(double); j++) {
                size_t i = off / sizeof(double) + j;
                if (t == 0) bloque[j] = e->a[i];
                else if (t == 1) bloque[j] = e->b[(i % m->interior) * m->columnas + i / m->interior];
                else bloque[j] = NAN;
            }
            rc = dsm_escribir(ctx, ids[t], off, bloque, n);
        }
    }
    free(bloque);
    return rc;
}

int entradas_verificar(dsm_context_t *ctx, const matrices_t *m, const entradas_t *e)
{
    double *bloque = malloc(m->page_size);
    if (!bloque) return DSM_ERR_NOMEM;
    size_t total = m->filas * m->columnas, paso = m->page_size / sizeof(double);
    int rc = DSM_OK;
    for (size_t i = 0; i < total && rc == DSM_OK; i += paso) {
        size_t n = total - i < paso ? total - i : paso;
        rc = dsm_leer(ctx, m->c, i * sizeof(double), bloque, n * sizeof(double));
        for (size_t j = 0; j < n && rc == DSM_OK; j++) {
            size_t f = (i + j) / m->columnas, c = (i + j) % m->columnas;
            long double ref = 0, escala = 0;
            for (size_t k = 0; k < m->interior; k++) {
                long double prod = (long double)e->a[f * m->interior + k] * e->b[k * m->columnas + c];
                ref += prod;
                escala += fabsl(prod);
            }
            if (!isfinite(bloque[j]) || !isfinite(ref) ||
                fabsl(ref - bloque[j]) > (2 * m->interior + 1) * DBL_EPSILON * escala
                                        + (m->interior + 1) * (long double)DBL_TRUE_MIN) {
                fprintf(stderr, "Diferencia C[%zu,%zu] esperado=%.18Lg obtenido=%.17g\n", f, c, ref, bloque[j]);
                rc = DSM_ERR_ESTADO;
            } else if (i + j == 0 || i + j == total / 2 || i + j + 1 == total) {
                printf("MUESTRA C[%zu,%zu]=%.17g\n", f, c, bloque[j]);
            }
        }
    }
    free(bloque);
    return rc;
}

void entradas_cerrar(entradas_t *e)
{
    free(e->a);
    free(e->b);
}

/* Alimenta el ejecutable secuencial original con el mismo generador del DSM. */
int entradas_exportar(const char *ruta, size_t filas, size_t columnas, int es_b)
{
    if (!filas || !columnas || filas > SIZE_MAX / sizeof(double) / columnas) return DSM_ERR_INVAL;
    FILE *f = fopen(ruta, "wx");
    if (!f) { perror(ruta); return DSM_ERR_ESTADO; }
    int rc = DSM_OK;
    for (size_t i = 0; i < filas && rc == DSM_OK; i++) {
        for (size_t j = 0; j < columnas; j++) {
            double v = es_b ? matrices_dato_b(i, j) : matrices_dato_a(i, j);
            if (fprintf(f, "%.0f%c", v, j + 1 == columnas ? '\n' : '\t') < 0) {
                rc = DSM_ERR_ESTADO;
                break;
            }
        }
    }
    if (fclose(f) != 0) rc = DSM_ERR_ESTADO;
    return rc;
}
