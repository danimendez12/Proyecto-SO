#ifndef DSM_DSM_H
#define DSM_DSM_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/**
 * @file dsm.h
 * @brief Interfaz pública del nodo de memoria compartida distribuida.
 * Las longitudes y desplazamientos se expresan en bytes salvo que se indiquen páginas.
 * En las operaciones sobre un contexto, entrar() toma el mutex del API para
 * comprobar el puntero ctx. Un ctx nulo devuelve DSM_ERR_INVAL. Si el contexto indicado no
 * es el activo se devuelve DSM_ERR_CERRADO. Tras un cierre exitoso, descartar ctx
 * y todos los punteros prestados por ese contexto.
 * @warning Estas funciones se llaman desde hilos de aplicación. El manejador
 * de SIGSEGV usa su ranura preasignada y el semáforo del paginador, sin entrar al API.
 */

/** Contexto opaco de un puesto. Reúne su directorio, coherencia, paginador,
 * proceso lógico y vinculaciones locales. dsm_iniciar() lo crea y dsm_cerrar() lo consume. */
typedef struct dsm_context dsm_context_t;
/** Índice del puesto en el INI, desde cero. Un mismo equipo puede tener varios puestos. */
typedef uint16_t dsm_node_id_t;
/** Proceso lógico del API. Los 16 bits altos identifican al nodo y los bajos valen
 * uno en el modelo actual. Cero indica que aún no se abrió el proceso. */
typedef uint32_t dsm_proc_id_t;
/** ID de reserva. Combina el nodo creador en los 16 bits altos y un consecutivo
 * local distinto de cero en los bajos. Un reinicio puede reutilizar un ID. */
typedef uint32_t dsm_reserva_id_t;

/** Resultados del API. El contrato de cada función precisa qué salidas quedan disponibles. */
enum {
    DSM_OK = 0, /**< Operación completada según su contrato. */
    DSM_ERR_INVAL = -1, /**< Argumento o rango inválido, o ID sin la vinculación requerida. */
    DSM_ERR_NOMEM = -2, /**< Memoria, capacidad o espacio de salida insuficientes. Incluye límites agotados. */
    DSM_ERR_NODO = -3, /**< Par sin enlace utilizable o página aislada por pérdida de coherencia. */
    DSM_ERR_TIEMPO = -4, /**< Venció el plazo de una operación que necesitaba respuesta. */
    DSM_ERR_ESTADO = -5, /**< Estado incompatible, aviso vigente no encontrado u otro fallo interno. */
    DSM_ERR_CONFIG = -6, /**< El INI no se pudo cargar o la configuración no admite el nodo. */
    DSM_ERR_PROTO = -7, /**< La trama o los metadatos recibidos incumplen el protocolo. */
    DSM_ERR_CERRADO = -8, /**< El contexto indicado no es el activo del proceso. */
    DSM_ERR_OCUPADA = -9, /**< Un reclamo, trabajador, cliente u operación impide completar el pedido ahora. */
    DSM_ERR_HILOS = -10, /**< Se agotaron las ranuras de trabajadores del paginador. */
    DSM_ERR_PARCIAL = -11 /**< Consulta global con datos de los hogares que respondieron y cobertura incompleta. */
};

/**
 * @brief Inicializa el único contexto DSM activo del proceso Linux.
 *
 * Copia la configuración y enlaza dsm_directorio_init(),
 * dsm_coherencia_iniciar() y dsm_pager_iniciar_vacio(). Coherencia inicia el
 * servicio TCP, los avisos y la atención de capacidad; el paginador crea su
 * hilo y semáforo e instala las acciones de SIGSEGV/SIGUSR1. Los enlaces con
 * otros nodos progresan en segundo plano. El proceso lógico se abre luego
 * con dsm_proc_abrir(); los trabajadores se registran desde sus propios hilos.
 *
 * @param[in] ruta_config Ruta legible de la configuración del clúster.
 * @param[in] nodo ID configurado cuyo endpoint local utilizará el servicio.
 * @param[out] salida Recibe el contexto solo en caso de éxito; en error se
 * conserva el valor anterior. El llamador conserva su propiedad hasta un
 * dsm_cerrar() exitoso y no debe liberarlo con free().
 * @return DSM_OK o un DSM_ERR_* de validación/inicialización.
 * @retval DSM_ERR_INVAL ruta_config o salida nulos, o parámetro rechazado
 * por una dependencia durante la inicialización.
 * @retval DSM_ERR_CONFIG Configuración inválida o nodo no configurado.
 * @retval DSM_ERR_ESTADO Ya existe un contexto activo, o falla un recurso
 * de inicialización cuya causa se traduce a este código.
 * @retval DSM_ERR_NOMEM No se pudo obtener memoria o capacidad auxiliar.
 * @retval DSM_ERR_OCUPADA Una dependencia, como las acciones de señal, está ocupada.
 * @note El mutex del API serializa el alta. Esta función no reclama el puesto
 * del pool: si se adquirió un reclamo nuevo para este alta y no se obtuvo
 * contexto, el llamador debe devolverlo mediante dsm_pool_liberar(). Un
 * rechazo por contexto ya activo conserva ese contexto y su reclamo previo.
 * @warning Llamar fuera de manejadores de señales. La instalación del
 * paginador afecta a las acciones de señal de todo el proceso y requiere
 * la plataforma Linux x86-64 admitida por el manejador.
 */
int dsm_iniciar(const char *ruta_config, dsm_node_id_t nodo, dsm_context_t **salida);

/**
 * @brief Retira las vinculaciones y destruye un contexto DSM propio.
 *
 * Desvincula primero las reservas remotas y libera después las propias,
 * incluidas preparaciones pendientes. dsm_directorio_preparar_cierre()
 * comprueba clientes y aportes; dsm_pager_cerrar() restaura las acciones de
 * señal, despierta su hilo con el semáforo y espera su terminación. Finalmente
 * se cierran coherencia/red/directorio y se devuelve el reclamo del pool.
 *
 * @param[in] ctx Contexto activo que pertenece al llamador.
 * @pre Todos los trabajadores deben haber dejado de usar Ar y haberse
 * desregistrado; el llamador debe coordinar también sus otras operaciones.
 * @return DSM_OK al completar el cierre, o un DSM_ERR_* del paso que falló.
 * @retval DSM_ERR_INVAL ctx es nulo, o una dependencia comunica un error de parámetros.
 * @retval DSM_ERR_CERRADO ctx no es el contexto activo.
 * @retval DSM_ERR_OCUPADA Hay trabajadores o consultas activos, clientes,
 * aportes o recursos que todavía impiden el cierre.
 * @post DSM_OK consume ctx: el llamador debe descartar el puntero y todas sus
 * vistas. En error de un contexto activo, ctx no se libera ni se devuelve su
 * reclamo; el cierre puede haber retirado ya algunas reservas o sus vistas.
 * @warning El cierre no es atómico. Tras un error, consultar las vinculaciones
 * con dsm_consultar_reserva() y reintentar la limpieza; no asumir que los
 * punteros anteriores o todo el estado del paginador sigan disponibles.
 * @note Conserva el mutex del API durante la retirada. Las RPC tienen sus
 * propios plazos; no existe un plazo único para todo el cierre. No llamar
 * desde un manejador de señales.
 */
