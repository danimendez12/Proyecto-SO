#ifndef DSM_DSM_H
#define DSM_DSM_H

#include <stddef.h>
#include <stdint.h>

typedef struct dsm_context dsm_context_t;
typedef uint16_t dsm_node_id_t;
typedef uint32_t dsm_proc_id_t;
typedef uint32_t dsm_reserva_id_t;

enum {
    DSM_OK = 0,
    DSM_ERR_INVAL = -1,
    DSM_ERR_NOMEM = -2,
    DSM_ERR_NODO = -3,
    DSM_ERR_TIEMPO = -4,
    DSM_ERR_ESTADO = -5,
    DSM_ERR_CONFIG = -6,
    DSM_ERR_PROTO = -7,
    DSM_ERR_CERRADO = -8,
    DSM_ERR_OCUPADA = -9,
    DSM_ERR_HILOS = -10
};

/* Un contexto activo por proceso. El cierre exitoso consume ctx. */
int dsm_iniciar(const char *ruta_config, dsm_node_id_t nodo, dsm_context_t **salida);
int dsm_cerrar(dsm_context_t *ctx);

/* Un ID de proceso logico. Cada trabajador se registra y desregistra en su hilo. */
int dsm_proc_abrir(dsm_context_t *ctx, dsm_proc_id_t *salida);
int dsm_hilo_registrar(dsm_context_t *ctx, dsm_proc_id_t proceso);
int dsm_hilo_desregistrar(dsm_context_t *ctx);

/* La reserva conserva bytes logicos y tiene una vista contigua redondeada a paginas. */
int dsm_reservar(dsm_context_t *ctx, dsm_proc_id_t proceso, size_t bytes,
                 dsm_reserva_id_t *id, void **base);
/* Abre la reserva del creador por TCP o devuelve la vista ya abierta en este
 * contexto. bytes recibe el tamano logico original. Una apertura repetida
 * conserva una sola vinculacion por nodo y reserva. */
int dsm_abrir_reserva(dsm_context_t *ctx, dsm_reserva_id_t id, void **base, size_t *bytes);
/* Consulta el hogar y el numero global de una pagina logica de la vista abierta. */
int dsm_ubicacion_pagina(dsm_context_t *ctx, dsm_reserva_id_t id, size_t orden,
                         dsm_node_id_t *hogar, uint64_t *gpn);
/* Desregistrar los trabajadores antes de desmontar. El cliente desvincula su
 * vista y entrega las paginas modificadas. El creador solo puede liberar
 * cuando todos los clientes han confirmado la desvinculacion. */
int dsm_desvincular_reserva(dsm_context_t *ctx, dsm_reserva_id_t id);
int dsm_liberar(dsm_context_t *ctx, dsm_reserva_id_t id);

int dsm_leer(dsm_context_t *ctx, dsm_reserva_id_t id, size_t offset,
              void *destino, size_t bytes);
int dsm_escribir(dsm_context_t *ctx, dsm_reserva_id_t id, size_t offset,
                  const void *origen, size_t bytes);
/* Los punteros prestados se usan desde hilos registrados hasta liberar o cerrar. */
int dsm_traducir(dsm_context_t *ctx, dsm_reserva_id_t id, size_t offset, void **salida);

#endif
