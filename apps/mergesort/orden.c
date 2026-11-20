/* Adaptacion de sorting.c de Brandon Lavello, Copyright (c) 2019.
 * MIT: LICENSE. Se conservan la division en tareas Pthreads y la combinacion
 * mergeSort; Bubble Sort se sustituye por Merge Sort iterativo O(n log n).
 */
#include "orden.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* Adaptacion reconocible de mergeSort(leftArr,leftSize,rightArr,rightSize,
 * sorted) del original: dos cursores, comparacion < y agotamiento de colas.
 * Se mantiene la eleccion del lado derecho cuando los valores son iguales. */
static void mergeSort(const int32_t *leftArr, size_t leftSize,
                       const int32_t *rightArr, size_t rightSize, int32_t *sorted)
{
    size_t i = 0, j = 0, k = 0;
    while (j < leftSize && k < rightSize) {
        if (leftArr[j] < rightArr[k]) {
            sorted[i++] = leftArr[j++];
        } else {
            sorted[i++] = rightArr[k++];
        }
    }
    while (j < leftSize) sorted[i++] = leftArr[j++];
    while (k < rightSize) sorted[i++] = rightArr[k++];
}

void orden_secuencial(int32_t *a, int32_t *aux, size_t n)
{
    if (n < 2) return;
    int32_t *origen = a, *destino = aux;
    for (size_t ancho = 1; ancho < n;) {
        for (size_t inicio = 0; inicio < n;) {
            size_t izquierda = n - inicio;
            if (izquierda > ancho) izquierda = ancho;
            size_t derecha = n - inicio - izquierda;
            if (derecha > ancho) derecha = ancho;
            mergeSort(origen + inicio, izquierda, origen + inicio + izquierda,
                       derecha, destino + inicio);
            inicio += izquierda + derecha;
        }
        int32_t *intercambio = origen;
        origen = destino;
        destino = intercambio;
        if (ancho >= n - ancho) break;
        ancho *= 2;
    }
    if (origen != a) memcpy(a, origen, n * sizeof *a);
}

int orden_tamanos(const orden_t *o, size_t *bytes, size_t *elementos_run)
{
    if (o == NULL || bytes == NULL || elementos_run == NULL || o->elementos == 0 ||
        o->page_size < sizeof(int32_t) || o->page_size % sizeof(int32_t) != 0 ||
        o->paginas_bloque == 0 || o->paginas_bloque > ORDEN_PAGINAS_BLOQUE_MAX) {
        return DSM_ERR_INVAL;
    }
    if (o->elementos > SIZE_MAX / sizeof(int32_t) ||
        o->page_size > SIZE_MAX / o->paginas_bloque) {
        return DSM_ERR_NOMEM;
    }
    *bytes = o->elementos * sizeof(int32_t);
    *elementos_run = o->paginas_bloque * (o->page_size / sizeof(int32_t));
    return DSM_OK;
}

static int validar(dsm_context_t *ctx, dsm_proc_id_t proc, const orden_t *o,
                    size_t *bytes, size_t *run)
{
    int rc = orden_tamanos(o, bytes, run);
    if (rc != DSM_OK) return rc;
    if (ctx == NULL || proc == 0 || o->a == 0 || o->b == 0 || o->a == o->b) {
        return DSM_ERR_INVAL;
    }
    return DSM_OK;
}

struct lector {
    dsm_context_t *ctx;
    dsm_reserva_id_t reserva;
    size_t siguiente;
    size_t fin;
    size_t capacidad;
    size_t cargados;
    size_t indice;
    int32_t *buffer;
};

static int rellenar(struct lector *l)
{
    if (l->indice < l->cargados || l->siguiente == l->fin) return DSM_OK;
    size_t n = l->fin - l->siguiente;
    if (n > l->capacidad) n = l->capacidad;
    int rc = dsm_leer(l->ctx, l->reserva, l->siguiente * sizeof(int32_t),
                       l->buffer, n * sizeof(int32_t));
    if (rc != DSM_OK) return rc;
    l->siguiente += n;
    l->cargados = n;
    l->indice = 0;
    return DSM_OK;
}

