#ifndef DSM_MERGESORT_ORDEN_H
#define DSM_MERGESORT_ORDEN_H

#include "dsm.h"

#include <stddef.h>
#include <stdint.h>

enum { ORDEN_HILOS_MAX = 64, ORDEN_PAGINAS_BLOQUE_MAX = 16 };

typedef struct orden {
    size_t elementos;
    size_t page_size;
    size_t paginas_bloque;
    dsm_reserva_id_t a;
    dsm_reserva_id_t b;
} orden_t;

/* Valida tamanos sin exigir IDs asignados. Cada reserva tiene *bytes logicos;
 * cada run inicial contiene hasta *elementos_run enteros. Devuelve DSM_ERR_*. */
int orden_tamanos(const orden_t *o, size_t *bytes, size_t *elementos_run);

/* Referencia secuencial de rendimiento. a y aux contienen n int32_t y no se
 * solapan. No utiliza DSM ni Pthreads; la verificacion independiente usa qsort. */
void orden_secuencial(int32_t *a, int32_t *aux, size_t n);

/* Ordena en A los runs iniciales [primer_run,fin_run). La aplicacion asigna
 * rangos disjuntos entre procesos. Usa hasta dos bloques locales por Pthread. */
int orden_bloques(dsm_context_t *ctx, dsm_proc_id_t proc, const orden_t *o,
                   size_t primer_run, size_t fin_run, size_t hilos);

/* Fusiona pares de runs desde origen hacia destino, que deben ser A/B
 * diferentes. ancho_run se mide en elementos y duplica su valor por fase.
 * La tarea t escribe [2*t*ancho_run,min(N,2*(t+1)*ancho_run)); si no existe
 * el segundo run, copia el primero. Usa tres paginas locales por Pthread.
 * La barrera entre fases y los rangos no solapados pertenecen al coordinador. */
int orden_fase(dsm_context_t *ctx, dsm_proc_id_t proc, const orden_t *o,
                dsm_reserva_id_t origen, dsm_reserva_id_t destino, size_t ancho_run,
                size_t primera_tarea, size_t fin_tarea, size_t hilos);

#endif