int dsm_cerrar(dsm_context_t *ctx);

/**
 * @brief Abre la única identidad de proceso lógico del contexto.
 *
 * Asigna (nodo << 16) | 1 bajo el mutex del API. Esa identidad se utiliza
 * para reservar y registrar trabajadores; no crea un proceso Linux ni un
 * Pthread y no coincide necesariamente con el PID del sistema operativo.
 *
 * @param[in] ctx Contexto activo inicializado mediante dsm_iniciar().
 * @param[out] salida Recibe el ID solo en caso de éxito.
 * @return DSM_OK, DSM_ERR_INVAL por ctx/salida nulos, DSM_ERR_CERRADO por
 * contexto no activo, o DSM_ERR_ESTADO si la identidad ya estaba abierta.
 * @post La identidad permanece asociada al contexto hasta su cierre; las
 * tareas posteriores reutilizan el ID obtenido en la primera llamada.
 * @note Es una operación local, sin RPC. No llamar desde un manejador de señales.
 */
int dsm_proc_abrir(dsm_context_t *ctx, dsm_proc_id_t *salida);

/**
 * @brief Registra al hilo llamador para usar las vistas Ar del contexto.
 *
 * dsm_pager_registrar() asigna una ranura preinicializada y
 * dsm_senales_registrar() publica su solicitud en TLS. SIGUSR1 queda bloqueada
 * y reservada para el paginador; SIGSEGV debe estar desbloqueada. Un fallo Ar
 * válido publica la solicitud y hace sem_post(); el hilo paginador resuelve
 * mediante dsm_coherencia_resolver() y notifica el resultado con SIGUSR1.
 *
 * @param[in] ctx Contexto activo propietario de las vistas y del paginador.
 * @param[in] proceso ID no nulo obtenido de dsm_proc_abrir() en este contexto.
 * @pre Invocar desde el propio trabajador, una vez por período de uso de Ar.
 * El llamador debe conservar el contexto y las vistas hasta desregistrarse.
 * @return DSM_OK o un DSM_ERR_* de contexto/registro.
 * @retval DSM_ERR_INVAL ctx nulo o proceso distinto del ID del contexto.
 * @retval DSM_ERR_CERRADO ctx no está activo.
 * @retval DSM_ERR_OCUPADA Existe una vista en retirada que aún conserva su base.
 * @retval DSM_ERR_HILOS No queda una ranura de trabajador disponible.
 * @retval DSM_ERR_ESTADO Falla el registro del paginador o de las señales,
 * por ejemplo por registro duplicado o SIGSEGV bloqueada.
 * @post Solo el éxito aumenta el número de trabajadores que impide desmontar
 * vistas o cerrar el contexto. El hilo debe llamar dsm_hilo_desregistrar().
 * @note No crea hilos ni puede registrar a otro Pthread. No llamar desde un
 * manejador de señales; los mutex se usan antes de entrar en la ruta de fallo.
 */
int dsm_hilo_registrar(dsm_context_t *ctx, dsm_proc_id_t proceso);

/**
 * @brief Termina el registro DSM del hilo llamador.
 *
 * dsm_pager_desregistrar() protege su ranura frente al último despertar;
 * dsm_senales_desregistrar() drena SIGUSR1 pendiente y restaura el bit de su
 * máscara que existía antes del registro. Las acciones de señal del proceso
 * permanecen instaladas hasta cerrar el paginador.
 *
 * @param[in] ctx Contexto activo en el que este mismo hilo se registró.
 * @pre El hilo ya terminó sus accesos Ar y no está atendiendo un fallo.
 * Desregistrarse antes de salir del Pthread o cancelarlo.
 * @return DSM_OK, DSM_ERR_INVAL por ctx nulo, DSM_ERR_CERRADO por contexto no
 * activo, o DSM_ERR_ESTADO si falta el registro o falla su retirada.
 * @post Solo el éxito libera la ranura, limpia el indicador TLS del API y
 * decrementa el contador de trabajadores. En error el llamador conserva la
 * obligación de resolver su registro antes de desmontar o cerrar.
 * @note No hace pthread_join(). La aplicación espera a sus trabajadores antes
 * de retirar las vistas. No llamar desde un manejador de señales.
 */
int dsm_hilo_desregistrar(dsm_context_t *ctx);

/**
 * @brief Crea una reserva propia con tamaño lógico y una vista Ar contigua.
 *
 * Prepara capacidad local, prioriza hogares conectados y, si falta, sondea
 * otros con un presupuesto común. Confirma el mapa y lo asocia al paginador
 * antes de publicarlo. El respaldo se redondea a páginas; las remotas se
 * resuelven al acceder a ellas. Para usar directamente Ar se debe registrar
 * el hilo, coordinar su vigencia y respetar el tamaño lógico, no el relleno.
 *
 * @param[in] ctx Contexto activo que será el creador de la reserva.
 * @param[in] proceso ID no nulo del proceso lógico ya abierto en ctx.
 * @param[in] bytes Tamaño lógico solicitado, positivo y expresado en bytes.
 * @param[out] id Recibe el ID global solo al completar la creación.
 * @param[out] base Recibe la dirección prestada de Ar solo al éxito.
 * @return DSM_OK o un DSM_ERR_* del gestor, transporte o validación.
 * @retval DSM_ERR_INVAL proceso no coincide con ctx, bytes es cero o una salida es nula.
 * @retval DSM_ERR_NOMEM Capacidad, tramos, IDs o recursos locales insuficientes.
 * @retval DSM_ERR_NODO No se cubrió la reserva y quedaron hogares ausentes.
 * @retval DSM_ERR_TIEMPO Venció una operación necesaria para crear la reserva.
 * @retval DSM_ERR_PROTO Los metadatos recibidos no forman un manifiesto válido.
 * @post En error no escribe las salidas e intenta cancelar lo preparado,
 * conservando el error original. Pueden quedar cancelaciones internas pendientes
 * que se reintentan al reservar otra vez o cerrar ctx.
 * @note Conserva el mutex del API; el presupuesto de sondeo no es un plazo
 * global para toda la creación y sus RPC.
 */
int dsm_reservar(dsm_context_t *ctx, dsm_proc_id_t proceso, size_t bytes,
                 dsm_reserva_id_t *id, void **base);
