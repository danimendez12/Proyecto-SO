/* Adaptacion de matrix_multiplication/src/thread2.c, Mathias Perlet.
 * Copyright (c) 2022 Mathias Perlet. Licencia MIT en LICENSE.
 * Conserva producto escalar, reparto estatico, ultimo rango con resto y join.
 */
#include "calculo.h"

#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

enum { PROYECCIONES = 4 };

static int instante(double *salida)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) {
        return DSM_ERR_ESTADO;
    }
    *salida = (double)t.tv_sec + (double)t.tv_nsec / 1000000000.0;
    return DSM_OK;
}

static uint64_t mezclar(uint64_t x)
{
    x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
    return x ^ (x >> 31);
}

static double dato(size_t fila, size_t columna, uint64_t semilla)
{
    uint64_t x = (uint64_t)fila * UINT64_C(0x9e3779b97f4a7c15) ^
                 (uint64_t)columna * UINT64_C(0xbf58476d1ce4e5b9) ^ semilla;
    return (double)((int)(mezclar(x) % 17u) - 8);
}

double matrices_dato_a(size_t fila, size_t interior)
{
    return dato(fila, interior, UINT64_C(2026));
}

double matrices_dato_b(size_t interior, size_t columna)
{
    return dato(interior, columna, UINT64_C(2027));
}

static int tamano(size_t filas, size_t columnas, size_t *bytes)
{
    if (filas > SIZE_MAX / columnas || filas * columnas > SIZE_MAX / sizeof(double)) {
        return DSM_ERR_NOMEM;
    }
    *bytes = filas * columnas * sizeof(double);
    return DSM_OK;
}

int matrices_tamanos(const matrices_t *m, size_t bytes[3])
{
    if (m == NULL || bytes == NULL || m->filas == 0 || m->interior == 0 ||
        m->columnas == 0 || m->page_size < sizeof(double) ||
        m->page_size % sizeof(double) != 0) {
        return DSM_ERR_INVAL;
    }
    /* Entradas enteras [-8,8]. Tambien las proyecciones por fila deben
     * permanecer dentro de los enteros representables exactamente en double. */
    uint64_t max_interior = UINT64_C(9007199254740992) / 64u;
    if (m->interior > max_interior || m->columnas > max_interior / m->interior) {
        return DSM_ERR_INVAL;
    }
    int rc = tamano(m->filas, m->interior, &bytes[0]);
    if (rc == DSM_OK) {
        rc = tamano(m->interior, m->columnas, &bytes[1]);
    }
    if (rc == DSM_OK) {
        rc = tamano(m->filas, m->columnas, &bytes[2]);
    }
    return rc;
}

static int validar(const matrices_t *m, size_t bytes[3])
{
    int rc = matrices_tamanos(m, bytes);
    if (rc == DSM_OK && (m->a == 0 || m->b == 0 || m->c == 0 ||
        m->a == m->b || m->a == m->c || m->b == m->c)) {
        return DSM_ERR_INVAL;
    }
    return rc;
}

int matrices_inicializar(dsm_context_t *ctx, const matrices_t *m)
{
    size_t bytes[3];
    int rc = validar(m, bytes);
    if (ctx == NULL || rc != DSM_OK) {
        return ctx == NULL ? DSM_ERR_INVAL : rc;
    }
    double *pagina = malloc(m->page_size);
    if (pagina == NULL) {
        return DSM_ERR_NOMEM;
    }
    dsm_reserva_id_t ids[3] = {m->a, m->b, m->c};
    for (size_t matriz = 0; matriz < 3 && rc == DSM_OK; matriz++) {
        for (size_t offset = 0; offset < bytes[matriz];) {
            size_t cantidad = bytes[matriz] - offset;
            if (cantidad > m->page_size) {
                cantidad = m->page_size;
            }
            for (size_t i = 0; i < cantidad / sizeof(double); i++) {
                size_t indice = offset / sizeof(double) + i;
                if (matriz == 0) {
                    pagina[i] = matrices_dato_a(indice / m->interior, indice % m->interior);
                } else if (matriz == 1) {
                    pagina[i] = matrices_dato_b(indice % m->interior, indice / m->interior);
                } else {
                    pagina[i] = NAN; /* Un resultado ausente nunca pasa como un cero legitimo. */
                }
            }
            rc = dsm_escribir(ctx, ids[matriz], offset, pagina, cantidad);
            if (rc != DSM_OK) {
                fprintf(stderr, "inicializacion matriz %c offset %zu error DSM %d\n",
                        (int)('A' + matriz), offset, rc);
                break;
            }
            offset += cantidad;
        }
    }
    free(pagina);
    return rc;
}

