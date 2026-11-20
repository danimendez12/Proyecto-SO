#include "datos.h"
#include "orden.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { TROZO = 4096, RUN_REFERENCIA = 65536, VENTANA = 256 };

double datos_reloj(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) {
        perror("CLOCK_MONOTONIC");
        exit(EXIT_FAILURE);
    }
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* Lee un entero decimal separado por espacios, incluso entre líneas TSV.
 * Devuelve 1 para un valor, 0 al final y -1 por sintaxis, rango o E/S. */
static int siguiente(FILE *f, int32_t *salida)
{
    int c;
    do {
        c = fgetc(f);
    } while (c != EOF && isspace(c));
    if (c == EOF) return ferror(f) ? -1 : 0;
    int negativo = c == '-';
    if (negativo || c == '+') c = fgetc(f);
    if (c < '0' || c > '9') return -1;
    uint32_t limite = (uint32_t)INT32_MAX + (unsigned)negativo;
    uint32_t v = 0;
    do {
        unsigned digito = (unsigned)(c - '0');
        if (v > (limite - digito) / 10) return -1;
        v = v * 10 + digito;
        c = fgetc(f);
    } while (c >= '0' && c <= '9');
    if (ferror(f) || (c != EOF && !isspace(c))) return -1;
    *salida = (int32_t)(negativo ? -(int64_t)v : (int64_t)v);
    return 1;
}

enum patron { ALEATORIO, INVERSO, ORDENADO, NEGATIVOS, REPETIDOS, IGUALES, ARCHIVO };
static const char *const nombres[] = {
    "aleatorio", "inverso", "ordenado", "negativos", "repetidos", "iguales", "archivo"
};

int datos_preparar(size_t n, const char *nombre, uint32_t semilla,
                   const char *archivo, FILE **original)
{
    size_t patron = 0;
    while (patron < sizeof nombres / sizeof nombres[0] && strcmp(nombre, nombres[patron]))
        ++patron;
    if (patron > ARCHIVO || (patron == ARCHIVO) != (archivo != NULL) ||
        n == 0 || n > INT32_MAX) return DSM_ERR_INVAL;
    FILE *entrada = archivo ? fopen(archivo, "r") : NULL;
    if (archivo && !entrada) { perror(archivo); return DSM_ERR_INVAL; }
    FILE *f = tmpfile();
    int rc = f ? DSM_OK : DSM_ERR_ESTADO;
    int32_t bloque[TROZO];
    uint32_t x = semilla;
    for (size_t i = 0; i < n && rc == DSM_OK;) {
        size_t k = n - i < TROZO ? n - i : TROZO;
        for (size_t j = 0; j < k; ++j) {
            x = UINT32_C(1664525) * x + UINT32_C(1013904223);
            switch (patron) {
            case ALEATORIO: bloque[j] = (int32_t)((int64_t)x - INT64_C(2147483648)); break;
            case ORDENADO: bloque[j] = (int32_t)(i + j); break;
            case INVERSO: bloque[j] = (int32_t)(n - i - j); break;
            case NEGATIVOS: bloque[j] = -1 - (int32_t)(x >> 1); break;
            case REPETIDOS: bloque[j] = (int32_t)((x >> 16) % 9) - 4; break;
            case IGUALES: bloque[j] = -17; break;
            case ARCHIVO:
                if (siguiente(entrada, &bloque[j]) != 1) {
                    fprintf(stderr, "Entrada inválida en índice %zu\n", i + j);
                    rc = DSM_ERR_INVAL;
                }
                break;
            }
            if (rc != DSM_OK) break;
        }
        if (rc == DSM_OK && fwrite(bloque, sizeof *bloque, k, f) != k) rc = DSM_ERR_ESTADO;
        i += k;
    }
    if (entrada) {
        if (rc == DSM_OK && siguiente(entrada, bloque) != 0) rc = DSM_ERR_INVAL;
        fclose(entrada);
    }
    if (rc == DSM_OK && fflush(f) != 0) rc = DSM_ERR_ESTADO;
    if (rc == DSM_OK) *original = f;
    else if (f) fclose(f);
    return rc;
}

int datos_inicializar(dsm_context_t *ctx, dsm_reserva_id_t id, size_t n, FILE *original)
{
    int32_t v[TROZO];
    if (fseeko(original, 0, SEEK_SET) != 0) return DSM_ERR_ESTADO;
    for (size_t i = 0; i < n;) {
        size_t k = n - i < TROZO ? n - i : TROZO;
        if (fread(v, sizeof *v, k, original) != k) return DSM_ERR_ESTADO;
        int rc = dsm_escribir(ctx, id, i * sizeof *v, v, k * sizeof *v);
        if (rc != DSM_OK) return rc;
        i += k;
    }
    return DSM_OK;
}

static int comparar(const void *a, const void *b)
{
    int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;
    return (x > y) - (x < y);
}

/* Referencia independiente. qsort ordena tramos privados. Una cola de prioridad
 * extrae el menor valor de todos los tramos, sin reutilizar la fusión adaptada.
 * El archivo temporal es local al creador y jamás transporta datos a otro nodo. */
struct cursor {
    size_t siguiente, fin, pos, usados;
    int32_t ventana[VENTANA];
};

static int recargar(FILE *f, struct cursor *c)
{
    size_t n = c->fin - c->siguiente;
    if (n > VENTANA) n = VENTANA;
    if (fseeko(f, (off_t)c->siguiente * sizeof(int32_t), SEEK_SET) ||
        fread(c->ventana, sizeof(int32_t), n, f) != n) return DSM_ERR_ESTADO;
    c->siguiente += n;
    c->usados = n;
    c->pos = 0;
    return DSM_OK;
}

