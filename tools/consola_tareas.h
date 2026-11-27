#ifndef DSM_CONSOLA_TAREAS_H
#define DSM_CONSOLA_TAREAS_H

#include "dsm.h"

#include <stdint.h>
#include <stdio.h>

/* Escribe y verifica (VALOR xor los bytes del offset absoluto). El caller
 * conserva la reserva y espera el retorno antes de liberarla. Ar se reserva
 * para demostraciones sanas: un fallo de coherencia puede terminar por señal. */
int consola_tarea_patron(dsm_context_t *ctx, dsm_proc_id_t proceso,
                         dsm_reserva_id_t id, size_t bytes, size_t pagina,
                         size_t hilos, uint8_t valor, int ar, FILE *salida);

#endif
