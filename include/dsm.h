#ifndef DSM_DSM_H
#define DSM_DSM_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

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
    DSM_ERR_HILOS = -10,
    DSM_ERR_PARCIAL = -11
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
/* Estado de la vinculación en este contexto, sin RPC. INVAL si el ID no está
 * vinculado. Una retirada o preparación conserva la identidad y no presta Ar. */
typedef struct dsm_reserva_estado {
    dsm_reserva_id_t id;
    size_t bytes;
    void *base;
    int retirando, provisional;
} dsm_reserva_estado_t;
int dsm_consultar_reserva(dsm_context_t *ctx, dsm_reserva_id_t id, dsm_reserva_estado_t *salida);
/* Consulta el hogar y el numero global de una pagina logica de la vista abierta. */
int dsm_ubicacion_pagina(dsm_context_t *ctx, dsm_reserva_id_t id, size_t orden,
                         dsm_node_id_t *hogar, uint64_t *gpn);
/* Desregistrar los trabajadores antes de desmontar. El cliente desvincula su
 * vista y entrega las paginas modificadas. El creador solo puede liberar
 * cuando todos los clientes han confirmado la desvinculacion. */
int dsm_desvincular_reserva(dsm_context_t *ctx, dsm_reserva_id_t id);
int dsm_liberar(dsm_context_t *ctx, dsm_reserva_id_t id);

/* Validan el rango antes de copiar; bytes == 0 admite un puntero nulo.
 * Una lectura hacia memoria ordinaria conserva el destino ante un error DSM.
 * Las escrituras y las lecturas hacia otra vista DSM avanzan por trozos: un
 * error puede dejar parte del destino modificada. Escribir intenta restaurar
 * localmente solo los trozos que aun conserva en M, en su misma version y
 * con los bytes que escribio; no pide permisos ni reemplaza cambios ajenos.
 * Si no puede restaurar todo, devuelve igualmente el fallo original.
 * Admiten solapamiento como memmove y registran temporalmente el hilo si hace
 * falta. No ofrecen una transaccion ni una instantanea distribuida; el acceso
 * directo Ar local solapado y los buffers ordinarios requieren sincronizacion
 * del llamador durante toda la operacion, incluida la restauracion. */
int dsm_leer(dsm_context_t *ctx, dsm_reserva_id_t id, size_t offset,
              void *destino, size_t bytes);
int dsm_escribir(dsm_context_t *ctx, dsm_reserva_id_t id, size_t offset,
                  const void *origen, size_t bytes);

enum {
    DSM_CONSULTA_CONTADORES = 8,
    DSM_CONSULTA_NOMBRE = 64,
    DSM_HOGAR_TODOS = 0xFFFF,
    DSM_CONSULTA_RANGO_MAX = 1024,
    DSM_ASIGNACION_ASIGNADA = 0,
    DSM_ASIGNACION_LIBRE = 1,
    DSM_ASIGNACION_PREPARADA = 2,
    DSM_DISP_VACIO = 0,
    DSM_DISP_UNION = 1,
    DSM_DISP_EN_LINEA = 2,
    DSM_DISP_SOSPECHA = 3,
    DSM_DISP_FUERA = 4
};

/* Contadores, en este orden: fallos de lectura, fallos de escritura,
 * invalidaciones enviadas, invalidaciones recibidas, páginas enviadas,
 * páginas recibidas, bytes enviados y bytes recibidos. */
typedef struct dsm_nodo_info {
    uint16_t id;
    uint8_t estado;
    uint32_t ipv4_host;
    uint16_t puerto;
    uint64_t memoria;
    uint64_t base;
    uint32_t libres;
    uint32_t totales;
    uint32_t aportados;
    char nombre[DSM_CONSULTA_NOMBRE];
    uint64_t contadores[DSM_CONSULTA_CONTADORES];
    uint8_t respondio; /* ocupacion/counters desconocidos si vale cero */
} dsm_nodo_info_t;

