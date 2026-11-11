#ifndef DSM_PAGER_H
#define DSM_PAGER_H

#include "memory.h"

/* Interfaz interna para la primera fase del paginador local. */
typedef struct dsm_pager dsm_pager_t;

/* El paginador ejecuta esta funcion para llenar una pagina por Cr.
 * La funcion devuelve 0 en caso de exito y evita acceder a Ar.
 */
typedef int (*dsm_pager_preparar_t)(void *pagina_cr, size_t indice, size_t bytes, void *datos);

/* Ar debe comenzar en PROT_NONE. Las vistas y los datos viven hasta el cierre.
 * Solo hay un paginador activo por proceso y esta fase habilita lecturas.
 */
int dsm_pager_iniciar(dsm_pager_t **salida, dsm_vistas_t *vistas, size_t page_size,
                      size_t max_trabajadores, dsm_pager_preparar_t preparar, void *datos);

/* Registrar y desregistrar desde el propio trabajador. Mientras este registrado,
 * SIGUSR1 queda bloqueada y reservada para el paginador. Se restaura su bit
 * original al desregistrar. Desregistrar antes de salir del hilo o cancelarlo.
 */
int dsm_pager_registrar(dsm_pager_t *pager);
int dsm_pager_desregistrar(dsm_pager_t *pager);

/* Cerrar con todos los trabajadores desregistrados y sin otras llamadas activas. */
int dsm_pager_cerrar(dsm_pager_t *pager);

#endif