/* La misma seleccion de mergeSort, con cursores que se recargan por pagina.
 * No se copia un run completo al heap, incluso en la fusion global final. */
static int mergeSort_dsm(dsm_context_t *ctx, const orden_t *o,
                          dsm_reserva_id_t origen, dsm_reserva_id_t destino,
                          size_t inicio, size_t medio, size_t fin,
                          int32_t *izquierda, int32_t *derecha, int32_t *salida)
{
    size_t capacidad = o->page_size / sizeof(int32_t);
    struct lector l = {.ctx = ctx, .reserva = origen, .siguiente = inicio,
                       .fin = medio, .capacidad = capacidad, .buffer = izquierda};
    struct lector r = {.ctx = ctx, .reserva = origen, .siguiente = medio,
                       .fin = fin, .capacidad = capacidad, .buffer = derecha};
    size_t escritos = inicio, usados = 0;
    while (escritos + usados < fin) {
        int rc = rellenar(&l);
        if (rc == DSM_OK) rc = rellenar(&r);
        if (rc != DSM_OK) return rc;
        int tiene_l = l.indice < l.cargados;
        int tiene_r = r.indice < r.cargados;
        if (!tiene_l && !tiene_r) return DSM_ERR_ESTADO;
        if (tiene_l && (!tiene_r || l.buffer[l.indice] < r.buffer[r.indice])) {
            salida[usados++] = l.buffer[l.indice++];
        } else {
            salida[usados++] = r.buffer[r.indice++];
        }
        if (usados == capacidad || escritos + usados == fin) {
            rc = dsm_escribir(ctx, destino, escritos * sizeof(int32_t),
                               salida, usados * sizeof(int32_t));
            if (rc != DSM_OK) return rc;
            escritos += usados;
            usados = 0;
        }
    }
    return DSM_OK;
}

enum tipo_tarea { ORDENAR_RUN, FUSIONAR_RUNS };

struct tarea {
    dsm_context_t *ctx;
    dsm_proc_id_t proc;
    const orden_t *o;
    enum tipo_tarea tipo;
    dsm_reserva_id_t origen, destino;
    size_t ancho;
    size_t inicio, fin;
    int rc;
};

static void *worker(void *arg)
{
    struct tarea *t = arg;
    t->rc = dsm_hilo_registrar(t->ctx, t->proc);
    if (t->rc != DSM_OK) return NULL;
    size_t capacidad = t->tipo == ORDENAR_RUN ? t->ancho :
                        t->o->page_size / sizeof(int32_t);
    size_t bytes = capacidad * sizeof(int32_t);
    int32_t *izquierda = malloc(bytes), *derecha = malloc(bytes);
    int32_t *salida = t->tipo == FUSIONAR_RUNS ? malloc(bytes) : NULL;
    if (izquierda == NULL || derecha == NULL ||
        (t->tipo == FUSIONAR_RUNS && salida == NULL)) {
        t->rc = DSM_ERR_NOMEM;
    }
    for (size_t i = t->inicio; i < t->fin && t->rc == DSM_OK; i++) {
        if (t->tipo == ORDENAR_RUN) {
            size_t inicial = i * t->ancho;
            size_t n = t->o->elementos - inicial;
            if (n > t->ancho) n = t->ancho;
            t->rc = dsm_leer(t->ctx, t->o->a, inicial * sizeof(int32_t), izquierda, n * sizeof(int32_t));
            if (t->rc == DSM_OK) {
                orden_secuencial(izquierda, derecha, n);
                t->rc = dsm_escribir(t->ctx, t->o->a, inicial * sizeof(int32_t), izquierda, n * sizeof(int32_t));
            }
        } else {
            size_t inicial = i * (2 * t->ancho);
            size_t n_izquierda = t->o->elementos - inicial;
            if (n_izquierda > t->ancho) n_izquierda = t->ancho;
            size_t n_derecha = t->o->elementos - inicial - n_izquierda;
            if (n_derecha > t->ancho) n_derecha = t->ancho;
            t->rc = mergeSort_dsm(t->ctx, t->o, t->origen, t->destino, inicial,
                                   inicial + n_izquierda, inicial + n_izquierda + n_derecha,
                                   izquierda, derecha, salida);
        }
    }
    free(izquierda);
    free(derecha);
    free(salida);
    int rc = dsm_hilo_desregistrar(t->ctx);
    if (t->rc == DSM_OK) t->rc = rc;
    return NULL;
}