typedef struct dsm_pagina_info {
    uint64_t gpn;
    uint64_t mascara;
    uint32_t proc; /* ultimo solicitante, no el propietario */
    uint32_t reserva;
    uint32_t orden;
    uint16_t hogar;
    uint16_t poseedor;
    uint8_t msi;
    uint8_t permiso;
    uint8_t copias;
    uint32_t memoria_libre; /* paginas libres del hogar; UINT32_MAX si desconocidas */
    uint32_t version;
    uint16_t creador;       /* 0xffff en pagina libre */
    uint32_t propietario;  /* ID logico estable; 0 en procedencia historica */
    uint8_t publica;
    uint8_t asignacion;    /* 0 asignada, 1 libre, 2 preparada; independiente de MSI */
} dsm_pagina_info_t;

/* Si cap no alcanza, escribe solo las primeras cap entradas, informa el total
 * en cantidad y devuelve DSM_ERR_NOMEM. cap 0 con salida NULL solo cuenta. */
int dsm_consultar_nodos(dsm_context_t *ctx, dsm_nodo_info_t *salida, size_t cap,
                        size_t *cantidad);
int dsm_consultar_nodo(dsm_context_t *ctx, uint16_t nodo, dsm_nodo_info_t *salida);
/* Una fila por página en su hogar. DSM_ERR_PARCIAL con TODOS entrega las
 * filas disponibles si algún puesto está fuera. Un hogar explícito devuelve
 * su error. Las observaciones de distintos hogares no son simultáneas. */
int dsm_consultar_paginas(dsm_context_t *ctx, uint16_t hogar, dsm_pagina_info_t *salida,
                          size_t cap, size_t *cantidad);
/* salida admite limite entradas (1..1024). desde y siguiente son GPN. Una
 * pagina de resultados no es una instantanea de otras consultas. copias=1
 * solo permite el nodo local; copias=0 incluye bitmap libre/preparado/asignado.
 * TODOS devuelve PARCIAL si falta un hogar. mas nunca implica truncado oculto. */
int dsm_consultar_rango(dsm_context_t *ctx, uint16_t hogar, int copias,
                        uint64_t desde, size_t limite, dsm_pagina_info_t *salida,
                        size_t *cantidad, uint64_t *siguiente, int *mas);
/* global distinto de cero incluye los demás nodos. */
int dsm_imprimir(dsm_context_t *ctx, FILE *salida, int global);

/* Ocupa el primer puesto del pool cuya IPv4 es local y cuyo puerto está libre.
 * Un reclamo por proceso; el cierre DSM exitoso lo devuelve. */
int dsm_pool_reclamar(const char *ruta, uint16_t *id);
void dsm_pool_liberar(void);
/* Ronda inicial de descubrimiento: reintenta puestos ausentes durante un
 * plazo comun y termina antes si todos estan conectados. Los ausentes al
 * vencer el plazo son normales. No acuerda membresia entre particiones. */
int dsm_sondear(dsm_context_t *ctx, uint32_t plazo_ms);
/* Avisos por autor y sesion, difundidos asincronamente en la malla TCP.
 * Todos los participantes de las apps usan esta misma revision. Una clave
 * tiene 1..31 bytes, datos 0..128. Tabla acotada a 128 entradas por contexto;
 * las retiradas se reciclan al entregarse. Saturacion local devuelve NOMEM
 * sin cerrar conexiones.
 * Consultar observa solo autores de una sesion actualmente conectada.
 * Publicar no es una barrera de entrega ni un consenso de participantes. */
int dsm_aviso_publicar(dsm_context_t *ctx, const char *clave, const void *datos, size_t n);
int dsm_aviso_consultar(dsm_context_t *ctx, const char *clave, uint16_t *autor,
                        void *datos, size_t n);
int dsm_aviso_participantes(dsm_context_t *ctx, const char *clave, uint16_t *autores,
                            size_t cap, size_t *cantidad);
int dsm_aviso_consultar_autor(dsm_context_t *ctx, const char *clave,
                              uint16_t autor, void *datos, size_t n);
int dsm_aviso_retirar(dsm_context_t *ctx, const char *clave);
int dsm_aviso_sesion(dsm_context_t *ctx, uint16_t autor, uint64_t *sesion);
/* Los punteros prestados se usan desde hilos registrados hasta liberar o cerrar. */
int dsm_traducir(dsm_context_t *ctx, dsm_reserva_id_t id, size_t offset, void **salida);

#endif