struct tarea {
    dsm_context_t *ctx;
    dsm_proc_id_t proc;
    const matrices_t *m;
    size_t inicio;
    size_t fin;
    int rc;
};

static void *worker(void *arg)
{
    struct tarea *t = arg;
    t->rc = dsm_hilo_registrar(t->ctx, t->proc);
    if (t->rc != DSM_OK) {
        return NULL;
    }
    void *base_a = NULL, *base_b = NULL;
    double *pagina = NULL;
    t->rc = dsm_traducir(t->ctx, t->m->a, 0, &base_a);
    if (t->rc == DSM_OK) {
        t->rc = dsm_traducir(t->ctx, t->m->b, 0, &base_b);
    }
    if (t->rc == DSM_OK) {
        pagina = malloc(t->m->page_size);
        if (pagina == NULL) {
            t->rc = DSM_ERR_NOMEM;
        }
    }
    const double *a = base_a;
    const double *bt = base_b;
    size_t por_pagina = t->m->page_size / sizeof(double);
    size_t elementos = t->m->filas * t->m->columnas;
    for (size_t bloque = t->inicio; bloque < t->fin && t->rc == DSM_OK; bloque++) {
        size_t inicial = bloque * por_pagina;
        size_t cantidad = elementos - inicial;
        if (cantidad > por_pagina) {
            cantidad = por_pagina;
        }
        for (size_t x = 0; x < cantidad; x++) {
            size_t i = (inicial + x) / t->m->columnas;
            size_t j = (inicial + x) % t->m->columnas;
            const double *fila_a = a + i * t->m->interior;
            const double *columna_b = bt + j * t->m->interior;
            double suma = 0.0;
            /* El producto i,j,k de thread2. B_t evita recorrer B con stride
             * sin copiar A/B fuera del DSM durante el calculo. */
            for (size_t k = 0; k < t->m->interior; k++) {
                suma += fila_a[k] * columna_b[k];
            }
            pagina[x] = suma;
        }
        t->rc = dsm_escribir(t->ctx, t->m->c, inicial * sizeof(double),
                              pagina, cantidad * sizeof(double));
    }
    free(pagina);
    int rc = dsm_hilo_desregistrar(t->ctx);
    if (t->rc == DSM_OK) {
        t->rc = rc;
    }
    return NULL;
}

int matrices_calcular(dsm_context_t *ctx, dsm_proc_id_t proc, const matrices_t *m,
                       size_t primer_bloque, size_t fin_bloque, size_t hilos)
{
    size_t bytes[3];
    int rc = validar(m, bytes);
    if (ctx == NULL || proc == 0 || rc != DSM_OK) {
        return ctx == NULL || proc == 0 ? DSM_ERR_INVAL : rc;
    }
    size_t bloques = 1 + (bytes[2] - 1) / m->page_size;
    if (primer_bloque > fin_bloque || fin_bloque > bloques ||
        hilos == 0 || hilos > MATRICES_HILOS_MAX) {
        return DSM_ERR_INVAL;
    }
    if (primer_bloque == fin_bloque) {
        return DSM_OK;
    }
    size_t pedidos = hilos;
    if (hilos > fin_bloque - primer_bloque) {
        hilos = fin_bloque - primer_bloque;
    }
    struct tarea *tareas = calloc(hilos, sizeof *tareas);
    pthread_t *threads = calloc(hilos, sizeof *threads);
    if (tareas == NULL || threads == NULL) {
        free(tareas);
        free(threads);
        return DSM_ERR_NOMEM;
    }
    double inicio, fin;
    rc = instante(&inicio);
    size_t creados = 0;
    size_t delta = (fin_bloque - primer_bloque) / hilos;
    for (size_t id = 0; id < hilos && rc == DSM_OK; id++) {
        tareas[id] = (struct tarea){
            .ctx = ctx, .proc = proc, .m = m,
            .inicio = primer_bloque + id * delta,
            .fin = id == hilos - 1 ? fin_bloque : primer_bloque + (id + 1) * delta,
        };
        if (pthread_create(&threads[id], NULL, worker, &tareas[id]) != 0) {
            rc = DSM_ERR_HILOS;
            break;
        }
        creados++;
    }
    for (size_t i = 0; i < creados; i++) {
        int unido = pthread_join(threads[i], NULL);
        if (rc == DSM_OK) {
            rc = unido == 0 ? tareas[i].rc : DSM_ERR_ESTADO;
        }
    }
    if (instante(&fin) == DSM_OK && creados != 0) {
        printf("matrices calculo_s %.6f hilos_solicitados %zu hilos_efectivos %zu bloques [%zu,%zu)\n",
               fin - inicio, pedidos, hilos, primer_bloque, fin_bloque);
    }
    free(threads);
    free(tareas);
    return rc;
}

