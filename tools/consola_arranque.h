#ifndef DSM_CONSOLA_ARRANQUE_H
#define DSM_CONSOLA_ARRANQUE_H

#include <stdint.h>

/* Un resultado de arranque por hijo. No es un servicio IPC de aplicaciones. */
enum { CONSOLA_ALTA_MAGIC = 0x43414c31 };
typedef struct consola_alta {
    uint32_t magic;
    int32_t resultado;
    int32_t pid;
    uint16_t nodo, puerto;
} consola_alta_t;

#endif
