#include "directory.h"

#include "network.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

struct dsm_reserva_dir {
    dsm_region_info_t info;
    dsm_region_info_t aporte;
    dsm_vistas_t vista;
    uint64_t token;
    uint64_t abiertos;
    uint32_t orden_preparado;
    uint32_t max_preparado;
    uint32_t tramos_preparados;
    int confirmada;
    int vinculada;
    int contada;
    int publicada;
    struct dsm_reserva_dir *siguiente;
};

struct dsm_tumba {
    uint32_t id;
    uint64_t token;
    struct dsm_tumba *siguiente;
};

static struct dsm_tumba *buscar_tumba(const dsm_directorio_t *dir, uint32_t id)
{
    for (struct dsm_tumba *t = dir->tumbas; t != NULL; t = t->siguiente) {
        if (t->id == id) {
            return t;
        }
    }
    return NULL;
}

static struct dsm_tumba *buscar_tumba_token(const dsm_directorio_t *dir,
                                            uint32_t id, uint64_t token)
{
    for (struct dsm_tumba *t = dir->tumbas; t != NULL; t = t->siguiente) {
        if (t->id == id && t->token == token) {
            return t;
        }
    }
    return NULL;
}

static int recordar_terminal(dsm_directorio_t *dir, uint32_t id, uint64_t token)
{
    if (buscar_tumba_token(dir, id, token) != NULL) {
        return 0;
    }
    struct dsm_tumba *t = malloc(sizeof *t);
    if (t == NULL) {
        return -1;
    }
    *t = (struct dsm_tumba){.id = id, .token = token, .siguiente = dir->tumbas};
    dir->tumbas = t;
    return 0;
}

static struct dsm_reserva_dir *buscar_registro(const dsm_directorio_t *dir, uint32_t id)
{
    for (struct dsm_reserva_dir *reserva = dir->reservas; reserva != NULL;
         reserva = reserva->siguiente) {
        if (reserva->info.reserva_id == id) {
            return reserva;
        }
    }
    return NULL;
}

static struct dsm_reserva_dir *buscar_reserva(const dsm_directorio_t *dir, uint32_t id)
{
    return buscar_registro(dir, id);
}

static dsm_vistas_t *vista_pagina(const dsm_directorio_t *dir, size_t indice)
{
    return &buscar_reserva(dir, dir->paginas[indice].reserva)->vista;
}

static void *datos_pagina(const dsm_directorio_t *dir, size_t indice)
{
    return (unsigned char *)vista_pagina(dir, indice)->c_r +
           (size_t)dir->paginas[indice].orden * dir->page_size;
}

/* Un donante conserva Cr autoritativa aunque su aplicacion retire la vista. */
static int proteger(dsm_directorio_t *dir, size_t indice, int prot)
{
    struct dsm_reserva_dir *reserva = buscar_reserva(dir, dir->paginas[indice].reserva);
    return dsm_vistas_proteger_pagina(&reserva->vista, dir->paginas[indice].orden,
                                     dir->page_size, reserva->vinculada ? prot : PROT_NONE);
}

static int ampliar_paginas(dsm_directorio_t *dir, size_t cantidad)
{
    if (cantidad > SIZE_MAX - dir->npaginas) {
        errno = ENOMEM;
        return -1;
    }
    size_t necesarias = dir->npaginas + cantidad;
    if (necesarias <= dir->capacidad_paginas) {
        return 0;
    }
    if (necesarias > SIZE_MAX / sizeof *dir->paginas) {
        errno = ENOMEM;
        return -1;
    }
    size_t capacidad = dir->capacidad_paginas ? dir->capacidad_paginas : 8;
    while (capacidad < necesarias && capacidad <= SIZE_MAX / 2) {
        capacidad *= 2;
    }
    if (capacidad < necesarias || capacidad > SIZE_MAX / sizeof *dir->paginas) {
        capacidad = necesarias;
    }
    void *paginas = realloc(dir->paginas, capacidad * sizeof *dir->paginas);
    if (paginas == NULL) {
        return -1;
    }
    dir->paginas = paginas;
    dir->capacidad_paginas = capacidad;
    return 0;
}

/* Con mutex tomado. El llamador excluye trabajadores y campanas activas. */
static void desmontar_reserva(dsm_directorio_t *dir, struct dsm_reserva_dir *reserva)
{
    size_t salida = 0;
    for (size_t i = 0; i < dir->npaginas; i++) {
        if (dir->paginas[i].reserva != reserva->info.reserva_id) {
            dir->paginas[salida++] = dir->paginas[i];
        }
    }
    dir->npaginas = salida;
    for (uint32_t t = 0; t < reserva->aporte.cantidad_tramos; t++) {
        const dsm_tramo_t *tramo = &reserva->aporte.tramos[t];
        size_t inicial = (size_t)(tramo->gpn_inicial - dir->limites_gpn[dir->nodo_id]);
        for (uint32_t j = 0; j < tramo->cantidad; j++) {
            dir->ocupadas[inicial + j] = 0;
            dir->paginas_libres++;
        }
    }
    memset(&reserva->aporte, 0, sizeof reserva->aporte);
    if (reserva->vista.a_r != NULL) {
        dsm_vistas_cerrar(&reserva->vista);
        dir->nvistas--;
    }
    if (reserva->contada) {
        dir->reservas_globales--;
        reserva->contada = 0;
    }
    reserva->confirmada = 0;
    reserva->publicada = 0;
    reserva->vinculada = 0;
}

static void quitar_reserva(dsm_directorio_t *dir, struct dsm_reserva_dir *reserva)
{
    desmontar_reserva(dir, reserva);
    struct dsm_reserva_dir **enlace = &dir->reservas;
    while (*enlace != reserva) {
        enlace = &(*enlace)->siguiente;
    }
    *enlace = reserva->siguiente;
    free(reserva);
}

static int terminar_reserva(dsm_directorio_t *dir, struct dsm_reserva_dir *reserva)
{
    if (recordar_terminal(dir, reserva->info.reserva_id, reserva->token) != 0) {
        return -1;
    }
    quitar_reserva(dir, reserva);
    pthread_cond_broadcast(&dir->cola_cv);
    return 0;
}

int dsm_directorio_init(dsm_directorio_t *dir, const dsm_config_t *cfg, uint16_t nodo_id)
{
    if (dir == NULL) {
        return -1;
    }
    memset(dir, 0, sizeof *dir);
    if (cfg == NULL || cfg->cantidad_nodos > DSM_CONFIG_MAX_NODOS ||
        cfg->rpc_timeout_ms == 0 || cfg->page_size == 0) {
        return -1;
    }
    const dsm_nodo_cfg_t *nodo = dsm_config_nodo(cfg, nodo_id);
    if (nodo == NULL) {
        return -1;
    }
    for (uint16_t i = 0; i <= cfg->cantidad_nodos; i++) {
        uint64_t base = dsm_config_base(cfg, i);
        if (base == UINT64_MAX || base % cfg->page_size != 0 ||
            (i > 0 && base / cfg->page_size <= dir->limites_gpn[i - 1])) {
            return -1;
        }
        dir->limites_gpn[i] = base / cfg->page_size;
    }
    uint64_t paginas = nodo->memoria / cfg->page_size;
    if (paginas > UINT32_MAX || pthread_mutex_init(&dir->mutex, NULL) != 0) {
        return -1;
    }
    if (pthread_mutex_init(&dir->coordinador, NULL) != 0) {
        pthread_mutex_destroy(&dir->mutex);
        return -1;
    }
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) != 0) {
        pthread_mutex_destroy(&dir->coordinador);
        pthread_mutex_destroy(&dir->mutex);
        return -1;
    }
    int cola_lista = 0;
    int error = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (error == 0) {
        error = pthread_cond_init(&dir->cola_cv, &attr);
        cola_lista = error == 0;
    }
    if (error == 0) {
        error = pthread_cond_init(&dir->campana_cv, &attr);
    }
    pthread_condattr_destroy(&attr);
    if (error != 0) {
        if (cola_lista) {
            pthread_cond_destroy(&dir->cola_cv);
        }
        pthread_mutex_destroy(&dir->coordinador);
        pthread_mutex_destroy(&dir->mutex);
        return -1;
    }
    dir->nodo_id = nodo_id;
    dir->cantidad_nodos = cfg->cantidad_nodos;
    dir->page_size = cfg->page_size;
    dir->rpc_timeout_ms = cfg->rpc_timeout_ms;
    dir->memoria_inicial = nodo->memoria;
    dir->paginas_totales = (uint32_t)paginas;
    dir->paginas_libres = dir->paginas_totales;
    dir->siguiente_local = 1;
    dir->local = *nodo;
    dir->ocupadas = calloc(dir->paginas_totales, 1);
    if (dir->ocupadas == NULL) {
        pthread_cond_destroy(&dir->campana_cv);
        pthread_cond_destroy(&dir->cola_cv);
        pthread_mutex_destroy(&dir->coordinador);
        pthread_mutex_destroy(&dir->mutex);
        return -1;
    }
    dir->inicializado = 1;
    return 0;
}

void dsm_directorio_cerrar(dsm_directorio_t *dir)
{
    if (dir == NULL || !dir->inicializado) {
        return;
    }
    dsm_directorio_desactivar(dir);
    while (dir->reservas != NULL) {
        quitar_reserva(dir, dir->reservas);
    }
    free(dir->paginas);
    free(dir->ocupadas);
    while (dir->tumbas != NULL) {
        struct dsm_tumba *t = dir->tumbas;
        dir->tumbas = t->siguiente;
        free(t);
    }
    pthread_cond_destroy(&dir->campana_cv);
    pthread_cond_destroy(&dir->cola_cv);
    pthread_mutex_destroy(&dir->coordinador);
    pthread_mutex_destroy(&dir->mutex);
    free(dir->campana_buf);
    memset(dir, 0, sizeof *dir);
}

/* Los lectores solo modifican el mutex de un directorio inicializado. */
static pthread_mutex_t *mutex_de(const dsm_directorio_t *dir)
{
    return (pthread_mutex_t *)&dir->mutex;
}

static int bloquear(const dsm_directorio_t *dir)
{
    return dir != NULL && dir->inicializado && pthread_mutex_lock(mutex_de(dir)) == 0;
}

static int id_reserva_valido(const dsm_directorio_t *dir, uint32_t reserva_id)
{
    return (reserva_id >> 16) < dir->cantidad_nodos && (uint16_t)reserva_id != 0;
}

static uint16_t hogar_del_gpn(const dsm_directorio_t *dir, uint64_t gpn)
{
    for (uint16_t hogar = 0; hogar < dir->cantidad_nodos; hogar++) {
        if (gpn < dir->limites_gpn[hogar + 1]) {
            return hogar;
        }
    }
    return dir->cantidad_nodos;
}

/* Se llama con el mutex del directorio tomado. */
static size_t indice_reserva(const dsm_directorio_t *dir, uint32_t reserva_id)
{
    for (size_t i = 0; i < dir->npaginas; i++) {
        if (dir->paginas[i].reserva == reserva_id) {
            return i;
        }
    }
    return dir->npaginas;
}

/* Conserva first-fit cuando cabe en el manifiesto. Si hay demasiados huecos,
 * los tramos libres de mayor tamano permiten aprovechar hasta 16 fragmentos. */
static int seleccionar_tramos(const dsm_directorio_t *dir, size_t cantidad,
                               uint32_t limite, int parcial, dsm_region_info_t *info)
{
    size_t asignadas = 0;
    for (size_t i = 0; i < dir->paginas_totales && asignadas < cantidad;) {
        if (dir->ocupadas[i]) {
            i++;
            continue;
        }
        size_t inicio = i;
        while (i < dir->paginas_totales && !dir->ocupadas[i] &&
               i - inicio < cantidad - asignadas) {
            i++;
        }
        if (info->total_tramos == limite) {
            break;
        }
        size_t paginas = i - inicio;
        info->tramos[info->total_tramos++] = (dsm_tramo_t){
            .orden_inicial = (uint32_t)asignadas, .cantidad = (uint32_t)paginas,
            .gpn_inicial = dir->limites_gpn[dir->nodo_id] + inicio,
        };
        asignadas += paginas;
    }
    if (asignadas == cantidad) {
        info->cantidad_tramos = info->total_tramos;
        info->total_paginas = (uint32_t)cantidad;
        return 0;
    }

    dsm_tramo_t mayores[DSM_MAX_TRAMOS] = {{0}};
    size_t usados = 0;
    for (size_t i = 0; i < dir->paginas_totales;) {
        if (dir->ocupadas[i]) {
            i++;
            continue;
        }
        size_t inicio = i;
        while (i < dir->paginas_totales && !dir->ocupadas[i]) {
            i++;
        }
        size_t puesto;
        if (usados < limite) {
            puesto = usados++;
        } else {
            puesto = limite - 1u;
            if (i - inicio <= mayores[puesto].cantidad) {
                continue;
            }
        }
        mayores[puesto] = (dsm_tramo_t){
            .cantidad = (uint32_t)(i - inicio),
            .gpn_inicial = dir->limites_gpn[dir->nodo_id] + inicio,
        };
        while (puesto > 0 && mayores[puesto].cantidad > mayores[puesto - 1u].cantidad) {
            dsm_tramo_t tmp = mayores[puesto - 1u];
            mayores[puesto - 1u] = mayores[puesto];
            mayores[puesto--] = tmp;
        }
    }
    size_t elegidos = 0;
    size_t disponibles = 0;
    while (elegidos < usados && disponibles < cantidad) {
        disponibles += mayores[elegidos++].cantidad;
    }
    if (disponibles < cantidad) {
        if (!parcial || disponibles == 0) {
            errno = ENOSPC;
            return -1;
        }
        cantidad = disponibles;
    }
    /* Publica en orden de GPN los tramos elegidos, con orden virtual continuo. */
    for (size_t i = 1; i < elegidos; i++) {
        for (size_t j = i; j > 0 && mayores[j].gpn_inicial < mayores[j - 1u].gpn_inicial; j--) {
            dsm_tramo_t tmp = mayores[j - 1u];
            mayores[j - 1u] = mayores[j];
            mayores[j] = tmp;
        }
    }
    asignadas = 0;
    memset(info->tramos, 0, sizeof info->tramos);
    for (size_t i = 0; i < elegidos; i++) {
        size_t paginas = mayores[i].cantidad;
        if (paginas > cantidad - asignadas) {
            paginas = cantidad - asignadas;
        }
        info->tramos[i] = (dsm_tramo_t){
            .orden_inicial = (uint32_t)asignadas, .cantidad = (uint32_t)paginas,
            .gpn_inicial = mayores[i].gpn_inicial,
        };
        asignadas += paginas;
    }
    info->cantidad_tramos = info->total_tramos = (uint32_t)elegidos;
    info->total_paginas = (uint32_t)cantidad;
    return 0;
}