/**
 * @brief Vincula una reserva existente o devuelve su vista ya abierta en ctx.
 *
 * Una apertura nueva obtiene el manifiesto del creador por TCP, monta las
 * vistas y registra el mapa en el paginador. Mantiene un pin por nodo en el
 * creador. Reabrir un ID vinculado devuelve la misma Ar y tamaño sin sumar
 * aperturas ni consultar de nuevo al creador; no recupera páginas aisladas.
 *
 * @param[in] ctx Contexto activo del cliente o creador que ya tiene el ID.
 * @param[in] id ID global de reserva, con creador configurado y parte local no nula.
 * @param[out] base Recibe la dirección prestada de Ar solo al éxito.
 * @param[out] bytes Recibe el tamaño lógico original solo al éxito.
 * @return DSM_OK o un DSM_ERR_* de apertura, asociación o transporte.
 * @retval DSM_ERR_INVAL ID inválido/inexistente o salidas nulas.
 * @retval DSM_ERR_NODO No se logró el alta del creador antes de enviar LOOKUP.
 * @retval DSM_ERR_OCUPADA El ID conserva una retirada pendiente.
 * @retval DSM_ERR_NOMEM No se pudo obtener memoria para el mapa o la vista.
 * @retval DSM_ERR_TIEMPO Venció una RPC de apertura.
 * @retval DSM_ERR_PROTO El manifiesto o la respuesta incumplen el protocolo.
 * @post Si falla después de solicitar la apertura, intenta desvincularla y
 * conserva el error inicial. Una limpieza inconclusa conserva el ID retirándose;
 * consultar su estado y reintentar dsm_desvincular_reserva permite concluirla.
 * @warning Ar es prestada; no se debe utilizar mientras su retirada esté pendiente.
 */
int dsm_abrir_reserva(dsm_context_t *ctx, dsm_reserva_id_t id, void **base, size_t *bytes);
/** Estado local observado por dsm_consultar_reserva(), sin consultar otros nodos.
 * Una retirada o preparación pendiente conserva el ID para la limpieza. */
typedef struct dsm_reserva_estado {
    dsm_reserva_id_t id; /**< Identidad de la reserva dentro de este registro. */
    size_t bytes;        /**< Longitud lógica. Puede valer cero si aún no se asoció la vista. */
    void *base;          /**< Inicio prestado de Ar. NULL durante preparación o retirada. */
    /** retirando impide nuevos accesos. provisional identifica una creación pendiente
     * de completar o cancelar. Ambos indicadores pueden estar activos a la vez. */
    int retirando, provisional;
} dsm_reserva_estado_t;
/**
 * @brief Copia el estado local de una vinculación, incluso si su retiro está pendiente.
 * @param[in] ctx Contexto activo cuya lista de reservas se observa.
 * @param[in] id ID que debe estar presente en esa lista local.
 * @param[out] salida Recibe el registro de estado; debe ser no nula.
 * @return DSM_OK al copiar el registro o un DSM_ERR_* de contexto/validación.
 * @retval DSM_ERR_INVAL salida es nula o el ID no está vinculado en ctx.
 * @note Se ejecuta bajo el mutex del API, sin RPC ni concesión MSI. Una
 * preparación o retirada conserva su ID y devuelve base NULL; bytes puede
 * ser cero si aún no se asoció una vista. En error no escribe salida.
 * @warning La observación no retiene la vista después de retornar. base es
 * prestada: su uso requiere coordinar el ciclo de vida y registrar el hilo.
 */
int dsm_consultar_reserva(dsm_context_t *ctx, dsm_reserva_id_t id, dsm_reserva_estado_t *salida);
/**
 * @brief Obtiene hogar y GPN del ordinal lógico de una reserva vinculada.
 * @param[in] ctx Contexto activo que conoce el mapa de la reserva.
 * @param[in] id ID de una reserva vinculada en ctx.
 * @param[in] orden Ordinal desde cero hasta la última página con bytes lógicos.
 * @param[out] hogar Recibe el ID del hogar solo al éxito; debe ser no nula.
 * @param[out] gpn Recibe el número global de página solo al éxito; debe ser no nula.
 * @return DSM_OK o un DSM_ERR_* de validación/consulta local.
 * @retval DSM_ERR_INVAL ID, ordinal o salidas inválidos.
 * @retval DSM_ERR_OCUPADA La vinculación está retirándose.
 * @note No exige registrar el hilo, traer datos ni adquirir permisos MSI.
 * El resultado describe el mapa, sin garantizar disponibilidad actual del
 * hogar o contenido. orden no es un GPN.
 */
int dsm_ubicacion_pagina(dsm_context_t *ctx, dsm_reserva_id_t id, size_t orden,
                         dsm_node_id_t *hogar, uint64_t *gpn);
/**
 * @brief Retira la vista de cliente y cierra su apertura con el creador.
 *
 * Revoca las copias por hogar, devuelve los M recuperables y confirma el cierre
 * del pin en el creador antes de desmontar Ar. Una copia perdida se puede retirar
 * con revocación confirmada sin recuperar su imagen: esa página sigue aislada.
 * Si ctx también aporta páginas, conserva el respaldo y el servicio del aporte.
 *
 * @param[in] ctx Contexto activo del cliente.
 * @param[in] id ID vinculado cuyo creador debe ser otro nodo.
 * @pre Cero trabajadores registrados en todo ctx, incluso sobre otras reservas.
 * @return DSM_OK o un DSM_ERR_* del cierre por hogares y del desmontaje local.
 * @retval DSM_ERR_INVAL ID no vinculado; también al repetir después de un cierre exitoso.
 * @retval DSM_ERR_ESTADO El ID corresponde a una reserva propia.
 * @retval DSM_ERR_OCUPADA Aún hay trabajadores registrados en ctx.
 * @retval DSM_ERR_TIEMPO Venció una confirmación necesaria para retirar la vista.
 * @retval DSM_ERR_NODO No hay un enlace utilizable para completar el cierre.
 * @post DSM_OK elimina la vinculación e invalida los punteros prestados.
 * @warning Ante error conservar el ID, consultar el estado y reintentar esta
 * función. Puede haber una retirada parcial; Ar no se garantiza utilizable.
 */
int dsm_desvincular_reserva(dsm_context_t *ctx, dsm_reserva_id_t id);
/**
 * @brief Devuelve los aportes de una reserva propia y elimina su vinculación.
 *
 * Cierra la admisión de nuevas aperturas, retira las copias del creador y
 * devuelve los aportes remotos antes del local. Guarda las confirmaciones por
 * hogar para continuar tras un error. Una preparación provisional usa CANCEL.
 *
 * @param[in] ctx Contexto activo del nodo creador.
 * @param[in] id ID propio vinculado que se desea liberar o cuya limpieza se reintenta.
 * @pre Cero trabajadores registrados en todo ctx.
 * @return DSM_OK o un DSM_ERR_* del retiro, devolución de aportes o validación.
 * @retval DSM_ERR_INVAL ID no vinculado; también al repetir después del éxito.
 * @retval DSM_ERR_ESTADO El ID es una vinculación cuyo creador es otro nodo.
 * @retval DSM_ERR_OCUPADA Hay trabajadores registrados o clientes con aperturas.
 * @retval DSM_ERR_TIEMPO Venció una operación necesaria para devolver capacidad.
 * @retval DSM_ERR_NODO Un hogar no tiene enlace utilizable para concluir.
 * @post DSM_OK elimina el ID de ctx e invalida todos sus punteros prestados.
 * @warning La liberación no es atómica entre hogares. En error puede haberse
 * desmontado Ar o devuelto parte de la capacidad: conservar el mismo ID,
 * consultar su estado y reintentar sin asumir que el puntero siga disponible.
 */
