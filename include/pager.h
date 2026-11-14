#ifndef DSM_PAGER_H
#define DSM_PAGER_H

#include "memory.h"
#include "protocol.h"
#include "signals.h"

#include <time.h>

/* Interfaz interna del paginador. */
typedef struct dsm_pager dsm_pager_t;

/* El paginador ejecuta esta funcion para llenar una pagina por Cr.
 * La funcion devuelve 0 en caso de exito y evita acceder a Ar.
 */
typedef int (*dsm_pager_preparar_t)(void *pagina_cr, size_t indice, size_t bytes, void *datos);

/* Ar debe comenzar en PROT_NONE. Las vistas y los datos viven hasta el cierre.
 * Solo hay un paginador activo por proceso. Entrada compatible de lectura.
 */
int dsm_pager_iniciar(dsm_pager_t **salida, dsm_vistas_t *vistas, size_t page_size,
                      size_t max_trabajadores, dsm_pager_preparar_t preparar, void *datos);

/* Adaptador interno. Los nombres y tipos del protocolo de coherencia se
 * acuerdan por separado. Una concesion se refiere a la peticion recibida.
 */
typedef struct dsm_pager_pagina {
    uint32_t reserva_id;
    uint64_t gpn;
    uint32_t version;
    uint8_t msi;
} dsm_pager_pagina_t;

typedef struct dsm_pager_peticion {
    dsm_pager_pagina_t pagina;
    uint16_t solicitante;
    dsm_acceso_t acceso;
    struct timespec plazo; /* Tiempo absoluto en CLOCK_MONOTONIC. */
} dsm_pager_peticion_t;

typedef struct dsm_pager_concesion {
    uint32_t version;
    size_t bytes_validos;
    int exclusiva;
} dsm_pager_concesion_t;

/* El adaptador valida origen, identidad, correlacion y version de la respuesta.
 * Devuelve 0 con la concesion, o -1 con errno, antes del plazo. buf es memoria
 * temporal para la imagen completa. Una escritura sin imagen solo es valida
 * desde S si el proveedor confirma que esa copia local sigue vigente.
 * Si llega una imagen desde S, debe coincidir byte a byte con Cr.
 * exclusiva vale 1 solo si el hogar otorgo M tras confirmar las invalidaciones.
 * El adaptador usa el plazo para acotar su espera y evita acceder a Ar.
 */
typedef int (*dsm_pager_coherencia_t)(const dsm_pager_peticion_t *peticion,
                                       void *buf, size_t capacidad,
                                       dsm_pager_concesion_t *concesion, void *contexto);

/* Ar comienza en PROT_NONE y Cr contiene los datos de las paginas S y M.
 * Recibe un descriptor por pagina, en orden, y aplica sus permisos iniciales.
 * El paginador es el unico que cambia estos permisos durante su uso.
 */
int dsm_pager_iniciar_msi(dsm_pager_t **salida, dsm_vistas_t *vistas, size_t page_size,
                          size_t max_trabajadores, const dsm_pager_pagina_t *paginas,
                          uint16_t solicitante, uint32_t timeout_ms,
                          dsm_pager_coherencia_t resolver, void *contexto);

int dsm_pager_consultar_pagina(dsm_pager_t *pager, size_t indice, dsm_pager_pagina_t *pagina);

/* Registrar y desregistrar desde el propio trabajador. Mientras este registrado,
 * SIGUSR1 queda bloqueada y reservada para el paginador. Se restaura su bit
 * original al desregistrar. Desregistrar antes de salir del hilo o cancelarlo.
 */
int dsm_pager_registrar(dsm_pager_t *pager);
int dsm_pager_desregistrar(dsm_pager_t *pager);

/* Cerrar con todos los trabajadores desregistrados y sin otras llamadas activas. */
int dsm_pager_cerrar(dsm_pager_t *pager);

#endif