int dsm_directorio_reservar(dsm_directorio_t *dir, size_t bytes,
                             dsm_pagina_desc_t *salida, dsm_vistas_t **vista)
{
    if (!bloquear(dir)) {
        errno = EINVAL;
        return -1;
    }
    int resultado = -1;
    if (bytes == 0) {
        errno = EINVAL;
        goto salir;
    }
    if (bytes > dir->memoria_inicial || bytes > SIZE_MAX - (dir->page_size - 1u)) {
        errno = ENOSPC;
        goto salir;
    }
    size_t cantidad = 1u + (bytes - 1u) / dir->page_size;
    if (dir->cerrando) {
        errno = ECANCELED;
        goto salir;
    }
    if (cantidad > dir->paginas_libres || dir->siguiente_local > UINT16_MAX) {
        errno = ENOSPC;
        goto salir;
    }
    dsm_region_info_t info = {
        .total_paginas = (uint32_t)cantidad, .tamano_bytes = bytes,
    };
    if (seleccionar_tramos(dir, cantidad, DSM_TRAMOS_POR_HOGAR, 0, &info) != 0 ||
        ampliar_paginas(dir, cantidad) != 0) {
        goto salir;
    }
    struct dsm_reserva_dir *reserva = calloc(1, sizeof *reserva);
    if (reserva == NULL) {
        goto salir;
    }
    if (dsm_vistas_crear(cantidad * dir->page_size, &reserva->vista) != 0) {
        free(reserva);
        goto salir;
    }
    if (dsm_vistas_respaldar_tramo(&reserva->vista, 0, cantidad * dir->page_size) != 0 ||
        dsm_vistas_proteger(&reserva->vista, PROT_READ | PROT_WRITE) != 0) {
        int error = errno;
        dsm_vistas_cerrar(&reserva->vista);
        free(reserva);
        errno = error;
        goto salir;
    }
    info.reserva_id = ((uint32_t)dir->nodo_id << 16) | dir->siguiente_local++;
    reserva->info = info;
    reserva->aporte = info;
    reserva->confirmada = 1;
    reserva->vinculada = 1;
    reserva->contada = 1;
    reserva->siguiente = dir->reservas;
    dir->reservas = reserva;
    size_t primera = dir->npaginas;
    for (uint32_t t = 0; t < info.cantidad_tramos; t++) {
        const dsm_tramo_t *tramo = &info.tramos[t];
        for (uint32_t k = 0; k < tramo->cantidad; k++) {
            uint32_t orden = tramo->orden_inicial + k;
            uint64_t gpn = tramo->gpn_inicial + k;
            dir->paginas[dir->npaginas++] = (dsm_pagina_desc_t){
                .gpn = gpn, .hogar = dir->nodo_id, .poseedor = dir->nodo_id,
                .reserva = info.reserva_id, .msi = DSM_MSI_M,
                .mascara_copias = UINT64_C(1) << dir->nodo_id, .orden = orden,
                .ubicacion_local = (unsigned char *)reserva->vista.a_r +
                                   (size_t)orden * dir->page_size,
            };
            dir->ocupadas[gpn - dir->limites_gpn[dir->nodo_id]] = 1;
        }
    }
    dir->nvistas++;
    dir->paginas_libres -= (uint32_t)cantidad;
    dir->reservas_globales++;
    if (salida != NULL) {
        *salida = dir->paginas[primera];
    }
    if (vista != NULL) {
        *vista = &reserva->vista;
    }
    resultado = 0;

salir:
    pthread_mutex_unlock(&dir->mutex);
    return resultado;
}

int dsm_directorio_publicar_reserva(dsm_directorio_t *dir, uint32_t id)
{
    if (!bloquear(dir)) {
        errno = EINVAL;
        return -1;
    }
    struct dsm_reserva_dir *reserva = buscar_reserva(dir, id);
    int error = 0;
    if (reserva == NULL) {
        error = ENOENT;
    } else if ((id >> 16) != dir->nodo_id) {
        error = EPERM;
    } else if (dir->cerrando) {
        error = ECANCELED;
    } else if (!reserva->confirmada) {
        error = EBUSY;
    } else {
        reserva->publicada = 1;
    }
    pthread_mutex_unlock(&dir->mutex);
    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}

int dsm_directorio_reservar_pagina(dsm_directorio_t *dir, dsm_pagina_desc_t *salida)
{
    dsm_pagina_desc_t pagina;
    if (dsm_directorio_reservar(dir, dir == NULL ? 0 : dir->page_size, &pagina, NULL) != 0 ||
        dsm_directorio_publicar_reserva(dir, pagina.reserva) != 0) {
        return -1;
    }
    if (salida != NULL) {
        *salida = pagina;
    }
    return 0;
}

int dsm_directorio_info(dsm_directorio_t *dir, uint32_t id, dsm_region_info_t *info)
{
    if (info == NULL || !bloquear(dir)) {
        errno = EINVAL;
        return -1;
    }
    struct dsm_reserva_dir *reserva = buscar_reserva(dir, id);
    int error = reserva == NULL ? ENOENT : !reserva->confirmada ? EBUSY : 0;
    if (error == 0) {
        *info = reserva->info;
    }
    pthread_mutex_unlock(&dir->mutex);
    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}

int dsm_directorio_liberar(dsm_directorio_t *dir, uint32_t id)
{
    if (dir == NULL || !dir->inicializado) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&dir->coordinador);
    pthread_mutex_lock(&dir->mutex);
    struct dsm_reserva_dir *reserva = buscar_reserva(dir, id);
    int error = 0;
    if (reserva == NULL) {
        error = ENOENT;
    } else if ((id >> 16) != dir->nodo_id) {
        error = EPERM;
    } else if (reserva->abiertos != 0) {
        error = EBUSY;
    } else {
        for (size_t i = 0; i < dir->npaginas; i++) {
            const dsm_pagina_desc_t *pagina = &dir->paginas[i];
            if (pagina->reserva == id &&
                ((pagina->mascara_copias & ~(UINT64_C(1) << dir->nodo_id)) ||
                 (pagina->msi == DSM_MSI_M && pagina->poseedor != dir->nodo_id))) {
                error = EBUSY;
                break;
            }
        }
        if (error == 0 && terminar_reserva(dir, reserva) != 0) {
            error = errno;
        }
    }
    pthread_mutex_unlock(&dir->mutex);
    pthread_mutex_unlock(&dir->coordinador);
    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}

int dsm_directorio_desvincular(dsm_directorio_t *dir, uint32_t id)
{
    if (dir == NULL || !dir->inicializado) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&dir->coordinador);
    pthread_mutex_lock(&dir->mutex);
    struct dsm_reserva_dir *reserva = buscar_reserva(dir, id);
    int error = reserva == NULL ? ENOENT : (id >> 16) == dir->nodo_id ? EPERM : 0;
    if (error == 0 && reserva->aporte.total_paginas != 0) {
        if (dsm_vistas_proteger(&reserva->vista, PROT_NONE) != 0) {
            error = errno;
        } else {
            reserva->vinculada = 0;
        }
    } else if (error == 0) {
        quitar_reserva(dir, reserva);
    }
    pthread_mutex_unlock(&dir->mutex);
    pthread_mutex_unlock(&dir->coordinador);
    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}

int dsm_directorio_preparar_cierre(dsm_directorio_t *dir)
{
    if (!bloquear(dir)) {
        errno = EINVAL;
        return -1;
    }
    int error = 0;
    for (struct dsm_reserva_dir *r = dir->reservas; r != NULL; r = r->siguiente) {
        if (((r->info.reserva_id >> 16) == dir->nodo_id && r->abiertos != 0) ||
            ((r->info.reserva_id >> 16) != dir->nodo_id && r->aporte.total_paginas != 0)) {
            error = EBUSY;
            break;
        }
    }
    if (error == 0) {
        dir->cerrando = 1;
    }
    pthread_mutex_unlock(&dir->mutex);
    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}

/* La CLI mantiene red y MSI disponibles mientras devuelve capacidad prestada. */
int dsm_directorio_esperar_fin_aportes(dsm_directorio_t *dir)
{
    if (!bloquear(dir)) {
        errno = EINVAL;
        return -1;
    }
    dir->cerrando = 1;
    int anunciado = 0;
    int error = 0;
    for (;;) {
        int pendientes = 0;
        for (struct dsm_reserva_dir *r = dir->reservas; r != NULL; r = r->siguiente) {
            if ((r->info.reserva_id >> 16) != dir->nodo_id && r->aporte.total_paginas != 0) {
                pendientes = 1;
                break;
            }
        }
        if (!pendientes) {
            break;
        }
        if (!anunciado) {
            printf("nodo %u espera devolucion de aportes antes de cerrar\n", dir->nodo_id);
            fflush(stdout);
            anunciado = 1;
        }
        error = pthread_cond_wait(&dir->cola_cv, &dir->mutex);
        if (error != 0) {
            break;
        }
    }
    pthread_mutex_unlock(&dir->mutex);
    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}

int dsm_directorio_consultar(const dsm_directorio_t *dir, uint32_t reserva_id,
                             uint32_t primer_tramo, dsm_region_info_t *info, uint16_t *estado)
{
    if (info == NULL || estado == NULL || !bloquear(dir)) {
        return -1;
    }
    struct dsm_reserva_dir *reserva = buscar_reserva(dir, reserva_id);
    if (reserva == NULL || !reserva->confirmada || !reserva->publicada ||
        (reserva_id >> 16) != dir->nodo_id || dir->cerrando || primer_tramo != 0) {
        *estado = DSM_ESTADO_RANGO;
        memset(info, 0, sizeof *info);
    } else {
        *info = reserva->info;
        *estado = DSM_ESTADO_OK;
    }
    pthread_mutex_unlock(mutex_de(dir));
    return 0;
}

static int manifiesto_valido(const dsm_directorio_t *dir, const dsm_region_info_t *info,
                              uint16_t *hogar)
{
    if (!id_reserva_valido(dir, info->reserva_id) || info->total_tramos == 0 ||
        info->total_tramos > DSM_MAX_TRAMOS || info->primer_tramo != 0 ||
        info->cantidad_tramos != info->total_tramos || info->total_paginas == 0 ||
        info->tamano_bytes == 0 ||
        1u + (info->tamano_bytes - 1u) / dir->page_size != info->total_paginas) {
        return 0;
    }
    uint64_t orden = 0;
    uint16_t primero = hogar_del_gpn(dir, info->tramos[0].gpn_inicial);
    if (primero >= dir->cantidad_nodos) {
        return 0;
    }
    for (uint32_t t = 0; t < info->cantidad_tramos; t++) {
        const dsm_tramo_t *tramo = &info->tramos[t];
        uint16_t actual = hogar_del_gpn(dir, tramo->gpn_inicial);
        if (tramo->cantidad == 0 || tramo->orden_inicial != orden ||
            actual >= dir->cantidad_nodos ||
            tramo->cantidad > dir->limites_gpn[actual + 1u] - tramo->gpn_inicial) {
            return 0;
        }
        uint64_t fin = tramo->gpn_inicial + tramo->cantidad;
        for (uint32_t anterior = 0; anterior < t; anterior++) {
            const dsm_tramo_t *otro = &info->tramos[anterior];
            if (tramo->gpn_inicial < otro->gpn_inicial + otro->cantidad &&
                otro->gpn_inicial < fin) {
                return 0;
            }
        }
        orden += tramo->cantidad;
    }
    *hogar = primero;
    return orden == info->total_paginas && orden <= SIZE_MAX / dir->page_size;
}

static int mismo_manifiesto(const dsm_region_info_t *a, const dsm_region_info_t *b)
{
    if (a->tamano_bytes != b->tamano_bytes || a->total_paginas != b->total_paginas ||
        a->cantidad_tramos != b->cantidad_tramos) {
        return 0;
    }
    for (uint32_t i = 0; i < a->cantidad_tramos; i++) {
        if (a->tramos[i].orden_inicial != b->tramos[i].orden_inicial ||
            a->tramos[i].cantidad != b->tramos[i].cantidad ||
            a->tramos[i].gpn_inicial != b->tramos[i].gpn_inicial) {
            return 0;
        }
    }
    return 1;
}

int dsm_directorio_id_reserva(dsm_directorio_t *dir, uint32_t *id)
{
    if (id == NULL || !bloquear(dir)) {
        errno = EINVAL;
        return -1;
    }
    int error = 0;
    if (dir->cerrando) {
        error = ECANCELED;
    } else if (dir->siguiente_local > UINT16_MAX) {
        error = ENOSPC;
    } else {
        *id = ((uint32_t)dir->nodo_id << 16) | dir->siguiente_local++;
    }
    pthread_mutex_unlock(&dir->mutex);
    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}

int dsm_directorio_preparar(dsm_directorio_t *dir, uint32_t id, uint64_t token,
                            uint32_t orden, uint32_t max_paginas, uint32_t max_tramos,
                            dsm_region_info_t *parcial)
{
    if (parcial == NULL || token == 0 || max_paginas == 0 || max_tramos == 0 ||
        max_tramos > DSM_TRAMOS_POR_HOGAR || !bloquear(dir)) {
        errno = EINVAL;
        return -1;
    }
    int resultado = -1;
    if (!id_reserva_valido(dir, id) || max_paginas > UINT32_MAX - orden || dir->cerrando) {
        errno = EINVAL;
        goto salir;
    }
    struct dsm_tumba *tumba = buscar_tumba_token(dir, id, token);
    if (tumba != NULL) {
        errno = ECANCELED;
        goto salir;
    }
    struct dsm_reserva_dir *reserva = buscar_reserva(dir, id);
    if (reserva != NULL) {
        if (reserva->token != token || reserva->orden_preparado != orden ||
            reserva->max_preparado != max_paginas || reserva->tramos_preparados != max_tramos) {
            errno = EPROTO;
        } else {
            *parcial = reserva->aporte;
            resultado = 0;
        }
        goto salir;
    }
    size_t cantidad = max_paginas < dir->paginas_libres ? max_paginas : dir->paginas_libres;
    dsm_region_info_t info = {.reserva_id = id};
    if (cantidad == 0 || seleccionar_tramos(dir, cantidad, max_tramos, 1, &info) != 0) {
        errno = ENOSPC;
        goto salir;
    }
    reserva = calloc(1, sizeof *reserva);
    if (reserva == NULL) {
        goto salir;
    }
    info.tamano_bytes = (uint64_t)info.total_paginas * dir->page_size;
    for (uint32_t t = 0; t < info.cantidad_tramos; t++) {
        info.tramos[t].orden_inicial += orden;
        size_t inicial = (size_t)(info.tramos[t].gpn_inicial - dir->limites_gpn[dir->nodo_id]);
        for (uint32_t k = 0; k < info.tramos[t].cantidad; k++) {
            dir->ocupadas[inicial + k] = 1;
        }
    }
    reserva->info = reserva->aporte = info;
    reserva->vista.fd = -1;
    reserva->token = token;
    reserva->orden_preparado = orden;
    reserva->max_preparado = max_paginas;
    reserva->tramos_preparados = max_tramos;
    reserva->siguiente = dir->reservas;
    dir->reservas = reserva;
    dir->paginas_libres -= info.total_paginas;
    *parcial = info;
    resultado = 0;

salir:
    pthread_mutex_unlock(&dir->mutex);
    return resultado;
}

static int aporte_coincide(const dsm_directorio_t *dir, const struct dsm_reserva_dir *reserva,
                           const dsm_region_info_t *info)
{
    uint64_t locales = 0;
    for (uint32_t t = 0; t < info->cantidad_tramos; t++) {
        const dsm_tramo_t *tramo = &info->tramos[t];
        if (hogar_del_gpn(dir, tramo->gpn_inicial) != dir->nodo_id) {
            continue;
        }
        locales += tramo->cantidad;
        for (uint32_t k = 0; k < tramo->cantidad; k++) {
            int encontrada = 0;
            for (uint32_t a = 0; a < reserva->aporte.cantidad_tramos; a++) {
                const dsm_tramo_t *aporte = &reserva->aporte.tramos[a];
                uint64_t gpn = tramo->gpn_inicial + k;
                if (gpn >= aporte->gpn_inicial && gpn - aporte->gpn_inicial < aporte->cantidad &&
                    tramo->orden_inicial + k == aporte->orden_inicial + gpn - aporte->gpn_inicial) {
                    encontrada = 1;
                    break;
                }
            }
            if (!encontrada) {
                return 0;
            }
        }
    }
    return locales == reserva->aporte.total_paginas;
}