static int32_t cabeza(const struct cursor *c) { return c->ventana[c->pos]; }

static void hundir(struct cursor **cola, size_t n, size_t i)
{
    while (i < n / 2) {
        size_t hijo = 2 * i + 1;
        if (hijo + 1 < n && cabeza(cola[hijo + 1]) < cabeza(cola[hijo])) hijo++;
        if (cabeza(cola[i]) <= cabeza(cola[hijo])) break;
        struct cursor *aux = cola[i];
        cola[i] = cola[hijo];
        cola[hijo] = aux;
        i = hijo;
    }
}

int datos_verificar(dsm_context_t *ctx, dsm_reserva_id_t id, size_t n, FILE *original)
{
    if (!n || !original || fseeko(original, 0, SEEK_SET)) return DSM_ERR_INVAL;
    double t0 = datos_reloj();
    size_t tramos = 1 + (n - 1) / RUN_REFERENCIA;
    FILE *ordenado = tmpfile();
    struct cursor *c = calloc(tramos, sizeof *c);
    struct cursor **cola = calloc(tramos, sizeof *cola);
    size_t capacidad = n < RUN_REFERENCIA ? n : RUN_REFERENCIA;
    int32_t *v = malloc(capacidad * sizeof *v);
    int rc = !ordenado ? DSM_ERR_ESTADO : (!c || !cola || !v) ? DSM_ERR_NOMEM : DSM_OK;
    for (size_t r = 0; r < tramos && rc == DSM_OK; ++r) {
        size_t i = r * RUN_REFERENCIA, k = n - i < capacidad ? n - i : capacidad;
        if (fread(v, sizeof *v, k, original) != k) { rc = DSM_ERR_ESTADO; break; }
        qsort(v, k, sizeof *v, comparar);
        if (fwrite(v, sizeof *v, k, ordenado) != k) { rc = DSM_ERR_ESTADO; break; }
        c[r].siguiente = i;
        c[r].fin = i + k;
        cola[r] = &c[r];
    }
    if (rc == DSM_OK && (fgetc(original) != EOF || ferror(original))) rc = DSM_ERR_INVAL;
    for (size_t r = 0; r < tramos && rc == DSM_OK; ++r) rc = recargar(ordenado, &c[r]);
    if (rc == DSM_OK) {
        for (size_t r = tramos / 2; r > 0; --r) hundir(cola, tramos, r - 1);
    }
    int ascendente = 1;
    size_t leidos = 0, primer_error = n;
    int32_t previo = 0, esperado = 0, obtenido = 0;
    while (leidos < n && rc == DSM_OK) {
        size_t k = n - leidos < TROZO ? n - leidos : TROZO;
        rc = dsm_leer(ctx, id, leidos * sizeof *v, v, k * sizeof *v);
        for (size_t j = 0; j < k && rc == DSM_OK; ++j) {
            if (!tramos) { rc = DSM_ERR_ESTADO; break; }
            struct cursor *menor = cola[0];
            if (v[j] != cabeza(menor) && primer_error == n) {
                primer_error = leidos;
                esperado = cabeza(menor);
                obtenido = v[j];
            }
            if (leidos && previo > v[j]) ascendente = 0;
            previo = v[j];
            ++leidos;
            if (++menor->pos == menor->usados) {
                if (menor->siguiente == menor->fin) cola[0] = cola[--tramos];
                else rc = recargar(ordenado, menor);
            }
            if (rc == DSM_OK) hundir(cola, tramos, 0);
        }
    }
    free(v);
    free(cola);
    free(c);
    if (ordenado && fclose(ordenado) != 0 && rc == DSM_OK) rc = DSM_ERR_ESTADO;
    if (rc == DSM_OK && (primer_error != n || !ascendente || tramos)) rc = DSM_ERR_ESTADO;
    if (primer_error < n)
        printf("DIFERENCIA indice=%zu esperado=%" PRId32 " obtenido=%" PRId32 "\n",
               primer_error, esperado, obtenido);
    printf("VERIFICACION %s elementos=%zu comparados=%zu ordenado=%s coincide=%s segundos=%.6f\n",
           rc == DSM_OK ? "OK" : "ERROR", n, leidos,
           leidos < n ? "incompleto" : ascendente ? "si" : "no",
           leidos < n ? "incompleto" : primer_error == n ? "si" : "no", datos_reloj() - t0);
    return rc;
}

/* Modo de rendimiento separado, en un proceso secuencial sin contexto DSM. */
int datos_secuencial(size_t n, FILE *original)
{
    size_t bytes = n * sizeof(int32_t);
    int32_t *a = malloc(bytes), *aux = malloc(bytes);
    int rc = a && aux ? DSM_OK : DSM_ERR_NOMEM;
    if (rc == DSM_OK && (fseeko(original, 0, SEEK_SET) || fread(a, sizeof *a, n, original) != n))
        rc = DSM_ERR_ESTADO;
    if (rc == DSM_OK) {
        double t = datos_reloj();
        orden_secuencial(a, aux, n);
        double merge = datos_reloj() - t;
        if (fseeko(original, 0, SEEK_SET) || fread(aux, sizeof *aux, n, original) != n)
            rc = DSM_ERR_ESTADO;
        else {
            t = datos_reloj();
            qsort(aux, n, sizeof *aux, comparar);
            double q = datos_reloj() - t;
            if (memcmp(a, aux, bytes)) rc = DSM_ERR_ESTADO;
            printf("SECUENCIAL %s elementos=%zu merge_s=%.6f qsort_s=%.6f\n",
                   rc == DSM_OK ? "OK" : "ERROR", n, merge, q);
        }
    }
    free(aux);
    free(a);
    return rc;
}