int dsm_liberar(dsm_context_t *ctx, dsm_reserva_id_t id);

/**
 * @brief Lee un rango lógico mediante coherencia por página y copia controlada.
 *
 * Admite cruces de páginas/hogares y solapamiento como memmove. Reconoce una
 * Ar de ctx como destino y valida también su rango. Registra temporalmente el
 * hilo si hace falta, conservando un registro previo del llamador. Las copias
 * controladas solapadas con escritura se serializan dentro de ctx.
 *
 * @param[in] ctx Contexto activo de las vistas que intervienen.
 * @param[in] id ID de una reserva vinculada que será la fuente.
 * @param[in] offset Desplazamiento en bytes dentro del tamaño lógico.
 * @param[out] destino Buffer ordinario o vista Ar de ctx con espacio para la longitud solicitada.
 * Puede ser NULL para bytes cero.
 * @param[in] bytes Longitud en bytes; cero sigue validando ID/estado y admite
 * offset al final de la reserva.
 * @return DSM_OK o un DSM_ERR_* de validación, registro, copia o limpieza.
 * @retval DSM_ERR_INVAL Rango incorrecto o destino nulo con bytes positivos.
 * @retval DSM_ERR_OCUPADA Una retirada impide usar la vista o registrar el hilo.
 * @retval DSM_ERR_HILOS No hay ranura para el registro temporal del trabajador.
 * @retval DSM_ERR_NOMEM No se pudieron obtener los temporales de la copia.
 * @retval DSM_ERR_NODO Página aislada o par sin enlace utilizable.
 * @retval DSM_ERR_TIEMPO Venció una resolución que necesitaba respuesta.
 * @retval DSM_ERR_PROTO Los datos o permisos recibidos incumplen el protocolo.
 * @post Publica un destino ordinario solo cuando termina la lectura DSM:
 * un error de copia/resolución lo conserva. Hacia Ar escribe por fragmentos y
 * puede dejar cambios parciales. Si solo falla el desregistro temporal, puede
 * devolver ESTADO después de publicar los bytes; esa publicación no se revierte.
 * @warning El llamador mantiene válidos y sincronizados los buffers ordinarios
 * y accesos Ar directos solapados. Varias páginas no forman una instantánea global.
 */
int dsm_leer(dsm_context_t *ctx, dsm_reserva_id_t id, size_t offset,
              void *destino, size_t bytes);
/**
 * @brief Escribe un rango lógico con exclusividad MSI y respaldo local por trozo.
 *
 * Reconoce una Ar de ctx como fuente y valida ambos rangos. Admite páginas y
 * hogares distintos, solapamiento como memmove y registro temporal del hilo.
 * Aplica cada fragmento sobre Cr bajo M, guardando bytes previos, versión y
 * contenido escrito. Ante error de copia intenta restaurar en orden inverso
 * solo si siguen vigentes identidad, M, versión y bytes escritos, sin pedir
 * permisos nuevos. Conserva el error original aunque la restauración falle.
 *
 * @param[in] ctx Contexto activo de las vistas que intervienen.
 * @param[in] id ID de una reserva vinculada que será el destino.
 * @param[in] offset Desplazamiento de destino dentro del tamaño lógico, en bytes.
 * @param[in] origen Buffer ordinario o vista Ar de ctx con la longitud legible solicitada. Puede
 * ser NULL para bytes cero.
 * @param[in] bytes Longitud en bytes; cero sigue validando ID/estado y admite
 * offset al final de la reserva.
 * @return DSM_OK o un DSM_ERR_* de validación, registro, copia o limpieza.
 * @retval DSM_ERR_INVAL Rango incorrecto u origen nulo con bytes positivos.
 * @retval DSM_ERR_OCUPADA Una retirada impide usar la vista o registrar el hilo.
 * @retval DSM_ERR_HILOS No hay ranura para el registro temporal del trabajador.
 * @retval DSM_ERR_NOMEM No se pudieron reservar los temporales o el diario.
 * @retval DSM_ERR_NODO Página aislada o par sin enlace utilizable.
 * @retval DSM_ERR_TIEMPO Venció una resolución que necesitaba respuesta.
 * @retval DSM_ERR_PROTO Los datos o permisos recibidos incumplen el protocolo.
 * @post Puede quedar una escritura parcial que otros nodos ya observaron.
 * Si solo falla el desregistro temporal después de copiar, devuelve ESTADO
 * sin intentar restaurar los bytes ya publicados.
 * @warning El llamador sincroniza buffers ordinarios y accesos Ar directos
 * solapados durante copia y restauración. Los rangos conflictivos de estas
 * funciones se serializan en ctx; el conjunto no es una transacción distribuida.
 */
int dsm_escribir(dsm_context_t *ctx, dsm_reserva_id_t id, size_t offset,
                  const void *origen, size_t bytes);

enum {
    DSM_CONSULTA_CONTADORES = 8, /**< Número de contadores por nodo, acumulados durante la sesión. */
    DSM_CONSULTA_NOMBRE = 64, /**< Capacidad del nombre en bytes, incluido el terminador NUL. */
    DSM_HOGAR_TODOS = 0xFFFF, /**< Selector global para las consultas de páginas autoritativas. */
    DSM_CONSULTA_RANGO_MAX = 1024, /**< Máximo de filas que se pueden pedir en una consulta por rango. */
    DSM_ASIGNACION_ASIGNADA = 0, /**< Página con descriptor de reserva confirmado. */
    DSM_ASIGNACION_LIBRE = 1, /**< El mapa de ocupación del hogar marca la página como disponible. */
    DSM_ASIGNACION_PREPARADA = 2, /**< Capacidad apartada por PREPARE y pendiente de confirmación. */
    DSM_DISP_VACIO = 0, /**< Puesto aún sin estado de enlace registrado. */
    DSM_DISP_UNION = 1, /**< Enlace en fase de incorporación. */
    DSM_DISP_EN_LINEA = 2, /**< Puesto en línea según la observación. */
    DSM_DISP_SOSPECHA = 3, /**< Puesto sospechoso, por ejemplo si vence una consulta. */
    DSM_DISP_FUERA = 4 /**< Puesto sin conexión utilizable en la observación. */
};

/** Ficha de un puesto configurado. Los datos de ocupación solo son medidos
 * cuando respondio vale uno. Las consultas sucesivas pueden observar cambios. */