static int leer(dsm_context_t *ctx, dsm_reserva_id_t id, size_t bytes,
                 size_t page_size, double *destino)
{
    for (size_t offset = 0; offset < bytes;) {
        size_t n = bytes - offset;
        if (n > page_size) {
            n = page_size;
        }
        int rc = dsm_leer(ctx, id, offset, (unsigned char *)destino + offset, n);
        if (rc != DSM_OK) {
            return rc;
        }
        offset += n;
    }
    return DSM_OK;
}

static int diferencia(const char *nombre, size_t i, size_t j, double esperado, double obtenido)
{
    fprintf(stderr, "verificacion ERROR %s[%zu,%zu] esperado %.17g obtenido %.17g\n",
            nombre, i, j, esperado, obtenido);
    return DSM_ERR_ESTADO;
}

static double referencia_elemento(const matrices_t *m, size_t i, size_t j)
{
    double suma = 0.0;
    for (size_t k = 0; k < m->interior; k++) {
        suma += matrices_dato_a(i, k) * matrices_dato_b(k, j);
    }
    return suma;
}

static int verificar_pequena(dsm_context_t *ctx, const matrices_t *m, const size_t bytes[3])
{
    double *a = malloc(bytes[0]), *bt = malloc(bytes[1]);
    double *b = malloc(bytes[1]), *referencia = calloc(1, bytes[2]), *c = malloc(bytes[2]);
    int rc = DSM_OK;
    if (a == NULL || bt == NULL || b == NULL || referencia == NULL || c == NULL) {
        rc = DSM_ERR_NOMEM;
        goto fin;
    }
    rc = leer(ctx, m->a, bytes[0], m->page_size, a);
    if (rc == DSM_OK) rc = leer(ctx, m->b, bytes[1], m->page_size, bt);
    if (rc == DSM_OK) rc = leer(ctx, m->c, bytes[2], m->page_size, c);
    if (rc != DSM_OK) goto fin;
    for (size_t i = 0; i < m->filas; i++) {
        for (size_t k = 0; k < m->interior; k++) {
            double esperado = matrices_dato_a(i, k);
            if (a[i * m->interior + k] != esperado) {
                rc = diferencia("A", i, k, esperado, a[i * m->interior + k]);
                goto fin;
            }
        }
    }
    for (size_t k = 0; k < m->interior; k++) {
        for (size_t j = 0; j < m->columnas; j++) {
            double obtenido = bt[j * m->interior + k], esperado = matrices_dato_b(k, j);
            if (obtenido != esperado) {
                rc = diferencia("B", k, j, esperado, obtenido);
                goto fin;
            }
            b[k * m->columnas + j] = obtenido;
        }
    }
    double inicio, fin_tiempo;
    rc = instante(&inicio);
    if (rc != DSM_OK) goto fin;
    /* Referencia independiente: bucle original i,j,k con B en orden por filas,
     * sin reparto en paginas, Pthreads ni B transpuesta en el producto. */
    for (size_t i = 0; i < m->filas; i++) {
        for (size_t j = 0; j < m->columnas; j++) {
            for (size_t k = 0; k < m->interior; k++) {
                referencia[i * m->columnas + j] += a[i * m->interior + k] * b[k * m->columnas + j];
            }
        }
    }
    rc = instante(&fin_tiempo);
    if (rc != DSM_OK) goto fin;
    printf("matrices referencia_secuencial_s %.6f elementos %zu\n",
           fin_tiempo - inicio, m->filas * m->columnas);
    for (size_t i = 0; i < m->filas; i++) {
        for (size_t j = 0; j < m->columnas; j++) {
            size_t x = i * m->columnas + j;
            if (c[x] != referencia[x]) {
                rc = diferencia("C", i, j, referencia[x], c[x]);
                goto fin;
            }
        }
    }
    printf("matrices verificacion_completa OK elementos %zu muestra_C00 %.17g muestra_Cfinal %.17g\n",
           m->filas * m->columnas, c[0], c[m->filas * m->columnas - 1]);
fin:
    free(a); free(bt); free(b); free(referencia); free(c);
    return rc;
}

