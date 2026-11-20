#ifndef MERGESORT_DATOS_H
#define MERGESORT_DATOS_H

#include "dsm.h"
#include <stdio.h>

/* Copia privada, anónima y exclusiva del verificador. Los trabajadores reciben
 * los enteros por DSM. Cada función deja el cursor en posición no especificada. */
int datos_preparar(size_t n, const char *patron, uint32_t semilla,
                   const char *archivo, FILE **original);
int datos_inicializar(dsm_context_t *ctx, dsm_reserva_id_t id, size_t n, FILE *original);
int datos_verificar(dsm_context_t *ctx, dsm_reserva_id_t id, size_t n, FILE *original);
int datos_secuencial(size_t n, FILE *original);
double datos_reloj(void);

#endif