int dsm_directorio_confirmar(dsm_directorio_t *dir, uint32_t id, uint64_t token,
                              const dsm_region_info_t *info)
{
    if (info == NULL || token == 0 || info->reserva_id != id || !bloquear(dir)) {
        errno = EINVAL;
        return -1;
    }
    int resultado = -1;
    uint16_t primero;
    if (!manifiesto_valido(dir, info, &primero) || dir->cerrando) {
        errno = EPROTO;
        goto salir;
    }
    struct dsm_tumba *tumba = buscar_tumba_token(dir, id, token);
    if (tumba != NULL) {
        errno = ECANCELED;
        goto salir;
    }
    struct dsm_reserva_dir *reserva = buscar_reserva(dir, id);
    if (reserva != NULL && reserva->token != token) {
        errno = EPROTO;
        goto salir;
    }
    if (reserva != NULL && reserva->confirmada) {
        if (mismo_manifiesto(&reserva->info, info)) {
            resultado = 0;
        } else {
            errno = EPROTO;
        }
        goto salir;
    }
    int nueva = reserva == NULL;
    if (nueva) {
        if ((id >> 16) != dir->nodo_id) {
            errno = EPROTO;
            goto salir;
        }
        reserva = calloc(1, sizeof *reserva);
        if (reserva == NULL) {
            goto salir;
        }
        reserva->vista.fd = -1;
        reserva->token = token;
        reserva->info.reserva_id = id;
    }
    if (!aporte_coincide(dir, reserva, info)) {
        errno = EPROTO;
        goto fallo;
    }
    for (size_t i = 0; i < dir->npaginas; i++) {
        for (uint32_t t = 0; t < info->cantidad_tramos; t++) {
            const dsm_tramo_t *tramo = &info->tramos[t];
            uint64_t gpn = dir->paginas[i].gpn;
            if (gpn >= tramo->gpn_inicial && gpn - tramo->gpn_inicial < tramo->cantidad) {
                errno = EPROTO;
                goto fallo;
            }
        }
    }
    if (ampliar_paginas(dir, info->total_paginas) != 0 ||
        dsm_vistas_crear((size_t)info->total_paginas * dir->page_size, &reserva->vista) != 0) {
        goto fallo;
    }
    for (uint32_t t = 0; t < info->cantidad_tramos; t++) {
        const dsm_tramo_t *tramo = &info->tramos[t];
        if (hogar_del_gpn(dir, tramo->gpn_inicial) == dir->nodo_id &&
            dsm_vistas_respaldar_tramo(&reserva->vista,
                                      (size_t)tramo->orden_inicial * dir->page_size,
                                      (size_t)tramo->cantidad * dir->page_size) != 0) {
            int error = errno;
            dsm_vistas_cerrar(&reserva->vista);
            errno = error;
            goto fallo;
        }
    }
    reserva->vinculada = (id >> 16) == dir->nodo_id;
    if (reserva->vinculada) {
        for (uint32_t t = 0; t < info->cantidad_tramos; t++) {
            const dsm_tramo_t *tramo = &info->tramos[t];
            if (hogar_del_gpn(dir, tramo->gpn_inicial) == dir->nodo_id) {
                for (uint32_t k = 0; k < tramo->cantidad; k++) {
                    if (dsm_vistas_proteger_pagina(&reserva->vista, tramo->orden_inicial + k,
                                                  dir->page_size, PROT_READ | PROT_WRITE) != 0) {
                        dsm_vistas_cerrar(&reserva->vista);
                        goto fallo;
                    }
                }
            }
        }
    }
    reserva->info = *info;
    reserva->confirmada = 1;
    if (nueva) {
        reserva->siguiente = dir->reservas;
        dir->reservas = reserva;
    }
    for (uint32_t t = 0; t < info->cantidad_tramos; t++) {
        const dsm_tramo_t *tramo = &info->tramos[t];
        uint16_t hogar = hogar_del_gpn(dir, tramo->gpn_inicial);
        for (uint32_t k = 0; k < tramo->cantidad; k++) {
            uint32_t orden = tramo->orden_inicial + k;
            int local = hogar == dir->nodo_id;
            dir->paginas[dir->npaginas++] = (dsm_pagina_desc_t){
                .gpn = tramo->gpn_inicial + k, .hogar = hogar, .reserva = id,
                .poseedor = local ? dir->nodo_id : DSM_POSEEDOR_NINGUNO,
                .msi = local ? DSM_MSI_M : DSM_MSI_I, .orden = orden,
                .mascara_copias = local ? UINT64_C(1) << dir->nodo_id : 0,
                .ubicacion_local = (unsigned char *)reserva->vista.a_r +
                                   (size_t)orden * dir->page_size,
            };
        }
    }
    dir->nvistas++;
    if ((id >> 16) == dir->nodo_id) {
        reserva->contada = 1;
        dir->reservas_globales++;
    }
    resultado = 0;
    goto salir;

fallo:
    if (nueva) {
        free(reserva);
    }
salir:
    pthread_mutex_unlock(&dir->mutex);
    return resultado;
}

static int asignacion_ocupada(const dsm_directorio_t *dir, const struct dsm_reserva_dir *reserva)
{
    if (reserva->abiertos != 0 ||
        ((reserva->info.reserva_id >> 16) != dir->nodo_id && reserva->vinculada)) {
        return 1;
    }
    for (size_t i = 0; i < dir->npaginas; i++) {
        const dsm_pagina_desc_t *p = &dir->paginas[i];
        if (p->reserva != reserva->info.reserva_id) {
            continue;
        }
        uint64_t bit = UINT64_C(1) << dir->nodo_id;
        if (p->hogar == dir->nodo_id ?
            ((p->mascara_copias & ~bit) || (p->msi == DSM_MSI_M && p->poseedor != dir->nodo_id)) :
            (p->mascara_copias & bit)) {
            return 1;
        }
    }
    return 0;
}

static int terminar_asignacion(dsm_directorio_t *dir, uint32_t id, uint64_t token, int cancelar)
{
    if (dir == NULL || !dir->inicializado || token == 0) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&dir->coordinador);
    pthread_mutex_lock(&dir->mutex);
    int error = 0;
    struct dsm_tumba *tumba = buscar_tumba_token(dir, id, token);
    struct dsm_reserva_dir *reserva = buscar_reserva(dir, id);
    if (!id_reserva_valido(dir, id)) {
        error = EINVAL;
    } else if (tumba != NULL) {
        error = 0;
    } else if (reserva == NULL) {
        if (recordar_terminal(dir, id, token) != 0) {
            error = errno;
        }
    } else if (reserva->token != token) {
        error = cancelar ? 0 : EPROTO;
    } else if ((cancelar && reserva->publicada) || asignacion_ocupada(dir, reserva)) {
        error = EBUSY;
    } else if (terminar_reserva(dir, reserva) != 0) {
        error = errno;
    }
    pthread_mutex_unlock(&dir->mutex);
    pthread_mutex_unlock(&dir->coordinador);
    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}

int dsm_directorio_cancelar(dsm_directorio_t *dir, uint32_t id, uint64_t token)
{
    return terminar_asignacion(dir, id, token, 1);
}

int dsm_directorio_soltar(dsm_directorio_t *dir, uint32_t id, uint64_t token)
{
    return terminar_asignacion(dir, id, token, 0);
}

int dsm_directorio_iniciar_retiro(dsm_directorio_t *dir, uint32_t id)
{
    if (!bloquear(dir)) {
        errno = EINVAL;
        return -1;
    }
    struct dsm_reserva_dir *reserva = buscar_reserva(dir, id);
    int error = reserva == NULL ? ENOENT :
                (id >> 16) != dir->nodo_id ? EPERM :
                reserva->abiertos != 0 ? EBUSY : 0;
    if (error == 0) {
        reserva->publicada = 0;
    }
    pthread_mutex_unlock(&dir->mutex);
    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}

/* Abrir una vista existente no cambia la autoridad MSI ni materializa replicas I. */
static int vincular_vista(dsm_directorio_t *dir, struct dsm_reserva_dir *reserva)
{
    if (reserva->vinculada) {
        return 0;
    }
    reserva->vinculada = 1;
    uint64_t bit = UINT64_C(1) << dir->nodo_id;
    for (size_t i = 0; i < dir->npaginas; i++) {
        const dsm_pagina_desc_t *pagina = &dir->paginas[i];
        if (pagina->reserva != reserva->info.reserva_id) {
            continue;
        }
        int prot = PROT_NONE;
        if (!pagina->perdida && (pagina->mascara_copias & bit) != 0) {
            if (pagina->msi == DSM_MSI_M && pagina->poseedor == dir->nodo_id) {
                prot = PROT_READ | PROT_WRITE;
            } else if (pagina->msi == DSM_MSI_S) {
                prot = PROT_READ;
            }
        }
        if (proteger(dir, i, prot) != 0) {
            int error = errno;
            reserva->vinculada = 0;
            (void)dsm_vistas_proteger(&reserva->vista, PROT_NONE);
            errno = error;
            return -1;
        }
    }
    return 0;
}

int dsm_directorio_instalar(dsm_directorio_t *dir, const dsm_region_info_t *info)
{
    if (info == NULL || !bloquear(dir)) {
        errno = EINVAL;
        return -1;
    }
    int resultado = -1;
    uint16_t primero;
    if (!manifiesto_valido(dir, info, &primero) || dir->cerrando) {
        errno = EPROTO;
        goto salir;
    }
    struct dsm_reserva_dir *anterior = buscar_reserva(dir, info->reserva_id);
    if (anterior != NULL) {
        if (!anterior->confirmada || !mismo_manifiesto(&anterior->info, info)) {
            errno = EPROTO;
        } else {
            resultado = vincular_vista(dir, anterior);
        }
        goto salir;
    }
    if ((info->reserva_id >> 16) == dir->nodo_id) {
        errno = EPROTO;
        goto salir;
    }
    for (uint32_t t = 0; t < info->cantidad_tramos; t++) {
        if (hogar_del_gpn(dir, info->tramos[t].gpn_inicial) == dir->nodo_id) {
            /* Un hogar solo puede montar capacidad que preparo y confirmo. */
            errno = EPROTO;
            goto salir;
        }
    }
    for (size_t i = 0; i < dir->npaginas; i++) {
        uint64_t gpn = dir->paginas[i].gpn;
        for (uint32_t t = 0; t < info->cantidad_tramos; t++) {
            const dsm_tramo_t *tramo = &info->tramos[t];
            if (gpn >= tramo->gpn_inicial && gpn - tramo->gpn_inicial < tramo->cantidad) {
                errno = EPROTO;
                goto salir;
            }
        }
    }
    if (ampliar_paginas(dir, info->total_paginas) != 0) {
        goto salir;
    }
    struct dsm_reserva_dir *reserva = calloc(1, sizeof *reserva);
    if (reserva == NULL) {
        goto salir;
    }
    size_t bytes = (size_t)info->total_paginas * dir->page_size;
    if (dsm_vistas_crear(bytes, &reserva->vista) != 0) {
        free(reserva);
        goto salir;
    }
    reserva->info = *info;
    reserva->confirmada = 1;
    reserva->vinculada = 1;
    reserva->siguiente = dir->reservas;
    dir->reservas = reserva;
    for (uint32_t t = 0; t < info->cantidad_tramos; t++) {
        const dsm_tramo_t *tramo = &info->tramos[t];
        uint16_t hogar = hogar_del_gpn(dir, tramo->gpn_inicial);
        for (uint32_t k = 0; k < tramo->cantidad; k++) {
            uint32_t orden = tramo->orden_inicial + k;
            dir->paginas[dir->npaginas++] = (dsm_pagina_desc_t){
                .gpn = tramo->gpn_inicial + k, .hogar = hogar, .reserva = info->reserva_id,
                .poseedor = DSM_POSEEDOR_NINGUNO, .msi = DSM_MSI_I, .orden = orden,
                .ubicacion_local = (unsigned char *)reserva->vista.a_r +
                                   (size_t)orden * dir->page_size,
            };
        }
    }
    dir->nvistas++;
    resultado = 0;

salir:
    pthread_mutex_unlock(&dir->mutex);
    return resultado;
}

int dsm_directorio_pagina_reserva(dsm_directorio_t *dir, uint32_t id, size_t orden,
                                  dsm_pagina_desc_t *pagina)
{
    if (pagina == NULL || !bloquear(dir)) {
        errno = EINVAL;
        return -1;
    }
    int encontrada = 0;
    for (size_t i = 0; i < dir->npaginas; i++) {
        if (dir->paginas[i].reserva == id && dir->paginas[i].orden == orden) {
            *pagina = dir->paginas[i];
            encontrada = 1;
            break;
        }
    }
    pthread_mutex_unlock(&dir->mutex);
    if (!encontrada) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}

int dsm_directorio_codificar_info(dsm_directorio_t *dir, const dsm_cabecera_t *pedido,
                                  const dsm_region_lookup_t *consulta, uint32_t id_msg,
                                  uint8_t *buf, size_t cap, size_t *n)
{
    if (dir == NULL || !dir->inicializado || pedido == NULL || consulta == NULL ||
        id_msg == 0 || buf == NULL || n == NULL || cap < DSM_CABECERA_BYTES) {
        errno = EINVAL;
        return -1;
    }
    if (pedido->origen >= dir->cantidad_nodos || pedido->origen == dir->nodo_id ||
        pedido->version != DSM_PROTO_VERSION || pedido->tipo != DSM_TIPO_REGION_LOOKUP ||
        pedido->banderas != 0 || pedido->destino != dir->nodo_id ||
        pedido->id_msg == 0 || pedido->id_corr != 0 || pedido->estado != DSM_ESTADO_OK) {
        errno = EPROTO;
        return -1;
    }
    pthread_mutex_lock(&dir->mutex);
    struct dsm_reserva_dir *reserva = buscar_reserva(dir, consulta->reserva_id);
    uint16_t estado = DSM_ESTADO_RANGO;
    if (reserva != NULL && reserva->confirmada && reserva->publicada && !dir->cerrando &&
        (consulta->reserva_id >> 16) == dir->nodo_id && consulta->primer_tramo == 0) {
        estado = DSM_ESTADO_OK;
    }
    dsm_cabecera_t cab = {
        .version = DSM_PROTO_VERSION, .tipo = DSM_TIPO_REGION_INFO,
        .banderas = DSM_FLAG_RESPUESTA, .longitud = DSM_CABECERA_BYTES,
        .origen = dir->nodo_id, .destino = pedido->origen, .id_msg = id_msg,
        .id_corr = pedido->id_msg, .estado = estado,
    };
    int rc;
    if (estado == DSM_ESTADO_OK) {
        rc = dsm_region_info_codificar(buf, cap, &cab, &reserva->info, n);
        if (rc == 0) {
            reserva->abiertos |= UINT64_C(1) << pedido->origen;
        }
    } else {
        rc = dsm_cabecera_codificar(buf, &cab);
        if (rc == 0) {
            *n = DSM_CABECERA_BYTES;
        }
    }
    pthread_mutex_unlock(&dir->mutex);
    return rc;
}