typedef struct dsm_nodo_info {
    uint16_t id;         /**< ID del puesto en el INI. */
    uint8_t estado;      /**< Estado conocido de disponibilidad, uno de los valores DSM_DISP_*. */
    uint32_t ipv4_host;  /**< IPv4 en orden de bytes del host. */
    uint16_t puerto;     /**< Puerto TCP del puesto. */
    uint64_t memoria;    /**< Capacidad nominal del hogar en bytes, incluso si está ausente. */
    uint64_t base;       /**< Desplazamiento global en bytes del primer GPN de este hogar. */
    uint32_t libres;     /**< Páginas disponibles en el hogar que respondió. */
    uint32_t totales;    /**< Páginas configuradas para este hogar, incluidas las libres. */
    uint32_t aportados;  /**< Páginas ocupadas en el hogar, incluidas las preparadas. */
    char nombre[DSM_CONSULTA_NOMBRE]; /**< Nombre del puesto terminado en NUL. */
    /** En orden se cuentan faltas de lectura y escritura, invalidaciones enviadas y recibidas,
     * páginas de datos enviadas y recibidas, y sus bytes de carga enviados y recibidos.
     * Las faltas cuentan intentos de resolución. Sumar envíos y recepciones de toda la
     * malla puede contar ambos extremos de una misma transferencia. */
    uint64_t contadores[DSM_CONSULTA_CONTADORES];
    uint8_t respondio;   /**< Cero indica ocupación y contadores desconocidos, aun si los campos valen cero. */
} dsm_nodo_info_t;

/** Fila de un GPN. El hogar administra la página, el creador identifica la reserva
 * y el poseedor MSI puede cambiar con los permisos. Consultar asignacion antes de
 * interpretar MSI. Las filas libres y preparadas carecen de concesión MSI. */
typedef struct dsm_pagina_info {
    uint64_t gpn;        /**< Número global de página según las bases de los hogares en el INI. */
    uint64_t mascara;    /**< El bit i identifica una copia registrada en el nodo i según el descriptor. */
    uint32_t proc;       /**< Último solicitante anotado, incluso si se rechaza su petición. Puede valer cero. */
    uint32_t reserva;    /**< ID de la reserva que ocupa el GPN. Cero si está libre. */
    uint32_t orden;      /**< Ordinal desde cero dentro de la reserva. UINT32_MAX si está libre. */
    uint16_t hogar;      /**< Puesto responsable del GPN y de contabilizarlo en su capacidad. */
    uint16_t poseedor;   /**< Puesto con exclusividad M. UINT16_MAX cuando no hay poseedor. */
    uint8_t msi;         /**< Estado del descriptor consultado en una página asignada (0 I, 1 S, 2 M). */
    uint8_t permiso;     /**< Permiso informado para la vista del emisor (0 ninguno, 1 lectura, 3 lectura/escritura). */
    uint8_t copias;      /**< Cantidad de bits activos en mascara. */
    uint32_t memoria_libre; /**< Páginas libres del hogar en consultas globales. En el rango de copias locales,
                             * UINT32_MAX indica hogar remoto. La tabla local de dsm_imprimir() usa el dato del emisor. */
    uint32_t version;    /**< Versión de coherencia del GPN, usada para distinguir concesiones sucesivas. */
    uint16_t creador;    /**< Nodo que originó el ID de reserva. UINT16_MAX si está libre. */
    uint32_t propietario; /**< Proceso lógico estable de la reserva pública. Cero en el servicio histórico o página libre. */
    uint8_t publica;     /**< Uno si la procedencia pública está marcada en el manifiesto. */
    uint8_t asignacion;  /**< Estado DSM_ASIGNACION_* independiente de los permisos de acceso. */
} dsm_pagina_info_t;

/**
 * @brief Consulta una ficha por cada puesto configurado, en orden de ID.
 * @param ctx Contexto activo; no exige abrir proceso ni registrar este hilo.
 * @param[out] salida Almacenamiento del llamador para cap fichas; puede ser NULL
 * solo con cap == 0. No se devuelve memoria interna ni hay que liberarla.
 * @param cap Capacidad de salida en fichas, no en bytes.
 * @param[out] cantidad Total de puestos configurados, aunque algunos no respondan.
 *
 * Se consulta cada puesto secuencialmente, también al contar con NULL/0. El
 * puesto local usa el servicio interno; los remotos, STATS por TCP con plazo
 * por RPC. No hay un plazo único para el recorrido ni una instantánea global.
 * NODO y TIEMPO se representan con respondio == 0 y estado FUERA o SOSPECHA:
 * capacidad y endpoint siguen siendo nominales; los ceros de ocupación y
 * contadores no son mediciones de esos hogares.
 *
 * @retval DSM_OK Se publicó toda la tabla, o solo su tamaño con NULL/0. Los
 * ausentes no producen DSM_ERR_PARCIAL en esta función.
 * @retval DSM_ERR_NOMEM Con salida no NULL y cap insuficiente se copian las
 * primeras cap fichas y cantidad conserva el total. También puede indicar
 * falta de memoria durante una RPC; en ese caso no se publica la tabla.
 * @retval DSM_ERR_INVAL ctx/cantidad nulos o salida NULL con cap distinto de cero.
 * @retval DSM_ERR_CERRADO El contexto no está activo.
 * @return Otro error de RPC (OCUPADA, PROTO, ESTADO, entre otros) interrumpe el
 * recorrido, deja cantidad == 0 y no modifica salida. Los argumentos inválidos
 * o un contexto rechazado no inicializan las salidas.
 * @note La consulta mantiene vivo ctx y hace que dsm_cerrar devuelva OCUPADA
 * mientras termina; no retiene el mutex del API durante RPC ni copia de salida.
 * El llamador sincroniza sus buffers y evita usar ctx tras un cierre exitoso.
 */
int dsm_consultar_nodos(dsm_context_t *ctx, dsm_nodo_info_t *salida, size_t cap,
                        size_t *cantidad);
/**
 * @brief Consulta la ficha y los contadores de un solo puesto configurado.
 * @param ctx Contexto activo; no exige proceso lógico ni registro del hilo.
 * @param nodo ID concreto, menor que la cantidad configurada; no admite TODOS.
 * @param[out] salida Una ficha en almacenamiento escribible del llamador.
 *
 * El nodo local se atiende por despacho interno; otro nodo requiere una RPC
 * STATS por TCP, con el plazo configurado. memoria/base están en bytes y las
 * cantidades de capacidad en páginas. Los contadores se leen individualmente:
 * ni sus campos ni consultas separadas forman una instantánea transaccional.
 * Solo se escribe salida al recibir una respuesta válida (respondio == 1).
 *
 * @retval DSM_OK Ficha recibida y copiada; el llamador conserva su almacenamiento.
 * @retval DSM_ERR_INVAL ctx/salida nulos o nodo no configurado.
 * @retval DSM_ERR_CERRADO El contexto no está activo.
 * @retval DSM_ERR_NODO No hay enlace o se pierde, o el servicio remoto está fuera.
 * @retval DSM_ERR_TIEMPO Venció el plazo de la RPC.
 * @retval DSM_ERR_NOMEM No hay memoria para la consulta/respuesta local o remota.
 * @retval DSM_ERR_OCUPADA El servicio rechaza la solicitud por cola/conflicto.
 * @retval DSM_ERR_PROTO La respuesta no respeta el formato o la correlación.
 * @retval DSM_ERR_ESTADO Otro fallo interno o de sistema.
 * @note No sintetiza una ficha de ausente ni devuelve PARCIAL. Protege la vida
 * de ctx frente al cierre, pero libera el mutex del API durante la RPC.
 */