static int ejecutar(struct tarea modelo, size_t hilos)
{
    if (hilos == 0 || hilos > ORDEN_HILOS_MAX || modelo.inicio > modelo.fin) {
        return DSM_ERR_INVAL;
    }
    if (modelo.inicio == modelo.fin) return DSM_OK;
    if (hilos > modelo.fin - modelo.inicio) hilos = modelo.fin - modelo.inicio;
    struct tarea *tareas = calloc(hilos, sizeof *tareas);
    pthread_t *threads = calloc(hilos, sizeof *threads);
    if (tareas == NULL || threads == NULL) {
        free(tareas); free(threads);
        return DSM_ERR_NOMEM;
    }
    int rc = DSM_OK;
    size_t creados = 0;
    size_t delta = (modelo.fin - modelo.inicio) / hilos;
    for (size_t i = 0; i < hilos && rc == DSM_OK; i++) {
        tareas[i] = modelo;
        tareas[i].inicio = modelo.inicio + i * delta;
        tareas[i].fin = i == hilos - 1 ? modelo.fin : modelo.inicio + (i + 1) * delta;
        if (pthread_create(&threads[i], NULL, worker, &tareas[i]) != 0) {
            rc = DSM_ERR_HILOS;
            break;
        }
        creados++;
    }
    for (size_t i = 0; i < creados; i++) {
        int unido = pthread_join(threads[i], NULL);
        if (rc == DSM_OK) rc = unido == 0 ? tareas[i].rc : DSM_ERR_ESTADO;
    }
    free(tareas); free(threads);
    return rc;
}

int orden_bloques(dsm_context_t *ctx, dsm_proc_id_t proc, const orden_t *o,
                   size_t primer_run, size_t fin_run, size_t hilos)
{
    size_t bytes, run;
    int rc = validar(ctx, proc, o, &bytes, &run);
    if (rc != DSM_OK) return rc;
    size_t tareas = 1 + (o->elementos - 1) / run;
    if (primer_run > fin_run || fin_run > tareas) return DSM_ERR_INVAL;
    struct tarea modelo = {.ctx = ctx, .proc = proc, .o = o, .tipo = ORDENAR_RUN,
                           .origen = o->a, .destino = o->a, .ancho = run,
                           .inicio = primer_run, .fin = fin_run};
    return ejecutar(modelo, hilos);
}

int orden_fase(dsm_context_t *ctx, dsm_proc_id_t proc, const orden_t *o,
                dsm_reserva_id_t origen, dsm_reserva_id_t destino, size_t ancho_run,
                size_t primera_tarea, size_t fin_tarea, size_t hilos)
{
    size_t bytes, run;
    int rc = validar(ctx, proc, o, &bytes, &run);
    if (rc != DSM_OK) return rc;
    if (origen == destino || (origen != o->a && origen != o->b) ||
        (destino != o->a && destino != o->b) || ancho_run < run || ancho_run % run != 0) {
        return DSM_ERR_INVAL;
    }
    size_t razon = ancho_run / run;
    if ((razon & (razon - 1)) != 0) return DSM_ERR_INVAL;
    if (ancho_run > SIZE_MAX / 2) return DSM_ERR_NOMEM;
    size_t tareas = 1 + (o->elementos - 1) / (2 * ancho_run);
    if (primera_tarea > fin_tarea || fin_tarea > tareas) return DSM_ERR_INVAL;
    struct tarea modelo = {.ctx = ctx, .proc = proc, .o = o, .tipo = FUSIONAR_RUNS,
                           .origen = origen, .destino = destino, .ancho = ancho_run,
                           .inicio = primera_tarea, .fin = fin_tarea};
    return ejecutar(modelo, hilos);
}