int dsm_directorio_responder(dsm_directorio_t *dir, int fd, uint16_t peer,
                             const dsm_cabecera_t *pedido,
                             const dsm_region_lookup_t *consulta, uint32_t *seq)
{
    if (pedido == NULL || seq == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (pedido->origen != peer) {
        errno = EPROTO;
        return -1;
    }
    uint8_t buf[DSM_REGION_INFO_MAX];
    size_t n;
    if (dsm_directorio_codificar_info(dir, pedido, consulta, *seq, buf, sizeof buf, &n) != 0) {
        return -1;
    }
    (*seq)++;
    return dsm_enviar_trama(fd, buf, n, dir->rpc_timeout_ms);
}

int dsm_directorio_publicar(dsm_directorio_t *dir, int fd, uint32_t *seq)
{
    if (dir == NULL || !dir->inicializado || seq == NULL || *seq == 0) {
        errno = EINVAL;
        return -1;
    }
    uint8_t buf[DSM_REGION_INFO_MAX];
    size_t n = 0;
    dsm_cabecera_t cab;
    dsm_region_lookup_t consulta;
    if (dsm_recibir_trama_plazo(fd, buf, sizeof buf, &n, dir->rpc_timeout_ms) != 0) {
        return -1;
    }
    if (dsm_region_lookup_decodificar(buf, n, &cab, &consulta) != 0) {
        errno = EPROTO;
        return -1;
    }
    return dsm_directorio_responder(dir, fd, cab.origen, &cab, &consulta, seq);
}

int dsm_directorio_solicitar(const dsm_directorio_t *dir, int fd, uint32_t reserva_id,
                            uint32_t *seq, uint32_t *id_lookup)
{
    uint16_t creador = (uint16_t)(reserva_id >> 16);
    if (dir == NULL || !dir->inicializado || seq == NULL || *seq == 0 || id_lookup == NULL ||
        creador == dir->nodo_id || !id_reserva_valido(dir, reserva_id)) {
        errno = EINVAL;
        return -1;
    }
    dsm_region_lookup_t consulta;
    consulta.reserva_id = reserva_id;
    consulta.primer_tramo = 0;
    dsm_cabecera_t cab;
    memset(&cab, 0, sizeof cab);
    cab.version = DSM_PROTO_VERSION;
    cab.tipo = DSM_TIPO_REGION_LOOKUP;
    cab.banderas = 0;
    cab.origen = dir->nodo_id;
    cab.destino = creador;
    cab.id_msg = (*seq)++;
    cab.id_corr = 0;
    cab.estado = DSM_ESTADO_OK;
    uint8_t buf[40];
    size_t n = 0;
    if (dsm_region_lookup_codificar(buf, sizeof buf, &cab, &consulta, &n) != 0) {
        errno = EPROTO;
        return -1;
    }
    if (dsm_enviar_trama(fd, buf, n, dir->rpc_timeout_ms) != 0) {
        return -1;
    }
    *id_lookup = cab.id_msg;
    return 0;
}

int dsm_directorio_recibir_info(dsm_directorio_t *dir, uint32_t reserva_id,
                                uint32_t id_lookup, const uint8_t *buf, size_t n,
                                dsm_cabecera_t *cab_out, dsm_region_info_t *info_out)
{
    uint16_t creador = (uint16_t)(reserva_id >> 16);
    if (dir == NULL || !dir->inicializado || id_lookup == 0 ||
        creador == dir->nodo_id || !id_reserva_valido(dir, reserva_id)) {
        errno = EINVAL;
        return -1;
    }
    dsm_cabecera_t resp;
    dsm_region_info_t info;
    if (dsm_region_info_decodificar(buf, n, &resp, &info) != 0 ||
        resp.banderas != DSM_FLAG_RESPUESTA || resp.id_corr != id_lookup ||
        resp.origen != creador || resp.destino != dir->nodo_id) {
        errno = EPROTO;
        return -1;
    }
    if (resp.estado != DSM_ESTADO_OK) {
        errno = resp.estado == DSM_ESTADO_RANGO ? ENOENT : EPROTO;
        return -1;
    }
    if (info.reserva_id != reserva_id) {
        errno = EPROTO;
        return -1;
    }
    if (dsm_directorio_instalar(dir, &info) != 0) {
        return -1;
    }
    if (cab_out != NULL) {
        *cab_out = resp;
    }
    if (info_out != NULL) {
        *info_out = info;
    }
    return 0;
}

int dsm_directorio_traer(dsm_directorio_t *dir, int fd, uint32_t reserva_id, uint32_t *seq,
                         dsm_cabecera_t *cab_out, dsm_region_info_t *info_out)
{
    uint32_t id_lookup;
    uint8_t buf[DSM_REGION_INFO_MAX];
    size_t n;
    if (dsm_directorio_solicitar(dir, fd, reserva_id, seq, &id_lookup) != 0 ||
        dsm_recibir_trama_plazo(fd, buf, sizeof buf, &n, dir->rpc_timeout_ms) != 0) {
        return -1;
    }
    return dsm_directorio_recibir_info(dir, reserva_id, id_lookup, buf, n, cab_out, info_out);
}

void *dsm_directorio_c_r_reserva(const dsm_directorio_t *dir, uint32_t reserva_id)
{
    if (!bloquear(dir)) {
        return NULL;
    }
    size_t i = indice_reserva(dir, reserva_id);
    void *gestor = i < dir->npaginas ? datos_pagina(dir, i) : NULL;
    pthread_mutex_unlock(mutex_de(dir));
    return gestor;
}

void *dsm_directorio_c_r(const dsm_directorio_t *dir)
{
    if (!bloquear(dir)) {
        return NULL;
    }
    void *gestor = dir->nvistas == 1 ? dir->reservas->vista.c_r : NULL;
    pthread_mutex_unlock(mutex_de(dir));
    return gestor;
}

int dsm_directorio_vista(dsm_directorio_t *dir, uint32_t reserva_id,
                         dsm_pagina_desc_t *desc, dsm_vistas_t **vista)
{
    if ((desc == NULL && vista == NULL) || !bloquear(dir)) {
        errno = EINVAL;
        return -1;
    }
    size_t i = indice_reserva(dir, reserva_id);
    if (i == dir->npaginas) {
        pthread_mutex_unlock(&dir->mutex);
        errno = ENOENT;
        return -1;
    }
    if (desc != NULL) {
        *desc = dir->paginas[i];
    }
    if (vista != NULL) {
        *vista = vista_pagina(dir, i);
    }
    pthread_mutex_unlock(&dir->mutex);
    return 0;
}

int dsm_directorio_servir_pagina(dsm_directorio_t *dir, uint16_t solicitante,
                                 const dsm_page_request_t *pedido, dsm_page_data_t *datos,
                                 void *imagen, size_t imagen_cap, int *cambio)
{
    if (pedido == NULL || datos == NULL || cambio == NULL || !bloquear(dir)) {
        errno = EINVAL;
        return -1;
    }
    int resultado = -1;
    if (solicitante >= dir->cantidad_nodos ||
        pedido->acceso != DSM_ACCESO_LECTURA || pedido->msi != DSM_MSI_I ||
        hogar_del_gpn(dir, pedido->gpn) != dir->nodo_id) {
        errno = EPROTO;
        goto salir;
    }
    size_t i = 0;
    while (i < dir->npaginas && dir->paginas[i].gpn != pedido->gpn) {
        i++;
    }
    if (i == dir->npaginas) {
        errno = ENOENT;
        goto salir;
    }
    dsm_pagina_desc_t *pagina = &dir->paginas[i];
    if (pagina->perdida || (pagina->msi != DSM_MSI_S &&
        (pagina->msi != DSM_MSI_M || pagina->poseedor != dir->nodo_id))) {
        errno = EPROTO;
        goto salir;
    }
    if (imagen != NULL && imagen_cap < dir->page_size) {
        errno = EINVAL;
        goto salir;
    }
    int era_modified = pagina->msi == DSM_MSI_M;
    if (era_modified) {
        if (pagina->version == UINT32_MAX) {
            errno = EOVERFLOW;
            goto salir;
        }
        if (proteger(dir, i, PROT_READ) != 0) {
            goto salir;
        }
        pagina->msi = DSM_MSI_S;
        pagina->poseedor = DSM_POSEEDOR_NINGUNO;
        pagina->version++;
    }
    if (solicitante == dir->nodo_id &&
        proteger(dir, i, PROT_READ) != 0) {
        goto salir;
    }
    pagina->mascara_copias |= UINT64_C(1) << solicitante;
    *datos = (dsm_page_data_t){
        .gpn = pagina->gpn,
        .proc_id = pedido->proc_id,
        .reserva_id = pagina->reserva,
        .orden = pagina->orden,
        .hogar = pagina->hogar,
        .poseedor = pagina->poseedor,
        .msi = pagina->msi,
        .permiso = DSM_PAGE_PERMISO_LEER,
        .mascara_copias = pagina->mascara_copias,
        .page_nbytes = dir->page_size,
        .version = pagina->version,
        .datos = datos_pagina(dir, i),
    };
    if (imagen != NULL) {
        memcpy(imagen, datos_pagina(dir, i), dir->page_size);
        datos->datos = imagen;
    }
    *cambio = era_modified;
    resultado = 0;

salir:
    pthread_mutex_unlock(&dir->mutex);
    return resultado;
}

int dsm_directorio_compartir_pagina(dsm_directorio_t *dir, uint32_t reserva_id,
                                     const dsm_page_data_t *datos)
{
    if (datos == NULL || !bloquear(dir)) {
        errno = EINVAL;
        return -1;
    }
    int resultado = -1;
    size_t i = indice_reserva(dir, reserva_id);
    if (i == dir->npaginas) {
        errno = ENOENT;
        goto salir;
    }
    dsm_pagina_desc_t *pagina = &dir->paginas[i];
    uint64_t bits_esperados = (UINT64_C(1) << pagina->hogar) | (UINT64_C(1) << dir->nodo_id);
    uint64_t bits_validos = (UINT64_C(1) << dir->cantidad_nodos) - 1;
    if (pagina->msi != DSM_MSI_I || datos->msi != DSM_MSI_S ||
        datos->reserva_id != reserva_id || datos->gpn != pagina->gpn ||
        datos->orden != pagina->orden || datos->hogar != pagina->hogar ||
        datos->poseedor != DSM_POSEEDOR_NINGUNO || datos->version <= pagina->version ||
        datos->permiso != DSM_PAGE_PERMISO_LEER || datos->banderas != 0 ||
        datos->page_nbytes != dir->page_size || datos->datos == NULL ||
        (datos->mascara_copias & bits_esperados) != bits_esperados ||
        (datos->mascara_copias & ~bits_validos) != 0) {
        errno = EPROTO;
        goto salir;
    }
    memcpy(datos_pagina(dir, i), datos->datos, dir->page_size);
    pagina->poseedor = datos->poseedor;
    pagina->msi = DSM_MSI_S;
    pagina->version = datos->version;
    pagina->mascara_copias = datos->mascara_copias;
    resultado = 0;

salir:
    pthread_mutex_unlock(&dir->mutex);
    return resultado;
}

static size_t indice_gpn(const dsm_directorio_t *dir, uint64_t gpn)
{
    for (size_t i = 0; i < dir->npaginas; i++) {
        if (dir->paginas[i].gpn == gpn) {
            return i;
        }
    }
    return dir->npaginas;
}

static int bit_de(uint64_t mascara, uint16_t id)
{
    return id < 64 && (mascara & (UINT64_C(1) << id)) != 0;
}

static dsm_cabecera_t cabecera_de(const dsm_directorio_t *dir, uint8_t tipo, uint16_t destino,
                                  uint32_t id)
{
    dsm_cabecera_t cab;
    memset(&cab, 0, sizeof cab);
    cab.version = DSM_PROTO_VERSION;
    cab.tipo = tipo;
    cab.origen = dir->nodo_id;
    cab.destino = destino;
    cab.id_msg = id;
    cab.estado = DSM_ESTADO_OK;
    return cab;
}

static int enviar_fuera(dsm_directorio_t *dir, uint16_t destino, const uint8_t *buf, size_t n)
{
    if (dir->enviar == NULL) {
        errno = EINVAL;
        return -1;
    }
    return dir->enviar(dir->envio_ctx, destino, buf, n);
}

static uint16_t estado_errno(int error)
{
    if (error == ETIMEDOUT) {
        return DSM_ESTADO_PLAZO;
    }
    if (error == ENOTCONN || error == ECONNRESET || error == EPIPE) {
        return DSM_ESTADO_FUERA;
    }
    if (error == EAGAIN) {
        return DSM_ESTADO_COLA;
    }
    if (error == EPROTO) {
        return DSM_ESTADO_CONFLICTO;
    }
    if (error == EPERM) {
        return DSM_ESTADO_NO_HOGAR;
    }
    return DSM_ESTADO_RANGO;
}

static int enviar_pagina(dsm_directorio_t *dir, uint16_t destino, uint32_t id_corr, uint16_t estado,
                         const dsm_page_data_t *datos)
{
    uint32_t id = dir->nuevo_id(dir->envio_ctx);
    if (id == 0) {
        errno = EINVAL;
        return -1;
    }
    dsm_cabecera_t cab = cabecera_de(dir, DSM_TIPO_PAGE_DATA, destino, id);
    cab.banderas = DSM_FLAG_RESPUESTA;
    cab.id_corr = id_corr;
    cab.estado = estado;
    size_t cap = dir->campana_cap;
    uint8_t *buf = malloc(cap);
    if (buf == NULL) {
        return -1;
    }
    size_t n = 0;
    int rc = dsm_page_data_codificar(buf, cap, &cab, estado == DSM_ESTADO_OK ? datos : NULL, &n);
    if (rc == 0) {
        rc = enviar_fuera(dir, destino, buf, n);
    }
    if (rc == 0 && estado == DSM_ESTADO_OK && datos != NULL) {
        dsm_directorio_contar(dir, DSM_CONTA_PAG_ENV, 1);
        dsm_directorio_contar(dir, DSM_CONTA_BYTES_ENV, datos->page_nbytes);
        printf("nodo %u PAGE_DATA enviado par %u reserva %" PRIu32 " gpn %" PRIu64
               " bytes %" PRIu32 " trama %zu corr %" PRIu32 "\n",
               dir->nodo_id, destino, datos->reserva_id, datos->gpn, datos->page_nbytes, n,
               id_corr);
    }
    free(buf);
    return rc;
}

static void campana_abrir(dsm_directorio_t *dir, uint16_t par, uint32_t id)
{
    pthread_mutex_lock(&dir->mutex);
    dir->campana_espera = 1;
    dir->campana_par = par;
    dir->campana_id = id;
    dir->campana_error = dir->servicio_parar ? ECANCELED : 0;
    dir->tiene_datos = 0;
    dir->tiene_ack = 0;
    dir->datos_n = 0;
    pthread_mutex_unlock(&dir->mutex);
}

static void campana_cerrar(dsm_directorio_t *dir)
{
    pthread_mutex_lock(&dir->mutex);
    dir->campana_espera = 0;
    pthread_mutex_unlock(&dir->mutex);
}

static int plazo_operacion(const dsm_directorio_t *dir, struct timespec *plazo)
{
    if (clock_gettime(CLOCK_MONOTONIC, plazo) != 0) {
        return -1;
    }
    plazo->tv_sec += dir->rpc_timeout_ms / 1000u;
    plazo->tv_nsec += (long)(dir->rpc_timeout_ms % 1000u) * 1000000L;
    if (plazo->tv_nsec >= 1000000000L) {
        plazo->tv_sec++;
        plazo->tv_nsec -= 1000000000L;
    }
    return 0;
}

static int plazo_vigente(const struct timespec *plazo)
{
    struct timespec ahora;
    if (clock_gettime(CLOCK_MONOTONIC, &ahora) != 0) {
        return 0;
    }
    if (ahora.tv_sec > plazo->tv_sec ||
        (ahora.tv_sec == plazo->tv_sec && ahora.tv_nsec >= plazo->tv_nsec)) {
        errno = ETIMEDOUT;
        return 0;
    }
    return 1;
}

static int campana_esperar(dsm_directorio_t *dir, int quiere_datos, int quiere_ack, uint8_t *ack,
                           uint8_t *datos, size_t *datos_n, const struct timespec *limite)
{
    struct timespec plazo;
    if (limite == NULL) {
        if (plazo_operacion(dir, &plazo) != 0) {
            return -1;
        }
        limite = &plazo;
    }

    pthread_mutex_lock(&dir->mutex);
    int error = 0;
    while (error == 0 && dir->campana_error == 0 &&
           ((quiere_datos && !dir->tiene_datos) || (quiere_ack && !dir->tiene_ack))) {
        error = pthread_cond_timedwait(&dir->campana_cv, &dir->mutex, limite);
    }
    if (error == 0) {
        error = dir->campana_error;
    }
    if (error == 0) {
        if (quiere_ack && ack != NULL) {
            memcpy(ack, dir->ack_trama, DSM_CONTROL_BYTES);
        }
        if (quiere_datos && datos != NULL && datos_n != NULL) {
            if (dir->datos_n > dir->campana_cap) {
                error = EPROTO;
            } else {
                memcpy(datos, dir->campana_buf, dir->datos_n);
                *datos_n = dir->datos_n;
            }
        }
    }
    pthread_mutex_unlock(&dir->mutex);
    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}

static void pausa_poseedor(dsm_directorio_t *dir)
{
    pthread_mutex_lock(&dir->mutex);
    uint32_t retardo = dir->retardo_ms;
    pthread_mutex_unlock(&dir->mutex);
    if (retardo != 0) {
        usleep(retardo * 1000u);
    }
}

static int atender_invalidate(dsm_directorio_t *dir, uint16_t par, const uint8_t *trama, size_t n)
{
    dsm_cabecera_t pedido;
    dsm_invalidate_t msg;
    if (dsm_invalidate_decodificar(trama, n, &pedido, &msg) != 0 || pedido.origen != par) {
        errno = EPROTO;
        return -1;
    }
    pausa_poseedor(dir);

    uint16_t resultado = DSM_INVALIDA_YA_I;
    uint16_t estado = DSM_ESTADO_OK;
    pthread_mutex_lock(&dir->mutex);
    size_t i = indice_gpn(dir, msg.gpn);
    dsm_pagina_desc_t *p = i < dir->npaginas ? &dir->paginas[i] : NULL;
    /* El escritor puede pedir retirar la copia S del propio hogar. Las demas
     * replicas unicamente aceptan la orden de su hogar. */
    if (p == NULL || msg.escritor >= dir->cantidad_nodos ||
        (p->hogar == dir->nodo_id ? msg.escritor != par : p->hogar != par) ||
        (p->msi == DSM_MSI_M && p->poseedor == dir->nodo_id)) {
        estado = DSM_ESTADO_CONFLICTO;
    } else if (p->msi == DSM_MSI_S && bit_de(p->mascara_copias, dir->nodo_id)) {
        if (proteger(dir, i, PROT_NONE) != 0) {
            int error = errno;
            pthread_mutex_unlock(&dir->mutex);
            errno = error;
            return -1;
        }
        if (p->hogar != dir->nodo_id) {
            p->msi = DSM_MSI_I;
        }
        p->mascara_copias &= ~(UINT64_C(1) << dir->nodo_id);
        resultado = DSM_INVALIDA_HECHO;
    }
    pthread_mutex_unlock(&dir->mutex);
    dsm_directorio_contar(dir, DSM_CONTA_INV_REC, 1);
    printf("nodo %u INVALIDATE recibido par %u gpn %" PRIu64 " solicitud %" PRIu32 "\n",
           dir->nodo_id, par, msg.gpn, msg.id_solicitud);

    uint32_t id = dir->nuevo_id(dir->envio_ctx);
    dsm_cabecera_t cab = cabecera_de(dir, DSM_TIPO_INVALIDATE_ACK, par, id);
    cab.banderas = DSM_FLAG_RESPUESTA;
    cab.id_corr = pedido.id_msg;
    cab.estado = estado;
    dsm_invalidate_ack_t ack = {
        .gpn = msg.gpn, .id_solicitud = msg.id_solicitud, .resultado = resultado,
    };
    uint8_t buf[DSM_CONTROL_BYTES];
    size_t out = 0;
    if (id == 0 || dsm_invalidate_ack_codificar(buf, sizeof buf, &cab, &ack, &out) != 0) {
        errno = EPROTO;
        return -1;
    }
    int rc = enviar_fuera(dir, par, buf, out);
    if (rc == 0 && estado == DSM_ESTADO_OK) {
        printf("nodo %u INVALIDATE_ACK enviado par %u gpn %" PRIu64 " solicitud %" PRIu32 "\n",
               dir->nodo_id, par, msg.gpn, msg.id_solicitud);
    }
    return rc;
}

static int atender_downgrade(dsm_directorio_t *dir, uint16_t par, const uint8_t *trama, size_t n)
{
    dsm_cabecera_t pedido;
    dsm_downgrade_t msg;
    if (dsm_downgrade_decodificar(trama, n, &pedido, &msg) != 0 || pedido.origen != par) {
        errno = EPROTO;
        return -1;
    }
    printf("nodo %u PAGE_DOWNGRADE recibido par %u gpn %" PRIu64 " solicitud %" PRIu32 "\n",
           dir->nodo_id, par, msg.gpn, msg.id_solicitud);
    pausa_poseedor(dir);

    uint8_t *imagen = malloc(dir->page_size);
    if (imagen == NULL) {
        return -1;
    }
    dsm_page_data_t datos;
    memset(&datos, 0, sizeof datos);
    int tenia = 0;
    uint16_t resultado = DSM_DOWNGRADE_NO_M;
    pthread_mutex_lock(&dir->mutex);
    size_t i = indice_gpn(dir, msg.gpn);
    dsm_pagina_desc_t *pagina = i < dir->npaginas ? &dir->paginas[i] : NULL;
    if (pagina != NULL && pagina->msi == DSM_MSI_M && pagina->poseedor == dir->nodo_id &&
        pagina->hogar == par && msg.solicitante < dir->cantidad_nodos) {
        if (msg.motivo == DSM_MOTIVO_A_S && pagina->version == UINT32_MAX) {
            pthread_mutex_unlock(&dir->mutex);
            free(imagen);
            errno = EOVERFLOW;
            return -1;
        }
        int prot = msg.motivo == DSM_MOTIVO_A_S ? PROT_READ : PROT_NONE;
        if (proteger(dir, i, prot) != 0) {
            int error = errno;
            pthread_mutex_unlock(&dir->mutex);
            free(imagen);
            errno = error;
            return -1;
        }
        memcpy(imagen, datos_pagina(dir, i), dir->page_size);
        if (msg.motivo == DSM_MOTIVO_A_S) {
            pagina->version++;
            pagina->msi = DSM_MSI_S;
            pagina->poseedor = DSM_POSEEDOR_NINGUNO;
            pagina->mascara_copias =
                (UINT64_C(1) << pagina->hogar) | (UINT64_C(1) << dir->nodo_id) |
                (UINT64_C(1) << msg.solicitante);
        } else {
            pagina->msi = DSM_MSI_I;
            pagina->poseedor = DSM_POSEEDOR_NINGUNO;
            pagina->mascara_copias = 0;
        }
        datos = (dsm_page_data_t){
            .gpn = pagina->gpn,
            .proc_id = (uint32_t)msg.solicitante << 16,
            .reserva_id = pagina->reserva,
            .orden = pagina->orden,
            .hogar = pagina->hogar,
            .poseedor = DSM_POSEEDOR_NINGUNO,
            .msi = DSM_MSI_S,
            .permiso = DSM_PAGE_PERMISO_LEER,
            .mascara_copias = (UINT64_C(1) << pagina->hogar) | (UINT64_C(1) << dir->nodo_id) |
                              (UINT64_C(1) << msg.solicitante),
            .page_nbytes = dir->page_size,
            .version = pagina->version,
            .datos = imagen,
        };
        if (msg.motivo == DSM_MOTIVO_A_I) {
            datos.msi = DSM_MSI_S;
            datos.mascara_copias = (UINT64_C(1) << pagina->hogar) | (UINT64_C(1) << msg.solicitante);
        }
        tenia = 1;
        resultado = DSM_DOWNGRADE_HECHO;
    }
    pthread_mutex_unlock(&dir->mutex);

    int rc = 0;
    if (tenia) {
        uint32_t id = dir->nuevo_id(dir->envio_ctx);
        dsm_cabecera_t cab = cabecera_de(dir, DSM_TIPO_PAGE_DATA, par, id);
        cab.banderas = DSM_FLAG_RESPUESTA;
        cab.id_corr = pedido.id_msg;
        size_t cap = (size_t)dir->page_size + DSM_PAGE_DATA_BASE;
        uint8_t *buf = malloc(cap);
        size_t out = 0;
        if (buf == NULL || id == 0 ||
            dsm_page_data_codificar(buf, cap, &cab, &datos, &out) != 0 ||
            enviar_fuera(dir, par, buf, out) != 0) {
            rc = -1;
        }
        free(buf);
    }
    uint32_t id_ack = dir->nuevo_id(dir->envio_ctx);
    dsm_cabecera_t cab_ack = cabecera_de(dir, DSM_TIPO_PAGE_DOWNGRADE_ACK, par, id_ack);
    cab_ack.banderas = DSM_FLAG_RESPUESTA;
    cab_ack.id_corr = pedido.id_msg;
    dsm_downgrade_ack_t ack = {
        .gpn = msg.gpn, .id_solicitud = msg.id_solicitud, .resultado = resultado,
    };
    uint8_t raw[DSM_CONTROL_BYTES];
    size_t out = 0;
    if (rc == 0 &&
        (id_ack == 0 || dsm_downgrade_ack_codificar(raw, sizeof raw, &cab_ack, &ack, &out) != 0 ||
         enviar_fuera(dir, par, raw, out) != 0)) {
        rc = -1;
    }
    free(imagen);
    return rc;
}

static int pedir_bajada_plazo(dsm_directorio_t *dir, uint16_t poseedor, uint64_t gpn,
                              uint32_t id_solicitud, uint16_t solicitante, uint8_t motivo,
                              uint32_t version_esperada, uint8_t *imagen, uint32_t *version_out,
                              const struct timespec *limite)
{
    uint32_t id = dir->nuevo_id(dir->envio_ctx);
    if (id == 0) {
        errno = EINVAL;
        return -1;
    }
    campana_abrir(dir, poseedor, id);
    dsm_cabecera_t cab = cabecera_de(dir, DSM_TIPO_PAGE_DOWNGRADE, poseedor, id);
    dsm_downgrade_t msg = {
        .gpn = gpn, .id_solicitud = id_solicitud, .solicitante = solicitante, .motivo = motivo,
    };
    uint8_t envio[DSM_CONTROL_BYTES];
    size_t n = 0;
    if (dsm_downgrade_codificar(envio, sizeof envio, &cab, &msg, &n) != 0 ||
        enviar_fuera(dir, poseedor, envio, n) != 0) {
        campana_cerrar(dir);
        return -1;
    }
    uint8_t ack_raw[DSM_CONTROL_BYTES];
    uint8_t *recibo = malloc(dir->campana_cap);
    size_t recibo_n = 0;
    int rc = -1;
    if (recibo != NULL &&
        campana_esperar(dir, 1, 1, ack_raw, recibo, &recibo_n, limite) == 0) {
        dsm_cabecera_t cab_ack;
        dsm_downgrade_ack_t ack;
        dsm_cabecera_t cab_datos;
        dsm_page_data_t datos;
        dsm_pagina_desc_t pagina;
        uint64_t mascara = (UINT64_C(1) << dir->nodo_id) | (UINT64_C(1) << solicitante);
        if (motivo == DSM_MOTIVO_A_S) {
            mascara |= UINT64_C(1) << poseedor;
        }
        if (dsm_directorio_describir(dir, gpn, &pagina) == 0 &&
            dsm_downgrade_ack_decodificar(ack_raw, DSM_CONTROL_BYTES, &cab_ack, &ack) == 0 &&
            cab_ack.estado == DSM_ESTADO_OK && cab_ack.id_corr == id && ack.gpn == gpn &&
            ack.id_solicitud == id_solicitud &&
            ack.resultado == DSM_DOWNGRADE_HECHO &&
            dsm_page_data_decodificar(recibo, recibo_n, &cab_datos, &datos) == 0 &&
            cab_datos.estado == DSM_ESTADO_OK && cab_datos.id_corr == id && datos.gpn == gpn &&
            datos.hogar == dir->nodo_id && datos.reserva_id == pagina.reserva &&
            datos.orden == pagina.orden && datos.msi == DSM_MSI_S &&
            datos.proc_id == ((uint32_t)solicitante << 16) &&
            datos.poseedor == DSM_POSEEDOR_NINGUNO &&
            datos.mascara_copias == mascara &&
            datos.version == version_esperada && datos.page_nbytes == dir->page_size &&
            datos.datos != NULL) {
            memcpy(imagen, datos.datos, dir->page_size);
            *version_out = datos.version;
            rc = 0;
        } else {
            errno = EPROTO;
        }
    }
    if (rc != 0) {
        /* Tras enviar PAGE_DOWNGRADE, el antiguo M pudo haber renunciado
         * aunque falten sus datos o su ACK. No reutilizar una imagen incierta. */
        pthread_mutex_lock(&dir->mutex);
        size_t i = indice_gpn(dir, gpn);
        if (i < dir->npaginas) {
            dir->paginas[i].perdida = 1;
        }
        pthread_mutex_unlock(&dir->mutex);
    }
    free(recibo);
    campana_cerrar(dir);
    return rc;
}

static int pedir_bajada(dsm_directorio_t *dir, uint16_t poseedor, uint64_t gpn, uint32_t id_solicitud,
                        uint16_t solicitante, uint8_t motivo, uint32_t version_esperada,
                        uint8_t *imagen, uint32_t *version_out)
{
    return pedir_bajada_plazo(dir, poseedor, gpn, id_solicitud, solicitante, motivo,
                               version_esperada, imagen, version_out, NULL);
}

static int pedir_invalidacion_plazo(dsm_directorio_t *dir, uint16_t destino, uint64_t gpn,
                                    uint32_t id_solicitud, uint16_t escritor,
                                    const struct timespec *limite)
{
    uint32_t id = dir->nuevo_id(dir->envio_ctx);
    if (id == 0) {
        errno = EINVAL;
        return -1;
    }
    campana_abrir(dir, destino, id);
    dsm_cabecera_t cab = cabecera_de(dir, DSM_TIPO_INVALIDATE, destino, id);
    dsm_invalidate_t msg = {.gpn = gpn, .id_solicitud = id_solicitud, .escritor = escritor};
    uint8_t envio[DSM_CONTROL_BYTES];
    size_t n = 0;
    int rc = -1;
    uint8_t ack_raw[DSM_CONTROL_BYTES];
    int enviado = dsm_invalidate_codificar(envio, sizeof envio, &cab, &msg, &n) == 0 &&
                  enviar_fuera(dir, destino, envio, n) == 0;
    if (enviado) {
        dsm_directorio_contar(dir, DSM_CONTA_INV_ENV, 1);
    }
    if (enviado && campana_esperar(dir, 0, 1, ack_raw, NULL, NULL, limite) == 0) {
        dsm_cabecera_t cab_ack;
        dsm_invalidate_ack_t ack;
        if (dsm_invalidate_ack_decodificar(ack_raw, DSM_CONTROL_BYTES, &cab_ack, &ack) == 0 &&
            cab_ack.estado == DSM_ESTADO_OK && cab_ack.id_corr == id && ack.gpn == gpn &&
            ack.id_solicitud == id_solicitud && ack.resultado <= DSM_INVALIDA_YA_I) {
            rc = 0;
        } else {
            errno = EPROTO;
        }
    }
    campana_cerrar(dir);
    return rc;
}

static int pedir_invalidacion(dsm_directorio_t *dir, uint16_t destino, uint64_t gpn,
                              uint32_t id_solicitud, uint16_t escritor)
{
    return pedir_invalidacion_plazo(dir, destino, gpn, id_solicitud, escritor, NULL);
}

/* Serializado con las concesiones del hogar mediante coordinador. */
/* La cola del hogar conserva el orden entre PAGE_REQUEST y COPY_CLOSE. */
static int cerrar_copia_serializada(dsm_directorio_t *dir, uint16_t par, uint32_t id)
{
    struct timespec limite;
    if (plazo_operacion(dir, &limite) != 0) {
        return -1;
    }
    pthread_mutex_lock(&dir->mutex);
    struct dsm_reserva_dir *reserva = buscar_reserva(dir, id);
    if (!id_reserva_valido(dir, id) || par >= dir->cantidad_nodos) {
        pthread_mutex_unlock(&dir->mutex);
        errno = EINVAL;
        return -1;
    }
    if (reserva == NULL) {
        int terminada = buscar_tumba(dir, id) != NULL;
        pthread_mutex_unlock(&dir->mutex);
        if (!terminada) {
            errno = ENOENT;
            return -1;
        }
        return 0;
    }
    if (!reserva->confirmada) {
        pthread_mutex_unlock(&dir->mutex);
        return 0;
    }
    uint64_t bit = UINT64_C(1) << par;
    size_t primera = indice_reserva(dir, id);
    pthread_mutex_unlock(&dir->mutex);

    for (uint32_t orden = 0; orden < reserva->info.total_paginas; orden++) {
        if (!plazo_vigente(&limite)) {
            return -1;
        }
        pthread_mutex_lock(&dir->mutex);
        size_t i = primera + orden;
        if (i >= dir->npaginas || dir->paginas[i].reserva != id ||
            dir->paginas[i].orden != orden) {
            pthread_mutex_unlock(&dir->mutex);
            errno = EPROTO;
            return -1;
        }
        dsm_pagina_desc_t *p = &dir->paginas[i];
        if (p->hogar != dir->nodo_id) {
            pthread_mutex_unlock(&dir->mutex);
            continue;
        }
        if (p->perdida && ((p->mascara_copias & bit) ||
                          (p->msi == DSM_MSI_M && p->poseedor == par))) {
            pthread_mutex_unlock(&dir->mutex);
            errno = ENOTCONN;
            return -1;
        }
        if (par == dir->nodo_id) {
            if (p->msi == DSM_MSI_M && p->poseedor == par && p->version == UINT32_MAX) {
                pthread_mutex_unlock(&dir->mutex);
                errno = EOVERFLOW;
                return -1;
            }
            if (proteger(dir, i, PROT_NONE) != 0) {
                pthread_mutex_unlock(&dir->mutex);
                return -1;
            }
            if (p->msi == DSM_MSI_M && p->poseedor == par) {
                p->msi = DSM_MSI_S;
                p->poseedor = DSM_POSEEDOR_NINGUNO;
                p->version++;
            }
            p->mascara_copias &= ~bit;
            pthread_mutex_unlock(&dir->mutex);
            continue;
        }
        dsm_pagina_desc_t pagina = *p;
        pthread_mutex_unlock(&dir->mutex);

        if (pagina.msi == DSM_MSI_M && pagina.poseedor == par) {
            if (pagina.version == UINT32_MAX) {
                errno = EOVERFLOW;
                return -1;
            }
            uint8_t *imagen = malloc(dir->page_size);
            if (imagen == NULL) {
                return -1;
            }
            uint32_t version = pagina.version;
            uint32_t solicitud = dir->nuevo_id(dir->envio_ctx);
            int rc = solicitud == 0 ? -1 : pedir_bajada_plazo(
                dir, par, pagina.gpn, solicitud, dir->nodo_id, DSM_MOTIVO_A_I,
                pagina.version, imagen, &version, &limite);
            if (rc == 0) {
                pthread_mutex_lock(&dir->mutex);
                p = &dir->paginas[i];
                if (p->perdida || p->version != pagina.version ||
                    proteger(dir, i, PROT_NONE) != 0) {
                    rc = -1;
                    p->perdida = 1;
                    errno = EPROTO;
                } else {
                    memcpy(datos_pagina(dir, i), imagen, dir->page_size);
                    if (proteger(dir, i, PROT_READ | PROT_WRITE) != 0) {
                        rc = -1;
                        p->perdida = 1;
                    } else {
                        p->version++;
                        p->msi = DSM_MSI_M;
                        p->poseedor = dir->nodo_id;
                        p->mascara_copias = UINT64_C(1) << dir->nodo_id;
                    }
                }
                pthread_mutex_unlock(&dir->mutex);
            }
            free(imagen);
            if (rc != 0) {
                return -1;
            }
        } else if (pagina.msi == DSM_MSI_S && (pagina.mascara_copias & bit)) {
            uint32_t solicitud = dir->nuevo_id(dir->envio_ctx);
            if (solicitud == 0 ||
                pedir_invalidacion_plazo(dir, par, pagina.gpn, solicitud, dir->nodo_id, &limite) != 0) {
                return -1;
            }
            pthread_mutex_lock(&dir->mutex);
            dir->paginas[i].mascara_copias &= ~bit;
            pthread_mutex_unlock(&dir->mutex);
        }
    }
    return 0;
}

static int cerrar_vinculo(dsm_directorio_t *dir, uint16_t par, uint32_t id)
{
    pthread_mutex_lock(&dir->mutex);
    struct dsm_reserva_dir *reserva = buscar_reserva(dir, id);
    int error = 0;
    if ((id >> 16) != dir->nodo_id || (uint16_t)id == 0 ||
        par >= dir->cantidad_nodos || par == dir->nodo_id) {
        error = EINVAL;
    } else if (reserva == NULL) {
        if (buscar_tumba(dir, id) == NULL && (uint16_t)id >= dir->siguiente_local) {
            error = ENOENT;
        }
    } else {
        uint64_t bit = UINT64_C(1) << par;
        for (size_t i = 0; i < dir->npaginas; i++) {
            const dsm_pagina_desc_t *p = &dir->paginas[i];
            if (p->reserva == id && p->hogar == dir->nodo_id &&
                ((p->mascara_copias & bit) || (p->msi == DSM_MSI_M && p->poseedor == par))) {
                error = EBUSY;
                break;
            }
        }
        if (error == 0) {
            reserva->abiertos &= ~bit;
        }
    }
    pthread_mutex_unlock(&dir->mutex);
    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}

static int atender_copia_cierre(dsm_directorio_t *dir, uint16_t par,
                                const uint8_t *trama, size_t n)
{
    dsm_cabecera_t pedido;
    uint32_t reserva;
    if (dsm_region_cierre_decodificar(trama, n, &pedido, &reserva) != 0 ||
        pedido.tipo != DSM_TIPO_REGION_COPY_CLOSE || pedido.origen != par ||
        pedido.destino != dir->nodo_id) {
        errno = EPROTO;
        return -1;
    }
    int rc = cerrar_copia_serializada(dir, par, reserva);
    dsm_cabecera_t cab = cabecera_de(dir, DSM_TIPO_REGION_COPY_CLOSE_ACK, par,
                                     dir->nuevo_id(dir->envio_ctx));
    cab.banderas = DSM_FLAG_RESPUESTA;
    cab.id_corr = pedido.id_msg;
    cab.estado = rc == 0 ? DSM_ESTADO_OK : estado_errno(errno);
    uint8_t buf[DSM_REGION_CIERRE_BYTES];
    size_t out;
    if (dsm_region_cierre_codificar(buf, sizeof buf, &cab, reserva, &out) != 0) {
        errno = EPROTO;
        return -1;
    }
    return enviar_fuera(dir, par, buf, out);
}

static int atender_cierre(dsm_directorio_t *dir, uint16_t par,
                          const uint8_t *trama, size_t n)
{
    dsm_cabecera_t pedido;
    uint32_t reserva;
    if (dsm_region_cierre_decodificar(trama, n, &pedido, &reserva) != 0 ||
        pedido.tipo != DSM_TIPO_REGION_CLOSE || pedido.origen != par ||
        pedido.destino != dir->nodo_id) {
        errno = EPROTO;
        return -1;
    }
    int rc = cerrar_vinculo(dir, par, reserva);
    uint16_t estado = rc == 0 ? DSM_ESTADO_OK : estado_errno(errno);
    dsm_cabecera_t cab = cabecera_de(dir, DSM_TIPO_REGION_CLOSE_ACK, par,
                                     dir->nuevo_id(dir->envio_ctx));
    cab.banderas = DSM_FLAG_RESPUESTA;
    cab.id_corr = pedido.id_msg;
    cab.estado = estado;
    uint8_t buf[DSM_REGION_CIERRE_BYTES];
    size_t out;
    if (dsm_region_cierre_codificar(buf, sizeof buf, &cab, reserva, &out) != 0) {
        errno = EPROTO;
        return -1;
    }
    return enviar_fuera(dir, par, buf, out);
}

static int atender_consulta(dsm_directorio_t *dir, uint16_t par,
                            const uint8_t *trama, size_t n)
{
    dsm_cabecera_t pedido;
    dsm_region_lookup_t consulta;
    uint8_t respuesta[DSM_REGION_INFO_MAX];
    size_t out;
    if (dsm_region_lookup_decodificar(trama, n, &pedido, &consulta) != 0 ||
        pedido.origen != par ||
        dsm_directorio_codificar_info(dir, &pedido, &consulta, dir->nuevo_id(dir->envio_ctx),
                                      respuesta, sizeof respuesta, &out) != 0) {
        errno = EPROTO;
        return -1;
    }
    return enviar_fuera(dir, par, respuesta, out);
}

static int publicar_lectura_local(dsm_directorio_t *dir, uint16_t par, const dsm_cabecera_t *pedido_cab,
                                  const dsm_page_request_t *pedido)
{
    printf("nodo %u PAGE_REQUEST recibido par %u gpn %" PRIu64 " bytes %u id %" PRIu32 "\n",
           dir->nodo_id, par, pedido->gpn, pedido_cab->longitud, pedido_cab->id_msg);
    dsm_page_data_t datos;
    int cambio = 0;
    uint8_t *imagen = malloc(dir->page_size);
    if (imagen == NULL) {
        return -1;
    }
    int rc = dsm_directorio_servir_pagina(dir, par, pedido, &datos, imagen, dir->page_size, &cambio);
    if (rc != 0) {
        int error = errno;
        free(imagen);
        return enviar_pagina(dir, par, pedido_cab->id_msg, estado_errno(error), NULL) == 0 ? 0 : -1;
    }
    if (cambio) {
        printf("nodo %u reserva %" PRIu32 " gpn %" PRIu64 " MSI M -> S version %" PRIu32 "\n",
               dir->nodo_id, datos.reserva_id, datos.gpn, datos.version);
    }
    rc = enviar_pagina(dir, par, pedido_cab->id_msg, DSM_ESTADO_OK, &datos);
    free(imagen);
    return rc;
}

static int instalar_bajada(dsm_directorio_t *dir, uint64_t gpn, uint16_t solicitante, uint16_t poseedor,
                           const uint8_t *imagen, uint32_t version)
{
    pthread_mutex_lock(&dir->mutex);
    size_t i = indice_gpn(dir, gpn);
    int rc = -1;
    if (i < dir->npaginas && !dir->paginas[i].perdida && version > 0 &&
        dir->paginas[i].poseedor == poseedor && dir->paginas[i].version == version - 1u) {
        memcpy(datos_pagina(dir, i), imagen, dir->page_size);
        if (proteger(dir, i, PROT_READ) != 0) {
            dir->paginas[i].perdida = 1;
            pthread_mutex_unlock(&dir->mutex);
            return -1;
        }
        dsm_pagina_desc_t *pagina = &dir->paginas[i];
        pagina->msi = DSM_MSI_S;
        pagina->poseedor = DSM_POSEEDOR_NINGUNO;
        pagina->version = version;
        pagina->mascara_copias = (UINT64_C(1) << dir->nodo_id) | (UINT64_C(1) << poseedor) |
                                 (UINT64_C(1) << solicitante);
        pagina->perdida = 0;
        printf("nodo %u reserva %" PRIu32 " gpn %" PRIu64 " MSI M -> S version %" PRIu32 "\n",
               dir->nodo_id, pagina->reserva, pagina->gpn, pagina->version);
        rc = 0;
    } else {
        errno = EPROTO;
    }
    pthread_mutex_unlock(&dir->mutex);
    return rc;
}

static int atender_lectura(dsm_directorio_t *dir, uint16_t par, const dsm_cabecera_t *pedido_cab,
                           const dsm_page_request_t *pedido)
{
    int modo = 0;
    uint16_t poseedor = 0;
    uint32_t version = 0;
    pthread_mutex_lock(&dir->mutex);
    size_t i = indice_gpn(dir, pedido->gpn);
    if (hogar_del_gpn(dir, pedido->gpn) != dir->nodo_id) {
        modo = DSM_ESTADO_NO_HOGAR;
    } else if (i == dir->npaginas) {
        modo = DSM_ESTADO_RANGO;
    } else if (dir->paginas[i].perdida) {
        modo = DSM_ESTADO_FUERA;
    } else if (dir->paginas[i].msi == DSM_MSI_M && dir->paginas[i].poseedor == dir->nodo_id) {
        modo = 0;
    } else if (dir->paginas[i].msi == DSM_MSI_S) {
        modo = 0;
    } else if (dir->paginas[i].msi == DSM_MSI_M && dir->paginas[i].poseedor != dir->nodo_id) {
        modo = 1;
        poseedor = dir->paginas[i].poseedor;
        version = dir->paginas[i].version;
        if (version == UINT32_MAX) {
            modo = DSM_ESTADO_RANGO;
        } else if ((dir->fuera & (1u << poseedor)) != 0) {
            modo = DSM_ESTADO_FUERA;
        }
    } else {
        modo = DSM_ESTADO_NO_POSEEDOR;
    }
    pthread_mutex_unlock(&dir->mutex);

    if (modo == DSM_ESTADO_FUERA || modo == DSM_ESTADO_NO_HOGAR || modo == DSM_ESTADO_RANGO ||
        modo == DSM_ESTADO_NO_POSEEDOR) {
        return enviar_pagina(dir, par, pedido_cab->id_msg, (uint16_t)modo, NULL);
    }
    if (modo == 0) {
        return publicar_lectura_local(dir, par, pedido_cab, pedido);
    }

    printf("nodo %u PAGE_REQUEST recibido par %u gpn %" PRIu64 " bytes %u id %" PRIu32 "\n",
           dir->nodo_id, par, pedido->gpn, pedido_cab->longitud, pedido_cab->id_msg);
    uint8_t *imagen = malloc(dir->page_size);
    uint32_t version_nueva = 0;
    if (imagen == NULL) {
        return -1;
    }
    int rc = pedir_bajada(dir, poseedor, pedido->gpn, pedido_cab->id_msg, par, DSM_MOTIVO_A_S,
                          version + 1u, imagen, &version_nueva);
    if (rc == 0) {
        rc = instalar_bajada(dir, pedido->gpn, par, poseedor, imagen, version_nueva);
    }
    if (rc == 0) {
        dsm_page_data_t datos;
        int cambio;
        rc = dsm_directorio_servir_pagina(dir, par, pedido, &datos, imagen, dir->page_size, &cambio);
        if (rc == 0) {
            rc = enviar_pagina(dir, par, pedido_cab->id_msg, DSM_ESTADO_OK, &datos);
        }
    } else {
        enviar_pagina(dir, par, pedido_cab->id_msg, estado_errno(errno), NULL);
    }
    free(imagen);
    return rc == 0 ? 0 : -1;
}

static int conceder_escritura(dsm_directorio_t *dir, uint16_t par, const dsm_cabecera_t *pedido_cab,
                              const dsm_page_request_t *pedido)
{
    printf("nodo %u PAGE_REQUEST escritura par %u gpn %" PRIu64 " id %" PRIu32 "\n",
           dir->nodo_id, par, pedido->gpn, pedido_cab->id_msg);

    uint16_t estado = DSM_ESTADO_OK;
    dsm_pagina_desc_t inicial;
    if (dsm_directorio_describir(dir, pedido->gpn, &inicial) != 0) {
        estado = DSM_ESTADO_RANGO;
    } else if (inicial.hogar != dir->nodo_id) {
        estado = DSM_ESTADO_NO_HOGAR;
    } else if (inicial.perdida) {
        estado = DSM_ESTADO_FUERA;
    } else if (inicial.version == UINT32_MAX) {
        estado = DSM_ESTADO_RANGO;
    } else if (inicial.msi == DSM_MSI_M && inicial.poseedor == par) {
        /* Una copia que ya recibio M puede haber escrito. El hogar no
         * dispone de una imagen para repetir esa concesion. */
        estado = DSM_ESTADO_CONFLICTO;
    }
    if (estado != DSM_ESTADO_OK) {
        return enviar_pagina(dir, par, pedido_cab->id_msg, estado, NULL);
    }

    uint8_t *imagen = malloc(dir->page_size);
    if (imagen == NULL) {
        return enviar_pagina(dir, par, pedido_cab->id_msg, DSM_ESTADO_COLA, NULL);
    }
    int recuperar = inicial.msi == DSM_MSI_M && inicial.poseedor != dir->nodo_id;
    uint32_t version = inicial.version;
    dsm_page_data_t datos;
    int rc;
    if (recuperar && pedir_bajada(dir, inicial.poseedor, inicial.gpn, pedido_cab->id_msg,
                                  par, DSM_MOTIVO_A_I, version, imagen, &version) != 0) {
        estado = estado_errno(errno);
        goto responder;
    }

    /* Un nodo desconectado aun puede leer su mapeo. Su bit solo se retira
     * despues de un ACK, tambien cuando una campana queda a medias. */
    for (uint16_t n = 0; inicial.msi == DSM_MSI_S && n < dir->cantidad_nodos; n++) {
        if (n == par || n == dir->nodo_id || !bit_de(inicial.mascara_copias, n)) {
            continue;
        }
        if (pedir_invalidacion(dir, n, inicial.gpn, pedido_cab->id_msg, par) != 0) {
            estado = estado_errno(errno);
            goto responder;
        }
        pthread_mutex_lock(&dir->mutex);
        size_t i = indice_gpn(dir, inicial.gpn);
        dir->paginas[i].mascara_copias &= ~(UINT64_C(1) << n);
        pthread_mutex_unlock(&dir->mutex);
    }

    pthread_mutex_lock(&dir->mutex);
    size_t i = indice_gpn(dir, inicial.gpn);
    dsm_pagina_desc_t *p = &dir->paginas[i];
    int con_carga = !(pedido->msi == DSM_MSI_S && inicial.msi == DSM_MSI_S &&
                      bit_de(p->mascara_copias, par));
    if (p->version != inicial.version || p->perdida ||
        (par != dir->nodo_id && (dir->fuera & (1u << par)))) {
        estado = DSM_ESTADO_FUERA;
    } else if (proteger(dir, i, PROT_NONE) != 0) {
        estado = DSM_ESTADO_CONFLICTO;
    } else {
        /* Un escritor previo ya perdio acceso. La instantanea incluye hasta
         * su ultima escritura, incluso cuando ese escritor era el hogar. */
        if (recuperar) {
            memcpy(datos_pagina(dir, i), imagen, dir->page_size);
        } else if (con_carga) {
            memcpy(imagen, datos_pagina(dir, i), dir->page_size);
        }
        if (par == dir->nodo_id && proteger(dir, i, PROT_READ | PROT_WRITE) != 0) {
            p->perdida = 1;
            estado = DSM_ESTADO_CONFLICTO;
        } else {
            p->version++;
            p->msi = DSM_MSI_M;
            p->poseedor = par;
            p->mascara_copias = UINT64_C(1) << par;
            datos = (dsm_page_data_t){
                .gpn = p->gpn, .proc_id = pedido->proc_id, .reserva_id = p->reserva,
                .orden = p->orden, .hogar = p->hogar, .poseedor = par, .msi = DSM_MSI_M,
                .permiso = DSM_PAGE_PERMISO_ESCRIBIR, .version = p->version,
                .mascara_copias = p->mascara_copias,
                .banderas = con_carga ? 0 : DSM_PAGE_DATA_SIN_CARGA,
                .page_nbytes = con_carga ? dir->page_size : 0, .datos = imagen,
            };
            printf("nodo %u reserva %" PRIu32 " gpn %" PRIu64 " MSI -> M poseedor %u version %" PRIu32 "\n",
                   dir->nodo_id, p->reserva, p->gpn, par, p->version);
        }
    }
    if (recuperar && estado != DSM_ESTADO_OK) {
        /* El poseedor anterior ya confirmo M -> I. */
        p->perdida = 1;
    }
    pthread_mutex_unlock(&dir->mutex);

responder:
    rc = enviar_pagina(dir, par, pedido_cab->id_msg, estado,
                       estado == DSM_ESTADO_OK ? &datos : NULL);
    if (rc != 0 && estado == DSM_ESTADO_OK) {
        /* Una respuesta parcialmente transmitida es ambigua. */
        pthread_mutex_lock(&dir->mutex);
        size_t indice = indice_gpn(dir, inicial.gpn);
        dir->paginas[indice].perdida = 1;
        if (par == dir->nodo_id && proteger(dir, indice, PROT_NONE) != 0) {
            abort();
        }
        pthread_mutex_unlock(&dir->mutex);
    }
    free(imagen);
    return rc;
}

static int atender_pedido(dsm_directorio_t *dir, uint16_t par, const uint8_t *trama, size_t n)
{
    dsm_cabecera_t cab;
    dsm_page_request_t pedido;
    if (dsm_page_request_decodificar(trama, n, &cab, &pedido) != 0 ||
        cab.origen != par || (pedido.proc_id >> 16) != par || (uint16_t)pedido.proc_id == 0) {
        errno = EPROTO;
        return -1;
    }
    if (pedido.reserva_id != 0) {
        dsm_pagina_desc_t pagina;
        if (dsm_directorio_describir(dir, pedido.gpn, &pagina) != 0 ||
            pagina.reserva != pedido.reserva_id || pagina.orden != pedido.orden) {
            return enviar_pagina(dir, par, cab.id_msg, DSM_ESTADO_RANGO, NULL);
        }
    }
    pthread_mutex_lock(&dir->mutex);
    size_t anotada = indice_gpn(dir, pedido.gpn);
    if (anotada < dir->npaginas) {
        dir->paginas[anotada].proc = pedido.proc_id;
    }
    pthread_mutex_unlock(&dir->mutex);
    if (pedido.acceso == DSM_ACCESO_ESCRITURA) {
        return conceder_escritura(dir, par, &cab, &pedido);
    }
    return atender_lectura(dir, par, &cab, &pedido);
}

static int atender_stats(dsm_directorio_t *dir, uint16_t par, const uint8_t *trama, size_t n);

static int procesar_item(dsm_directorio_t *dir, uint16_t par, const uint8_t *trama, size_t n)
{
    dsm_cabecera_t cab;
    if (dsm_cabecera_decodificar(trama, n < DSM_CABECERA_BYTES ? n : DSM_CABECERA_BYTES, &cab) != 0) {
        errno = EPROTO;
        return -1;
    }
    if (cab.tipo == DSM_TIPO_REGION_LOOKUP) {
        return atender_consulta(dir, par, trama, n);
    }
    if (cab.tipo == DSM_TIPO_REGION_CLOSE) {
        return atender_cierre(dir, par, trama, n);
    }
    if (cab.tipo == DSM_TIPO_REGION_COPY_CLOSE) {
        return atender_copia_cierre(dir, par, trama, n);
    }
    if (cab.tipo == DSM_TIPO_PAGE_REQUEST) {
        return atender_pedido(dir, par, trama, n);
    }
    if (cab.tipo == DSM_TIPO_INVALIDATE) {
        return atender_invalidate(dir, par, trama, n);
    }
    if (cab.tipo == DSM_TIPO_PAGE_DOWNGRADE) {
        return atender_downgrade(dir, par, trama, n);
    }
    if (cab.tipo == DSM_TIPO_STATS_REQUEST) {
        return atender_stats(dir, par, trama, n);
    }
    errno = EPROTO;
    return -1;
}

static void *servicio_coherencia(void *arg)
{
    struct dsm_cola *cola = arg;
    dsm_directorio_t *dir = cola->directorio;
    for (;;) {
        pthread_mutex_lock(&dir->mutex);
        while (!dir->servicio_parar && cola->cantidad == 0) {
            pthread_cond_wait(&dir->cola_cv, &dir->mutex);
        }
        if (dir->servicio_parar) {
            pthread_mutex_unlock(&dir->mutex);
            return NULL;
        }
        struct dsm_item item = cola->items[cola->inicio];
        cola->inicio = (cola->inicio + 1u) % DSM_DIR_COLA;
        cola->cantidad--;
        pthread_mutex_unlock(&dir->mutex);
        if (cola == &dir->hogar) {
            pthread_mutex_lock(&dir->coordinador);
        }
        int rc = procesar_item(dir, item.par, item.trama, item.n);
        if (cola == &dir->hogar) {
            pthread_mutex_unlock(&dir->coordinador);
        }
        if (rc != 0) {
            fprintf(stderr, "nodo %u coherencia: %s\n", dir->nodo_id, strerror(errno));
        }
    }
}

int dsm_directorio_activar(dsm_directorio_t *dir, dsm_directorio_envio_t enviar,
                           dsm_directorio_id_t nuevo_id, void *ctx)
{
    if (dir == NULL || !dir->inicializado || enviar == NULL || nuevo_id == NULL) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&dir->mutex);
    if (dir->servicio_activo) {
        pthread_mutex_unlock(&dir->mutex);
        errno = EALREADY;
        return -1;
    }
    dir->campana_cap = (size_t)dir->page_size + DSM_PAGE_DATA_BASE;
    dir->campana_buf = malloc(dir->campana_cap);
    if (dir->campana_buf == NULL) {
        pthread_mutex_unlock(&dir->mutex);
        return -1;
    }
    dir->enviar = enviar;
    dir->nuevo_id = nuevo_id;
    dir->envio_ctx = ctx;
    dir->servicio_parar = 0;
    dir->hogar = (struct dsm_cola){.directorio = dir};
    dir->replica = (struct dsm_cola){.directorio = dir};
    dir->campana_espera = 0;
    int error = pthread_create(&dir->hogar.hilo, NULL, servicio_coherencia, &dir->hogar);
    if (error == 0) {
        error = pthread_create(&dir->replica.hilo, NULL, servicio_coherencia, &dir->replica);
        if (error != 0) {
            dir->servicio_parar = 1;
            pthread_cond_broadcast(&dir->cola_cv);
            pthread_mutex_unlock(&dir->mutex);
            pthread_join(dir->hogar.hilo, NULL);
            pthread_mutex_lock(&dir->mutex);
        }
    }
    if (error != 0) {
        free(dir->campana_buf);
        dir->campana_buf = NULL;
        pthread_mutex_unlock(&dir->mutex);
        errno = error;
        return -1;
    }
    dir->servicio_activo = 1;
    pthread_mutex_unlock(&dir->mutex);
    return 0;
}