int dsm_consultar_nodo(dsm_context_t *ctx, uint16_t nodo, dsm_nodo_info_t *salida);
/**
 * @brief Obtiene la tabla histórica de páginas asignadas de sus hogares.
 * @param ctx Contexto activo; no exige registro del hilo.
 * @param hogar ID configurado o DSM_HOGAR_TODOS para recorrerlos en orden de ID.
 * @param[out] salida Almacenamiento para cap filas; NULL solo con cap == 0.
 * @param cap Número de filas que caben en salida.
 * @param[out] cantidad Total de filas reunidas de los hogares que respondieron,
 * aunque cap sea menor. No cuenta páginas libres ni preparadas sin descriptor.
 *
 * Usa STATS_HOGAR histórico: solo descriptores asignados cuyo hogar es el
 * emisor, sin garantizar orden por GPN dentro de cada hogar. No es el inventario
 * libre/preparado/asignado de dsm_consultar_rango. Cada hogar aporta su cifra de
 * páginas libres; las observaciones y los contadores no son simultáneos.
 * Reúne en memoria toda la tabla antes de publicar, incluso con NULL/0 o cap
 * pequeño. Las respuestas históricas no tienen cursor y su tamaño está limitado
 * por la trama; para recorridos acotados se debe usar dsm_consultar_rango.
 *
 * @retval DSM_OK Tabla completa para los hogares seleccionados; NULL/0 solo
 * comunica cantidad, pero realiza igualmente las RPC y asignaciones internas.
 * @retval DSM_ERR_PARCIAL Solo con TODOS: se entregan las filas disponibles y
 * faltó al menos un hogar por NODO/TIEMPO; cantidad no incluye sus páginas.
 * @retval DSM_ERR_NOMEM Si cap no alcanza, copia el prefijo que cabe e informa
 * el total disponible. Este error prevalece sobre PARCIAL. También puede fallar
 * la memoria interna o el tamaño de una respuesta antes de publicar.
 * @retval DSM_ERR_INVAL Argumentos nulos/incompatibles o hogar no configurado.
 * @retval DSM_ERR_CERRADO El contexto no está activo.
 * @return Un hogar explícito propaga NODO/TIEMPO; cualquier hogar puede devolver
 * OCUPADA, PROTO, ESTADO u otro error de RPC. Un fallo anterior a la publicación
 * deja cantidad == 0 y salida intacta tras validar argumentos.
 * @note Mantiene vivo ctx sin retener el mutex del API durante las RPC ni la
 * publicación. No hay plazo global, reserva de páginas ni memoria interna
 * prestada al llamador; las filas siguen siendo observaciones que pueden caducar.
 */
int dsm_consultar_paginas(dsm_context_t *ctx, uint16_t hogar, dsm_pagina_info_t *salida,
                          size_t cap, size_t *cantidad);
/**
 * @brief Recorre por GPN el inventario de hogares o las ventanas locales.
 * @param ctx Contexto activo; no exige registro del hilo.
 * @param hogar ID configurado o DSM_HOGAR_TODOS; con copias == 1 debe ser
 * exactamente el nodo de ctx, incluso si las ventanas tienen otros hogares.
 * @param copias Solo 0 o 1. Cero incluye LIBRE/PREPARADA/ASIGNADA en el hogar;
 * uno enumera páginas de ventanas vinculadas localmente, incluidas las I,
 * excluyendo el backing de donantes sin vinculación.
 * @param desde Primer GPN inclusivo; no es un offset en bytes ni ordinal de reserva.
 * @param limite Capacidad de salida, de 1 a DSM_CONSULTA_RANGO_MAX filas.
 * @param[out] salida Buffer no NULL para al menos limite filas, del llamador.
 * @param[out] cantidad Filas publicadas en esta llamada, como máximo limite;
 * no es un recuento de toda la tabla.
 * @param[out] siguiente GPN desde el que continuar; comienza en desde.
 * @param[out] mas Continuación indicada por el recorrido, no garantía de filas
 * disponibles ni de cobertura completa de hogares ausentes.
 *
 * Inicializa cantidad/mas a cero y siguiente a desde después de validar los
 * argumentos. Publica filas crecientes directamente, sin reunir el inventario
 * completo. Un hogar sin GPN elegibles puede omitirse sin RPC. En TODOS recorre
 * los hogares a partir del GPN indicado por @p desde. Tras llenar el búfer aún consulta
 * fichas de los restantes para observar su disponibilidad, sin descargar filas.
 * Cada RPC tiene su plazo propio y no hay una instantánea entre llamadas.
 *
 * @retval DSM_OK Recorrido atendido, incluso si devuelve cero filas. Si mas
 * vale uno, continuar con siguiente para evitar un truncado silencioso.
 * @retval DSM_ERR_PARCIAL Solo con TODOS: faltó algún hogar visitado por
 * NODO/TIEMPO. Puede haber filas útiles; el cursor omite el intervalo ausente
 * mientras queda espacio. Continuar no vuelve a consultar ese intervalo:
 * debe reintentarse aparte si se necesita cubrirlo.
 * @retval DSM_ERR_INVAL Punteros obligatorios nulos, límite/copias/hogar inválidos
 * o intento de pedir copias de otro nodo o de TODOS.
 * @retval DSM_ERR_CERRADO El contexto no está activo.
 * @return NODO/TIEMPO de un hogar explícito y otros fallos de RPC (NOMEM,
 * OCUPADA, PROTO, ESTADO) se propagan. Tras comenzar puede quedar un prefijo
 * publicado junto con cantidad/siguiente/mas: no se revierte ante esos errores.
 * No hay modo NULL/0 para contar ni NOMEM por un buffer de limite filas lleno.
 * @note Cada respuesta protege su estado con el mutex del directorio; el API
 * solo mantiene una referencia contra el cierre y no retiene su mutex en red.
 * memoria_libre se expresa en páginas; en copias de hogar remoto es UINT32_MAX.
 * El llamador conserva los buffers, los sincroniza y no libera memoria interna.
 */
int dsm_consultar_rango(dsm_context_t *ctx, uint16_t hogar, int copias,
                        uint64_t desde, size_t limite, dsm_pagina_info_t *salida,
                        size_t *cantidad, uint64_t *siguiente, int *mas);
