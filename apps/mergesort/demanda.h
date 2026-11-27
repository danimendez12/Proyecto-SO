#ifndef MERGESORT_DEMANDA_H
#define MERGESORT_DEMANDA_H

#include <stddef.h>
#include <stdint.h>

/* Mismo comando en cada terminal. El puesto de menor ID entre los que se
 * anuncian crea las reservas; los demás las abren y toman su tramo. */
int mergesort_demanda(const char *ini, size_t n, const char *patron, uint32_t semilla);

#endif