void dsm_directorio_desactivar(dsm_directorio_t *dir)
{
    if (dir == NULL || !dir->inicializado || !dir->servicio_activo) {
        return;
    }
    pthread_mutex_lock(&dir->mutex);
    dir->servicio_parar = 1;
    if (dir->campana_error == 0) {
        dir->campana_error = ECANCELED;
    }
    pthread_cond_broadcast(&dir->cola_cv);
    pthread_cond_broadcast(&dir->campana_cv);
    pthread_mutex_unlock(&dir->mutex);
    pthread_join(dir->hogar.hilo, NULL);
    pthread_join(dir->replica.hilo, NULL);
    pthread_mutex_lock(&dir->mutex);
    dir->servicio_activo = 0;
    dir->servicio_parar = 0;
    dir->campana_espera = 0;
    free(dir->campana_buf);
    dir->campana_buf = NULL;
    pthread_mutex_unlock(&dir->mutex);
}

static int rechazar_por_cola(dsm_directorio_t *dir, uint16_t par,
                             const dsm_cabecera_t *pedido, const uint8_t *trama, size_t n)
{
    if (pedido->tipo == DSM_TIPO_PAGE_REQUEST) {
        return enviar_pagina(dir, par, pedido->id_msg, DSM_ESTADO_COLA, NULL);
    }
    if (pedido->tipo != DSM_TIPO_REGION_LOOKUP && pedido->tipo != DSM_TIPO_REGION_CLOSE &&
        pedido->tipo != DSM_TIPO_REGION_COPY_CLOSE) {
        errno = EAGAIN;
        return -1;
    }
    dsm_cabecera_t cab = cabecera_de(dir, pedido->tipo == DSM_TIPO_REGION_LOOKUP ?
                                     DSM_TIPO_REGION_INFO : pedido->tipo == DSM_TIPO_REGION_CLOSE ?
                                     DSM_TIPO_REGION_CLOSE_ACK : DSM_TIPO_REGION_COPY_CLOSE_ACK,
                                     par, dir->nuevo_id(dir->envio_ctx));
    cab.banderas = DSM_FLAG_RESPUESTA;
    cab.id_corr = pedido->id_msg;
    cab.estado = DSM_ESTADO_COLA;
    uint8_t respuesta[DSM_REGION_CIERRE_BYTES];
    size_t bytes = DSM_CABECERA_BYTES;
    if (pedido->tipo == DSM_TIPO_REGION_CLOSE || pedido->tipo == DSM_TIPO_REGION_COPY_CLOSE) {
        dsm_cabecera_t copia;
        uint32_t reserva;
        if (dsm_region_cierre_decodificar(trama, n, &copia, &reserva) != 0 ||
            dsm_region_cierre_codificar(respuesta, sizeof respuesta, &cab, reserva, &bytes) != 0) {
            errno = EPROTO;
            return -1;
        }
    } else {
        cab.longitud = DSM_CABECERA_BYTES;
        if (dsm_cabecera_codificar(respuesta, &cab) != 0) {
            errno = EPROTO;
            return -1;
        }
    }
    return enviar_fuera(dir, par, respuesta, bytes);
}