/**
 * @brief Imprime fichas, contadores y descriptores mediante STATS histórico.
 * @param ctx Contexto activo; no exige registro del hilo.
 * @param[in,out] salida FILE abierto por el llamador para escritura; no se
 * cierra ni se hace fflush, y el error persistente del flujo no se limpia.
 * @param global Solo 0 o 1: cero consulta el servicio local (STATS_AMBOS);
 * uno recorre todos los hogares (STATS_HOGAR) en orden de ID.
 *
 * El modo local imprime todos los descriptores conocidos, incluso de hogar
 * remoto o de un donante sin ventana vinculada. No equivale a copias local.
 * El global imprime solo asignadas en cada hogar; ninguno enumera páginas
 * libres/preparadas ni divide sus tablas por cursor. La columna histórica
 * memoria_libre usa las páginas libres del emisor, incluso en filas locales
 * de otro hogar.
 * nominal_bytes está en bytes, aportados/libres en páginas y los GPN/orden
 * identifican páginas. No hay orden por GPN garantizado ni instantánea global.
 *
 * @retval DSM_OK Se atendieron las consultas y no hay ferror en salida.
 * @retval DSM_ERR_PARCIAL En global == 1 faltó algún hogar por NODO/TIEMPO;
 * imprime su estado estimado y capacidad nominal y continúa. Los ceros de
 * aportados/libres de esa línea no acreditan que esté vacío.
 * @retval DSM_ERR_INVAL ctx/salida nulos o global fuera de 0/1.
 * @retval DSM_ERR_CERRADO El contexto no está activo.
 * @retval DSM_ERR_ESTADO Al completar el recorrido se detectó ferror, que
 * prevalece sobre PARCIAL, u ocurrió otro fallo interno.
 * @return En local, NODO/TIEMPO se propagan. Los errores restantes de RPC
 * (NOMEM, OCUPADA, PROTO, entre otros) interrumpen cualquier modo y prevalecen
 * sobre la comprobación final de ferror. El texto ya escrito no se revierte.
 * @note Puede asignar una respuesta histórica completa por hogar y bloquear
 * en RPC o E/S del flujo. Mantiene vivo ctx sin retener el mutex del API;
 * dsm_cerrar devuelve OCUPADA mientras imprime. El llamador coordina otros
 * escritores del flujo si necesita que el informe completo no se intercale.
 */
int dsm_imprimir(dsm_context_t *ctx, FILE *salida, int global);

/**
 * @brief Reclama un endpoint local configurado mediante un guardia del pool.
 *
 * Recorre los puestos en orden de configuración, comprueba que la IPv4 sea
 * local y mantiene un socket UNIX abstracto identificado por IPv4/puerto.
 * Un bind TCP temporal descarta puertos ocupados; el guardia excluye otros
 * usuarios del pool en el mismo namespace de red. Se mantiene un reclamo por
 * proceso. Repetir con una configuración que contiene ese endpoint devuelve
 * su ID sin adquirir otro guardia.
 *
 * @param[in] ruta Configuración que se mantendrá consistente durante la sesión.
 * @param[out] id Recibe el puesto solo al devolver DSM_OK.
 * @return DSM_OK; DSM_ERR_INVAL por argumentos nulos; DSM_ERR_CONFIG por
 * configuración inválida; DSM_ERR_OCUPADA si no hay puesto local elegible o
 * ya se retiene un endpoint ajeno a esa configuración; DSM_ERR_ESTADO ante
 * un fallo de instalación de atfork o de creación del guardia.
 * @post El reclamo dura hasta dsm_pool_liberar(), el cierre DSM exitoso o la
 * salida del proceso. No se ha iniciado todavía el servicio TCP ni el contexto.
 * @note Su mutex es propio del pool. pthread_atfork() hace que el hijo descarte
 * su copia del guardia sin devolver la del padre; no habilita el uso de un
 * contexto DSM heredado. Un programa ajeno al pool aún puede competir por el
 * puerto entre la comprobación y dsm_iniciar(). No llamar desde señales.
 */
int dsm_pool_reclamar(const char *ruta, uint16_t *id);

/**
 * @brief Devuelve el reclamo conocido por el pool en este proceso.
 *
 * Cierra el descriptor guardia bajo el mutex del pool y limpia su referencia.
 * Es idempotente cuando no se conserva ningún reclamo. No cierra sockets TCP,
 * contextos ni vistas; dsm_cerrar() lo invoca al completar su propio cierre.
 *
 * @pre Usar para deshacer un reclamo sin contexto, o después de completar la
 * retirada del servicio; no devolver manualmente un puesto DSM aún activo.
 * @note No devuelve estado de close(). Un exec conserva el descriptor sin
 * CLOEXEC, pero reinicializa el estado C del pool: esta función solo gestiona
 * el guardia que conoce la imagen actual. No llamar desde manejadores de señales.
 */
void dsm_pool_liberar(void);

/**
 * @brief Adelanta descubrimiento TCP con un presupuesto monótono común.
 *
 * dsm_coherencia_sondear() delega en dsm_red_sondear(): despierta los reintentos
 * salientes a IDs mayores y observa también los enlaces entrantes desde IDs
 * menores. Termina antes si todos los pares configurados están conectados.
 * La espera usa el mutex/condición de red; no utiliza el semáforo del paginador.
 *
 * @param[in] ctx Contexto DSM activo.
 * @param[in] plazo_ms Presupuesto positivo en milisegundos CLOCK_MONOTONIC,
 * compartido por la ronda, no por cada puesto.
 * @return DSM_OK al observar todos los enlaces o al agotar el presupuesto
 * con puestos ausentes; DSM_ERR_INVAL por ctx nulo/plazo cero;
 * DSM_ERR_CERRADO por contexto no activo, u otro DSM_ERR_* al fallar la red,
 * el reloj o su espera.
 * @note Mantiene una referencia de consulta durante la espera, suelta el mutex
 * del API y hace que dsm_cerrar() devuelva OCUPADA mientras la ronda sigue
 * activa. No garantiza disponibilidad futura, un número mínimo de nodos ni
 * acuerdo de membresía entre particiones. Los intentos TCP ya iniciados
 * conservan sus propios plazos. No llamar desde manejadores de señales.
 */
