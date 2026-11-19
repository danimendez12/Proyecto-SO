#ifndef DSM_MATRICES_CALCULO_H
#define DSM_MATRICES_CALCULO_H

#include "dsm.h"

#include <stddef.h>

enum { MATRICES_HILOS_MAX = 64 };

/* A[filas][interior], B_t[columnas][interior], C[filas][columnas].
 * Solo se comparten doubles; los identificadores y punteros son locales. */
typedef struct matrices {
    size_t filas;
    size_t interior;
    size_t columnas;
    size_t page_size;
    dsm_reserva_id_t a;
    dsm_reserva_id_t b;
    dsm_reserva_id_t c;
} matrices_t;

/* Devuelven los codigos publicos DSM. tamanos funciona antes de asignar IDs. */
int matrices_tamanos(const matrices_t *m, size_t bytes[3]);
double matrices_dato_a(size_t fila, size_t interior);
double matrices_dato_b(size_t interior, size_t columna);

/* Inicializacion determinista y verificacion usan copias controladas y no
 * requieren que el hilo llamador este registrado. C comienza con NAN. */
int matrices_inicializar(dsm_context_t *ctx, const matrices_t *m);
int matrices_verificar(dsm_context_t *ctx, const matrices_t *m);

/* Rangos [primer_bloque, fin_bloque), en paginas de C. El coordinador evita
 * solaparlos entre procesos. Cada Pthread registra su propio hilo, lee A/B
 * por Ar y publica una pagina de C por dsm_escribir. A/B permanecen estables. */
int matrices_calcular(dsm_context_t *ctx, dsm_proc_id_t proc, const matrices_t *m,
                       size_t primer_bloque, size_t fin_bloque, size_t hilos);

#endif