int dsm_directorio_ofrecer(dsm_directorio_t *dir, uint16_t par, const uint8_t *trama, size_t n)
{
    if (dir == NULL || !dir->inicializado || trama == NULL ||
        n < DSM_CABECERA_BYTES || n > sizeof dir->hogar.items[0].trama) {
        errno = EINVAL;
        return -1;
    }
    dsm_cabecera_t cab;
    if (dsm_cabecera_decodificar(trama, DSM_CABECERA_BYTES, &cab) != 0 || cab.origen != par ||
        cab.destino != dir->nodo_id) {
        errno = EPROTO;
        return -1;
    }
    if (cab.tipo != DSM_TIPO_PAGE_REQUEST && cab.tipo != DSM_TIPO_INVALIDATE &&
        cab.tipo != DSM_TIPO_PAGE_DOWNGRADE && cab.tipo != DSM_TIPO_REGION_LOOKUP &&
        cab.tipo != DSM_TIPO_REGION_CLOSE && cab.tipo != DSM_TIPO_REGION_COPY_CLOSE &&
        cab.tipo != DSM_TIPO_STATS_REQUEST) {
        errno = EPROTO;
        return -1;
    }
    pthread_mutex_lock(&dir->mutex);
    if (!dir->servicio_activo || dir->servicio_parar) {
        pthread_mutex_unlock(&dir->mutex);
        errno = ECANCELED;
        return -1;
    }
    /* Las revocaciones entrantes avanzan aunque el coordinador de este nodo
     * espere un ACK de otro hogar. El hogar emisor espera nuestra confirmacion
     * antes de emitir una concesion incompatible. */
    struct dsm_cola *cola = &dir->hogar;
    if (cab.tipo == DSM_TIPO_PAGE_DOWNGRADE || cab.tipo == DSM_TIPO_INVALIDATE) {
        uint64_t gpn;
        if (cab.tipo == DSM_TIPO_PAGE_DOWNGRADE) {
            dsm_downgrade_t msg;
            if (dsm_downgrade_decodificar(trama, n, &cab, &msg) != 0) {
                pthread_mutex_unlock(&dir->mutex);
                errno = EPROTO;
                return -1;
            }
            gpn = msg.gpn;
        } else {
            dsm_invalidate_t msg;
            if (dsm_invalidate_decodificar(trama, n, &cab, &msg) != 0) {
                pthread_mutex_unlock(&dir->mutex);
                errno = EPROTO;
                return -1;
            }
            gpn = msg.gpn;
        }
        if (hogar_del_gpn(dir, gpn) == par) {
            cola = &dir->replica;
        } else if (cab.tipo != DSM_TIPO_INVALIDATE || hogar_del_gpn(dir, gpn) != dir->nodo_id) {
            pthread_mutex_unlock(&dir->mutex);
            errno = EPROTO;
            return -1;
        }
    }
    if (cola->cantidad == DSM_DIR_COLA) {
        pthread_mutex_unlock(&dir->mutex);
        return rechazar_por_cola(dir, par, &cab, trama, n);
    }
    size_t slot = (cola->inicio + cola->cantidad) % DSM_DIR_COLA;
    cola->items[slot].par = par;
    cola->items[slot].n = (uint16_t)n;
    memcpy(cola->items[slot].trama, trama, n);
    cola->cantidad++;
    pthread_cond_broadcast(&dir->cola_cv);
    pthread_mutex_unlock(&dir->mutex);
    return 0;
}