int dsm_sondear(dsm_context_t *ctx, uint32_t plazo_ms);
/**
 * @brief Publica o actualiza el valor que este nodo asocia con una clave.
 * @param[in] ctx Contexto activo del nodo autor.
 * @param[in] clave Cadena terminada en NUL, de 1 a 31 bytes antes del terminador.
 * @param[in] datos Carga que el servicio copia a su tabla. Admite NULL si n es cero.
 * @param[in] n Tamaño de la carga en bytes, de 0 a 128.
 * @return DSM_OK si el valor queda registrado localmente, incluso si ya era idéntico.
 * DSM_ERR_INVAL por clave, puntero o longitud inválidos. DSM_ERR_NOMEM si las 128
 * entradas están ocupadas y hace falta otra. DSM_ERR_ESTADO si se necesita una
 * generación nueva y el servicio está cerrando o agotó su secuencia.
 * @note El trabajador de avisos difunde ANUNCIO por TCP. El autor vuelve a
 * anunciar su estado vigente al establecer un enlace. El éxito local no acredita
 * recepción en cada par. Las aplicaciones de matrices y Merge Sort reconocen a los
 * participantes y confirman la propuesta de grupo en apps/demanda_grupo.c. Los puestos
 * que intercambian esos mensajes de aplicación usan la misma revisión del formato.
 */
int dsm_aviso_publicar(dsm_context_t *ctx, const char *clave, const void *datos, size_t n);
/**
 * @brief Copia el aviso vigente del autor de menor ID que coincida en clave y tamaño.
 * @param[in] ctx Contexto activo que consulta su tabla local de avisos.
 * @param[in] clave Clave terminada en NUL de 1 a 31 bytes.
 * @param[out] autor ID del puesto elegido. Es obligatorio aunque n valga cero.
 * @param[out] datos Destino para n bytes. Puede ser NULL con n igual a cero.
 * @param[in] n Longitud exacta del valor buscado, entre 0 y 128 bytes.
 * @return DSM_OK si encontró el valor. DSM_ERR_ESTADO si falta una coincidencia
 * de la sesión vigente. DSM_ERR_INVAL por argumentos inválidos.
 * @note Comprueba la sesión conocida y copia el resultado bajo el mutex de avisos.
 * No espera la llegada de publicaciones ni consume el aviso al leerlo.
 */
int dsm_aviso_consultar(dsm_context_t *ctx, const char *clave, uint16_t *autor,
                        void *datos, size_t n);
/**
 * @brief Enumera los nodos con un aviso vigente para la clave, de cualquier longitud.
 * @param[in] ctx Contexto activo que observa el grupo.
 * @param[in] clave Clave terminada en NUL de 1 a 31 bytes.
 * @param[out] autores Arreglo de IDs en orden de recorrido de la tabla local.
 * Puede ser NULL cuando cap es cero.
 * @param[in] cap Cantidad de elementos uint16_t que caben en autores.
 * @param[out] cantidad Total de coincidencias vigentes, incluso si supera cap.
 * @return DSM_OK si todas caben. DSM_ERR_NOMEM si solo cabe un prefijo, incluso
 * al consultar con cap cero si hay participantes. DSM_ERR_INVAL por argumentos inválidos.
 * @note Los avisos llegan de forma asíncrona. La aplicación ordena y confirma
 * los IDs antes de usarlos como los integrantes de un cálculo distribuido.
 */
int dsm_aviso_participantes(dsm_context_t *ctx, const char *clave, uint16_t *autores,
                            size_t cap, size_t *cantidad);
/**
 * @brief Obtiene la publicación vigente de un autor concreto desde la tabla local.
 * @param[in] ctx Contexto activo del observador.
 * @param[in] clave Clave terminada en NUL de 1 a 31 bytes.
 * @param[in] autor ID del nodo publicador dentro del INI.
 * @param[out] datos Destino para la copia, o NULL si n vale cero.
 * @param[in] n Tamaño exacto esperado en bytes, entre 0 y 128.
 * @return DSM_OK si se copió el aviso. DSM_ERR_ESTADO si la sesión, el tamaño
 * o la clave no coinciden con una publicación vigente de ese autor.
 * DSM_ERR_INVAL por ID fuera de rango, clave, longitud o puntero inválidos.
 * @note Las aplicaciones usan esta variante para leer la propuesta o los IDs de
 * reserva publicados por el líder, aun cuando otros nodos utilicen la misma clave.
 */
int dsm_aviso_consultar_autor(dsm_context_t *ctx, const char *clave,
                              uint16_t autor, void *datos, size_t n);
/**
 * @brief Retira la publicación del nodo local para una clave.
 * @param[in] ctx Contexto activo del autor.
 * @param[in] clave Clave propia terminada en NUL, de 1 a 31 bytes.
 * @return DSM_OK si se registró la retirada o el aviso propio ya estaba ausente
 * o retirado. DSM_ERR_INVAL por clave inválida. DSM_ERR_ESTADO si el servicio
 * está cerrando o agotó la secuencia necesaria para publicar el cambio.
 * @note Las consultas locales dejan de ver ese valor. La retirada se difunde
 * con una generación nueva cuando corresponde. La entrada puede reutilizarse
 * al agotar los envíos pendientes, sin confirmación de entrega de todos los pares.
 */
int dsm_aviso_retirar(dsm_context_t *ctx, const char *clave);
/**
 * @brief Consulta el identificador de la sesión de nodo que conoce la capa de coherencia.
 * @param[in] ctx Contexto activo del observador.
 * @param[in] autor ID del nodo local o de un par configurado.
 * @param[out] sesion Token de 64 bits distinto de cero. Identifica la instancia
 * del nodo y puede mantenerse al reconectar el transporte de esa misma instancia.
 * @return DSM_OK si obtuvo el token, DSM_ERR_INVAL por argumentos inválidos,
 * DSM_ERR_NODO si el par está desconectado o DSM_ERR_PROTO si carece de token
 * pese a tener un enlace activo.
 * @note Lee el token local o el aprendido en el alta TCP. El token de grupo
 * utilizado para coordinar una ejecución de una aplicación tiene otro alcance.
 */
int dsm_aviso_sesion(dsm_context_t *ctx, uint16_t autor, uint64_t *sesion);
/**
 * @brief Calcula una dirección prestada de Ar para un byte lógico de la reserva.
 * @param[in] ctx Contexto activo que conserva la vinculación.
 * @param[in] id ID de una reserva vinculada en ctx.
 * @param[in] offset Desplazamiento en bytes, estrictamente menor que el tamaño lógico.
 * @param[out] salida Recibe Ar+offset solo al éxito; debe ser no nula.
 * @return DSM_OK o un DSM_ERR_* de contexto, rango o estado.
 * @retval DSM_ERR_INVAL salida nula, ID no vinculado u offset fuera del rango lógico.
 * @retval DSM_ERR_OCUPADA La vinculación está retirándose.
 * @note Traducir no registra el hilo, trae páginas ni cambia permisos MSI.
 * Antes de desreferenciar se debe registrar el trabajador y coordinar la
 * vigencia de la reserva. Para errores DSM recuperables, usar leer/escribir.
 * @warning Una falta irresoluble sobre Ar termina el proceso con SIGSEGV.
 * El puntero deja de ser utilizable al retirar, desvincular o liberar la vista
 * o al cerrar ctx; la traducción no prolonga su ciclo de vida.
 */
int dsm_traducir(dsm_context_t *ctx, dsm_reserva_id_t id, size_t offset, void **salida);

#endif