static int muestra(dsm_context_t *ctx, const matrices_t *m, size_t indice, double *valor)
{
    double obtenido;
    int rc = dsm_leer(ctx, m->c, indice * sizeof(double), &obtenido, sizeof obtenido);
    if (rc != DSM_OK) return rc;
    size_t i = indice / m->columnas, j = indice % m->columnas;
    double esperado = referencia_elemento(m, i, j);
    if (valor != NULL) *valor = obtenido;
    return obtenido == esperado ? DSM_OK : diferencia("C", i, j, esperado, obtenido);
}

static int verificar_grande(dsm_context_t *ctx, const matrices_t *m, const size_t bytes[3])
{
    if (m->filas > SIZE_MAX / (PROYECCIONES * sizeof(int64_t)) ||
        m->interior > SIZE_MAX / (PROYECCIONES * sizeof(int64_t)) ||
        m->columnas > SIZE_MAX / PROYECCIONES) {
        return DSM_ERR_NOMEM;
    }
    signed char *r = malloc(PROYECCIONES * m->columnas);
    int64_t *br = calloc(PROYECCIONES * m->interior, sizeof *br);
    int64_t *abr = calloc(PROYECCIONES * m->filas, sizeof *abr);
    int64_t *cr = calloc(PROYECCIONES * m->filas, sizeof *cr);
    double *pagina = malloc(m->page_size);
    int rc = DSM_OK;
    if (r == NULL || br == NULL || abr == NULL || cr == NULL || pagina == NULL) {
        rc = DSM_ERR_NOMEM;
        goto fin;
    }
    for (size_t ronda = 0; ronda < PROYECCIONES; ronda++) {
        for (size_t j = 0; j < m->columnas; j++) {
            r[ronda * m->columnas + j] = ronda == 0 ? 1 :
                (mezclar((uint64_t)j ^ mezclar(UINT64_C(2026) + ronda)) & 1u ? 1 : -1);
        }
    }
    dsm_reserva_id_t ids[3] = {m->b, m->a, m->c};
    size_t cantidades[3] = {bytes[1], bytes[0], bytes[2]};
    for (size_t matriz = 0; matriz < 3; matriz++) {
        for (size_t offset = 0; offset < cantidades[matriz];) {
            size_t n = cantidades[matriz] - offset;
            if (n > m->page_size) n = m->page_size;
            rc = dsm_leer(ctx, ids[matriz], offset, pagina, n);
            if (rc != DSM_OK) goto fin;
            for (size_t x = 0; x < n / sizeof(double); x++) {
                size_t indice = offset / sizeof(double) + x;
                size_t i, j;
                double esperado;
                if (matriz == 0) {
                    i = indice % m->interior; j = indice / m->interior;
                    esperado = matrices_dato_b(i, j);
                } else if (matriz == 1) {
                    i = indice / m->interior; j = indice % m->interior;
                    esperado = matrices_dato_a(i, j);
                } else {
                    i = indice / m->columnas; j = indice % m->columnas;
                    double limite = (double)m->interior * 64.0;
                    if (!isfinite(pagina[x]) || pagina[x] < -limite || pagina[x] > limite ||
                        (double)(int64_t)pagina[x] != pagina[x]) {
                        rc = diferencia("C", i, j, referencia_elemento(m, i, j), pagina[x]);
                        goto fin;
                    }
                    esperado = pagina[x];
                }
                if (pagina[x] != esperado) {
                    rc = diferencia(matriz == 0 ? "B" : "A", i, j, esperado, pagina[x]);
                    goto fin;
                }
                int64_t valor = (int64_t)pagina[x];
                for (size_t ronda = 0; ronda < PROYECCIONES; ronda++) {
                    if (matriz == 0) {
                        br[ronda * m->interior + i] += valor * r[ronda * m->columnas + j];
                    } else if (matriz == 1) {
                        abr[ronda * m->filas + i] += valor * br[ronda * m->interior + j];
                    } else {
                        cr[ronda * m->filas + i] += valor * r[ronda * m->columnas + j];
                    }
                }
            }
            offset += n;
        }
    }
    for (size_t ronda = 0; ronda < PROYECCIONES; ronda++) {
        for (size_t i = 0; i < m->filas; i++) {
            if (abr[ronda * m->filas + i] != cr[ronda * m->filas + i]) {
                fprintf(stderr, "verificacion ERROR proyeccion %zu fila %zu esperado %" PRId64
                        " obtenido %" PRId64 "\n", ronda, i,
                        abr[ronda * m->filas + i], cr[ronda * m->filas + i]);
                for (size_t j = 0; j < m->columnas; j++) {
                    rc = muestra(ctx, m, i * m->columnas + j, NULL);
                    if (rc != DSM_OK) goto fin;
                }
                rc = DSM_ERR_ESTADO;
                goto fin;
            }
        }
    }
    size_t muestras = 0;
    for (size_t i = 0; i < m->filas; i++) {
        size_t j = (size_t)(mezclar((uint64_t)i ^ UINT64_C(2028)) % m->columnas);
        rc = muestra(ctx, m, i * m->columnas + j, NULL);
        if (rc != DSM_OK) goto fin;
        muestras++;
    }
    size_t bloques = 1 + (bytes[2] - 1) / m->page_size;
    for (size_t p = 0; p < bloques; p++) {
        rc = muestra(ctx, m, p * (m->page_size / sizeof(double)), NULL);
        if (rc != DSM_OK) goto fin;
        muestras++;
    }
    size_t elementos = m->filas * m->columnas;
    size_t indices[3] = {0, elementos / 2, elementos - 1};
    for (size_t i = 0; i < 3; i++) {
        double valor;
        rc = muestra(ctx, m, indices[i], &valor);
        if (rc != DSM_OK) goto fin;
        printf("matrices muestra C[%zu,%zu] %.17g\n",
               indices[i] / m->columnas, indices[i] % m->columnas, valor);
    }
    printf("matrices verificacion_grande OK elementos %zu proyecciones %u muestras %zu"
           " entradas_deterministas_completas\n", elementos, PROYECCIONES, muestras + 3);
fin:
    free(r); free(br); free(abr); free(cr); free(pagina);
    return rc;
}

int matrices_verificar(dsm_context_t *ctx, const matrices_t *m)
{
    size_t bytes[3];
    int rc = validar(m, bytes);
    if (ctx == NULL || rc != DSM_OK) return ctx == NULL ? DSM_ERR_INVAL : rc;
    double inicio, fin;
    rc = instante(&inicio);
    if (rc != DSM_OK) return rc;
    if (m->filas <= 256 && m->interior <= 256 && m->columnas <= 256) {
        rc = verificar_pequena(ctx, m, bytes);
    } else {
        rc = verificar_grande(ctx, m, bytes);
    }
    if (instante(&fin) == DSM_OK) {
        printf("matrices verificacion_s %.6f resultado %d\n", fin - inicio, rc);
    }
    return rc;
}