int dsm_directorio_completar(dsm_directorio_t *dir, uint16_t par, const uint8_t *trama, size_t n)
{
    dsm_cabecera_t cab;
    if (dir == NULL || !dir->inicializado || trama == NULL ||
        dsm_cabecera_decodificar(trama, n, &cab) != 0 || cab.banderas != DSM_FLAG_RESPUESTA ||
        cab.longitud != n ||
        cab.id_corr == 0 || cab.origen != par || cab.destino != dir->nodo_id) {
        return -1;
    }
    pthread_mutex_lock(&dir->mutex);
    if (!dir->campana_espera || par != dir->campana_par || cab.id_corr != dir->campana_id) {
        pthread_mutex_unlock(&dir->mutex);
        return -1;
    }
    if (cab.tipo == DSM_TIPO_PAGE_DATA) {
        if (n > dir->campana_cap) {
            dir->campana_error = EPROTO;
        } else if (!dir->tiene_datos) {
            memcpy(dir->campana_buf, trama, n);
            dir->datos_n = n;
            dir->tiene_datos = 1;
        }
    } else if (cab.tipo == DSM_TIPO_PAGE_DOWNGRADE_ACK || cab.tipo == DSM_TIPO_INVALIDATE_ACK) {
        if (n != sizeof dir->ack_trama || cab.estado != DSM_ESTADO_OK) {
            dir->campana_error = EPROTO;
        } else if (cab.tipo == DSM_TIPO_PAGE_DOWNGRADE_ACK) {
            dsm_downgrade_ack_t ack;
            if (dsm_downgrade_ack_decodificar(trama, n, &cab, &ack) != 0 ||
                ack.resultado != DSM_DOWNGRADE_HECHO) {
                dir->campana_error = EPROTO;
            }
        }
        if (!dir->campana_error && !dir->tiene_ack) {
            memset(dir->ack_trama, 0, sizeof dir->ack_trama);
            memcpy(dir->ack_trama, trama, n);
            dir->tiene_ack = 1;
        }
    } else {
        pthread_mutex_unlock(&dir->mutex);
        return -1;
    }
    pthread_cond_signal(&dir->campana_cv);
    pthread_mutex_unlock(&dir->mutex);
    return 0;
}

void dsm_directorio_par_fuera(dsm_directorio_t *dir, uint16_t par)
{
    if (dir == NULL || !dir->inicializado || par >= dir->cantidad_nodos || par == dir->nodo_id) {
        return;
    }
    pthread_mutex_lock(&dir->mutex);
    dir->fuera |= 1u << par;
    for (size_t i = 0; i < dir->npaginas; i++) {
        dsm_pagina_desc_t *pagina = &dir->paginas[i];
        if (pagina->hogar == par) {
            /* No hay epocas persistentes. Tras perder al hogar, esta reserva
             * queda aislada incluso si reaparece otro proceso con el mismo ID. */
            if (proteger(dir, i, PROT_NONE) != 0) {
                abort();
            }
            pagina->perdida = 1;
            pagina->msi = DSM_MSI_I;
            pagina->mascara_copias = 0;
            pagina->poseedor = DSM_POSEEDOR_NINGUNO;
        }
        if (pagina->hogar != dir->nodo_id) {
            continue;
        }
        /* La desconexion no demuestra que la copia ya no sea legible. */
        if (pagina->poseedor == par && pagina->msi == DSM_MSI_M) {
            pagina->perdida = 1;
        }
    }
    if (dir->campana_espera && dir->campana_par == par && dir->campana_error == 0) {
        dir->campana_error = ENOTCONN;
        pthread_cond_signal(&dir->campana_cv);
    }
    pthread_mutex_unlock(&dir->mutex);
}

void dsm_directorio_par_linea(dsm_directorio_t *dir, uint16_t par)
{
    if (dir == NULL || !dir->inicializado || par >= 32) {
        return;
    }
    pthread_mutex_lock(&dir->mutex);
    dir->fuera &= ~(1u << par);
    pthread_mutex_unlock(&dir->mutex);
}

void dsm_directorio_fijar_retardo(dsm_directorio_t *dir, uint32_t ms)
{
    if (dir == NULL || !dir->inicializado) {
        return;
    }
    pthread_mutex_lock(&dir->mutex);
    dir->retardo_ms = ms;
    pthread_mutex_unlock(&dir->mutex);
}

int dsm_directorio_aplicar_pagina(dsm_directorio_t *dir, const dsm_page_data_t *datos)
{
    if (datos == NULL || (datos->msi != DSM_MSI_S && datos->msi != DSM_MSI_M) || !bloquear(dir)) {
        errno = EINVAL;
        return -1;
    }
    int resultado = -1;
    size_t i = indice_gpn(dir, datos->gpn);
    if (i == dir->npaginas) {
        errno = ENOENT;
        goto salir;
    }
    dsm_pagina_desc_t *pagina = &dir->paginas[i];
    if (datos->reserva_id != pagina->reserva || datos->hogar != pagina->hogar ||
        datos->orden != pagina->orden || datos->gpn != pagina->gpn) {
        errno = EPROTO;
        goto salir;
    }
    uint64_t bit = UINT64_C(1) << dir->nodo_id;
    uint64_t admitidos = (UINT64_C(1) << dir->cantidad_nodos) - 1;
    int escritura = datos->msi == DSM_MSI_M;
    int sin_carga = datos->page_nbytes == 0;
    if (pagina->perdida || pagina->hogar == dir->nodo_id ||
        datos->version < pagina->version || !(datos->mascara_copias & bit) ||
        (datos->mascara_copias & ~admitidos) ||
        datos->poseedor != (escritura ? dir->nodo_id : DSM_POSEEDOR_NINGUNO) ||
        datos->permiso != (escritura ? DSM_PAGE_PERMISO_ESCRIBIR : DSM_PAGE_PERMISO_LEER) ||
        (escritura && (datos->mascara_copias != bit || datos->version <= pagina->version)) ||
        pagina->msi == DSM_MSI_M) {
        errno = EPROTO;
        goto salir;
    }

    if (sin_carga) {
        if (!escritura || datos->banderas != DSM_PAGE_DATA_SIN_CARGA ||
            pagina->msi != DSM_MSI_S || datos->version != pagina->version + 1u) {
            errno = EPROTO;
            goto salir;
        }
    } else {
        if (datos->page_nbytes != dir->page_size || datos->datos == NULL || datos->banderas != 0) {
            errno = EPROTO;
            goto salir;
        }
        if (pagina->msi == DSM_MSI_I) {
            /* Ar sigue en PROT_NONE hasta instalar toda la imagen. */
            memcpy(datos_pagina(dir, i), datos->datos, dir->page_size);
        } else if (memcmp(datos_pagina(dir, i), datos->datos, dir->page_size) != 0) {
            errno = EPROTO;
            goto salir;
        }
    }
    if (proteger(dir, i, escritura ? PROT_READ | PROT_WRITE : PROT_READ) != 0) {
        goto salir;
    }
    pagina->msi = datos->msi;
    pagina->poseedor = datos->poseedor;
    pagina->version = datos->version;
    pagina->mascara_copias = datos->mascara_copias;
    resultado = 0;

salir:
    pthread_mutex_unlock(&dir->mutex);
    return resultado;
}

static int copiar_serializado(dsm_directorio_t *dir, uint64_t gpn, void *imagen,
                              dsm_pagina_desc_t *desc)
{
    pthread_mutex_lock(&dir->mutex);
    size_t i = indice_gpn(dir, gpn);
    if (i == dir->npaginas) {
        pthread_mutex_unlock(&dir->mutex);
        errno = ENOENT;
        return -1;
    }
    dsm_pagina_desc_t pagina = dir->paginas[i];
    int local = !pagina.perdida &&
                ((pagina.msi == DSM_MSI_S && bit_de(pagina.mascara_copias, dir->nodo_id)) ||
                 (pagina.msi == DSM_MSI_M && pagina.poseedor == dir->nodo_id));
    if (local) {
        int escritor = pagina.msi == DSM_MSI_M && pagina.poseedor == dir->nodo_id;
        if (escritor && proteger(dir, i, PROT_READ) != 0) {
            pthread_mutex_unlock(&dir->mutex);
            return -1;
        }
        memcpy(imagen, datos_pagina(dir, i), dir->page_size);
        if (escritor && proteger(dir, i, PROT_READ | PROT_WRITE) != 0) {
            dir->paginas[i].perdida = 1;
            pthread_mutex_unlock(&dir->mutex);
            return -1;
        }
        pthread_mutex_unlock(&dir->mutex);
        if (desc != NULL) {
            *desc = pagina;
        }
        return 0;
    }
    int remota = pagina.msi == DSM_MSI_M && pagina.poseedor != dir->nodo_id &&
                 pagina.poseedor != DSM_POSEEDOR_NINGUNO && !pagina.perdida;
    uint16_t poseedor = pagina.poseedor;
    uint32_t version = pagina.version;
    pthread_mutex_unlock(&dir->mutex);
    if (!remota || dir->nuevo_id == NULL || version == UINT32_MAX) {
        errno = pagina.perdida ? ENOTCONN : EPROTO;
        return -1;
    }
    uint8_t *tmp = malloc(dir->page_size);
    if (tmp == NULL) {
        return -1;
    }
    uint32_t version_nueva = 0;
    uint32_t id_solicitud = dir->nuevo_id(dir->envio_ctx);
    int rc = id_solicitud == 0 ? -1
                              : pedir_bajada(dir, poseedor, gpn, id_solicitud, dir->nodo_id,
                                             DSM_MOTIVO_A_S, version + 1u, tmp, &version_nueva);
    if (rc == 0) {
        rc = instalar_bajada(dir, gpn, dir->nodo_id, poseedor, tmp, version_nueva);
    }
    if (rc == 0) {
        memcpy(imagen, tmp, dir->page_size);
        if (desc != NULL) {
            rc = dsm_directorio_describir(dir, gpn, desc);
        }
    }
    free(tmp);
    return rc;
}

int dsm_directorio_copiar(dsm_directorio_t *dir, uint64_t gpn, void *imagen, size_t n,
                          dsm_pagina_desc_t *desc)
{
    if (dir == NULL || !dir->inicializado || imagen == NULL || n < dir->page_size) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&dir->coordinador);
    int rc = copiar_serializado(dir, gpn, imagen, desc);
    pthread_mutex_unlock(&dir->coordinador);
    return rc;
}

int dsm_directorio_describir(dsm_directorio_t *dir, uint64_t gpn, dsm_pagina_desc_t *desc)
{
    if (desc == NULL || !bloquear(dir)) {
        errno = EINVAL;
        return -1;
    }
    size_t i = indice_gpn(dir, gpn);
    if (i == dir->npaginas) {
        pthread_mutex_unlock(&dir->mutex);
        errno = ENOENT;
        return -1;
    }
    *desc = dir->paginas[i];
    pthread_mutex_unlock(&dir->mutex);
    return 0;
}

int dsm_directorio_copiar_controlado(dsm_directorio_t *dir, uint32_t reserva, uint64_t gpn,
                                     size_t offset, void *scratch, size_t bytes, int escritura)
{
    if (scratch == NULL || (escritura != 0 && escritura != 1) || !bloquear(dir)) {
        errno = EINVAL;
        return -1;
    }
    int error = 0;
    size_t i = indice_gpn(dir, gpn);
    if (i == dir->npaginas || dir->paginas[i].reserva != reserva) {
        error = ENOENT;
    } else {
        const dsm_pagina_desc_t *pagina = &dir->paginas[i];
        const struct dsm_reserva_dir *region = buscar_reserva(dir, reserva);
        uint64_t posicion = (uint64_t)pagina->orden * dir->page_size + offset;
        int copia = bit_de(pagina->mascara_copias, dir->nodo_id);
        int modificada = pagina->msi == DSM_MSI_M && pagina->poseedor == dir->nodo_id;
        if (offset > dir->page_size || bytes > dir->page_size - offset || region == NULL ||
            posicion > region->info.tamano_bytes || bytes > region->info.tamano_bytes - posicion) {
            error = EINVAL;
        } else if (pagina->perdida) {
            error = ENOTCONN;
        } else if (!copia || (escritura ? !modificada : !modificada && pagina->msi != DSM_MSI_S)) {
            error = EAGAIN;
        } else if (bytes != 0) {
            unsigned char *datos = (unsigned char *)datos_pagina(dir, i) + offset;
            if (escritura) {
                memcpy(datos, scratch, bytes);
            } else {
                memcpy(scratch, datos, bytes);
            }
        }
    }
    pthread_mutex_unlock(&dir->mutex);
    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}

static uint8_t copias_de(uint64_t mascara)
{
    uint8_t n = 0;
    while (mascara != 0) {
        n = (uint8_t)(n + (mascara & 1u));
        mascara >>= 1;
    }
    return n;
}

void dsm_directorio_contar(dsm_directorio_t *dir, int indice, uint64_t n)
{
    if (dir == NULL || !dir->inicializado || indice < 0 || indice >= DSM_CONTA_N || n == 0) {
        return;
    }
    atomic_fetch_add_explicit(&dir->contadores[indice], n, memory_order_relaxed);
}

int dsm_directorio_exportar(dsm_directorio_t *dir, uint64_t contadores[DSM_CONTA_N],
                            dsm_pagina_desc_t *paginas, size_t capacidad, size_t *cantidad,
                            uint32_t *libres, uint32_t *totales)
{
    if (contadores == NULL || cantidad == NULL || !bloquear(dir)) {
        errno = EINVAL;
        return -1;
    }
    for (int i = 0; i < DSM_CONTA_N; i++) {
        contadores[i] = atomic_load_explicit(&dir->contadores[i], memory_order_relaxed);
    }
    *cantidad = dir->npaginas;
    if (libres != NULL) {
        *libres = dir->paginas_libres;
    }
    if (totales != NULL) {
        *totales = dir->paginas_totales;
    }
    int rc = 0;
    if (paginas != NULL) {
        if (capacidad < dir->npaginas) {
            errno = ENOMEM;
            rc = -1;
        } else if (dir->npaginas != 0) {
            memcpy(paginas, dir->paginas, dir->npaginas * sizeof *paginas);
        }
    }
    pthread_mutex_unlock(&dir->mutex);
    return rc;
}

static int atender_stats(dsm_directorio_t *dir, uint16_t par, const uint8_t *trama, size_t n)
{
    dsm_cabecera_t pedido;
    dsm_stats_request_t pedido_stats;
    if (dsm_stats_request_decodificar(trama, n, &pedido, &pedido_stats) != 0 || pedido.origen != par) {
        errno = EPROTO;
        return -1;
    }
    uint64_t contadores[DSM_CONTA_N];
    size_t np = 0;
    uint32_t libres = 0;
    uint32_t totales = 0;
    if (dsm_directorio_exportar(dir, contadores, NULL, 0, &np, &libres, &totales) != 0) {
        return -1;
    }
    int con_paginas = pedido_stats.seleccion != DSM_STATS_NODOS;
    int con_nodos = pedido_stats.seleccion != DSM_STATS_PAGINAS;
    dsm_pagina_desc_t *desc = NULL;
    dsm_pagina_consulta_t *consultas = NULL;
    if (con_paginas && np != 0) {
        desc = calloc(np, sizeof *desc);
        consultas = calloc(np, sizeof *consultas);
        size_t n2 = 0;
        if (desc == NULL || consultas == NULL ||
            dsm_directorio_exportar(dir, contadores, desc, np, &n2, &libres, &totales) != 0) {
            free(desc);
            free(consultas);
            return -1;
        }
        np = n2;
        for (size_t i = 0; i < np; i++) {
            consultas[i] = (dsm_pagina_consulta_t){
                .gpn = desc[i].gpn,
                .mascara = desc[i].mascara_copias,
                .proc = desc[i].proc,
                .reserva = desc[i].reserva,
                .orden = desc[i].orden,
                .hogar = desc[i].hogar,
                .poseedor = desc[i].poseedor,
                .msi = desc[i].msi,
                .permiso = desc[i].msi == DSM_MSI_M ? DSM_PAGE_PERMISO_ESCRIBIR :
                           desc[i].msi == DSM_MSI_S ? DSM_PAGE_PERMISO_LEER : 0,
                .copias = copias_de(desc[i].mascara_copias),
            };
        }
    }
    dsm_stats_reply_t msg;
    memset(&msg, 0, sizeof msg);
    msg.emisor = dir->nodo_id;
    msg.n = con_nodos ? 1 : 0;
    msg.p = con_paginas ? (uint32_t)np : 0;
    msg.paginas = consultas;
    memcpy(msg.contadores, contadores, sizeof msg.contadores);
    if (con_nodos) {
        msg.fichas[0].id = dir->local.id;
        msg.fichas[0].estado = DSM_NODO_EN_LINEA;
        msg.fichas[0].ipv4_host = dir->local.ipv4_host;
        msg.fichas[0].puerto = dir->local.puerto;
        msg.fichas[0].memoria = dir->local.memoria;
        msg.fichas[0].base = dir->limites_gpn[dir->nodo_id] * (uint64_t)dir->page_size;
        msg.fichas[0].libres = libres;
        msg.fichas[0].totales = totales;
        memcpy(msg.fichas[0].nombre, dir->local.nombre, DSM_NOMBRE_MAX);
    }
    uint32_t id = dir->nuevo_id == NULL ? 0 : dir->nuevo_id(dir->envio_ctx);
    dsm_cabecera_t cab = cabecera_de(dir, DSM_TIPO_STATS_REPLY, par, id);
    cab.banderas = DSM_FLAG_RESPUESTA;
    cab.id_corr = pedido.id_msg;
    size_t cap = DSM_STATS_PREF + (size_t)msg.n * 104u + (size_t)msg.p * DSM_FICHA_PAGINA;
    uint8_t *buf = malloc(cap);
    size_t out = 0;
    int rc = -1;
    if (buf != NULL && id != 0 &&
        dsm_stats_reply_codificar(buf, cap, &cab, &msg, &out) == 0 &&
        enviar_fuera(dir, par, buf, out) == 0) {
        rc = 0;
    } else {
        errno = EPROTO;
    }
    free(buf);
    free(consultas);
    free(desc);
    return rc;
}
