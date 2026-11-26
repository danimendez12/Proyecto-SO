#include "consola_tareas.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdlib.h>

enum { MAX_HILOS = 64 };

struct tarea {
    dsm_context_t *ctx;
    dsm_proc_id_t proceso;
    dsm_reserva_id_t id;
    size_t bytes, pagina, desde, hasta, procesados;
    uint8_t valor;
    uint64_t suma;
    int ar, resultado;
};

static uint8_t patron(size_t offset, uint8_t valor)
{
    uint64_t n = (uint64_t)offset;
    n ^= n >> 8; n ^= n >> 16; n ^= n >> 32;
    return (uint8_t)n ^ valor;
}

static void *trabajar(void *arg)
{
    struct tarea *t = arg;
    t->resultado = dsm_hilo_registrar(t->ctx, t->proceso);
    if (t->resultado != DSM_OK) return NULL;
    unsigned char *buffer = NULL;
    if (!t->ar) {
        buffer = malloc(t->pagina);
        if (buffer == NULL) t->resultado = DSM_ERR_NOMEM;
    }
    for (size_t p = t->desde; p < t->hasta && t->resultado == DSM_OK; p++) {
        size_t offset = p * t->pagina;
        size_t n = t->bytes - offset < t->pagina ? t->bytes - offset : t->pagina;
        volatile unsigned char *vista = NULL;
        if (t->ar) {
            void *direccion;
            t->resultado = dsm_traducir(t->ctx, t->id, offset, &direccion);
            if (t->resultado != DSM_OK) break;
            vista = direccion;
            for (size_t i = 0; i < n; i++) vista[i] = patron(offset + i, t->valor);
        } else {
            for (size_t i = 0; i < n; i++) buffer[i] = patron(offset + i, t->valor);
            t->resultado = dsm_escribir(t->ctx, t->id, offset, buffer, n);
            if (t->resultado == DSM_OK) t->resultado = dsm_leer(t->ctx, t->id, offset, buffer, n);
        }
        if (t->resultado != DSM_OK) break;
        for (size_t i = 0; i < n; i++) {
            unsigned char actual = t->ar ? vista[i] : buffer[i];
            if (actual != patron(offset + i, t->valor)) { t->resultado = DSM_ERR_ESTADO; break; }
            t->suma += actual;
            t->procesados++;
        }
    }
    free(buffer);
    int otro = dsm_hilo_desregistrar(t->ctx);
    if (t->resultado == DSM_OK) t->resultado = otro;
    return NULL;
}

int consola_tarea_patron(dsm_context_t *ctx, dsm_proc_id_t proceso,
                         dsm_reserva_id_t id, size_t bytes, size_t pagina,
                         size_t hilos, uint8_t valor, int ar, FILE *salida)
{
    if (!ctx || !proceso || !id || !bytes || !pagina || !hilos || hilos > MAX_HILOS ||
        bytes > UINT64_MAX / 255u || (ar != 0 && ar != 1) || !salida) return DSM_ERR_INVAL;
    size_t paginas = 1 + (bytes - 1) / pagina;
    pthread_t threads[MAX_HILOS];
    struct tarea tareas[MAX_HILOS] = {0};
    size_t creados = 0;
    int resultado = DSM_OK;
    fprintf(salida, "TAREA INICIO id=%" PRIu32 " bytes=%zu hilos=%zu valor=%u modo=%s\n",
            id, bytes, hilos, (unsigned)valor, ar ? "Ar" : "api");
    for (size_t i = 0; i < hilos; i++) {
        size_t base = paginas / hilos, extra = paginas % hilos;
        size_t desde = base * i + (i < extra ? i : extra);
        tareas[i] = (struct tarea){.ctx = ctx, .proceso = proceso, .id = id, .bytes = bytes,
            .pagina = pagina, .desde = desde, .hasta = desde + base + (i < extra),
            .valor = valor, .ar = ar, .resultado = DSM_ERR_ESTADO};
        if (pthread_create(&threads[i], NULL, trabajar, &tareas[i]) != 0) {
            resultado = DSM_ERR_HILOS; break;
        }
        creados++;
    }
    size_t procesados = 0;
    uint64_t suma = 0;
    for (size_t i = 0; i < creados; i++) {
        if (pthread_join(threads[i], NULL) != 0 && resultado == DSM_OK) resultado = DSM_ERR_ESTADO;
        fprintf(salida, "TAREA HILO indice=%zu paginas=[%zu,%zu) bytes=%zu resultado=%d\n",
                i, tareas[i].desde, tareas[i].hasta, tareas[i].procesados, tareas[i].resultado);
        if (tareas[i].resultado != DSM_OK && resultado == DSM_OK) resultado = tareas[i].resultado;
        if (tareas[i].procesados > bytes - procesados) resultado = DSM_ERR_ESTADO;
        else procesados += tareas[i].procesados;
        suma += tareas[i].suma;
    }
    if (resultado == DSM_OK && (creados != hilos || procesados != bytes)) resultado = DSM_ERR_ESTADO;
    if (resultado == DSM_OK)
        fprintf(salida, "TAREA OK id=%" PRIu32 " bytes=%zu hilos=%zu suma=%" PRIu64 " modo=%s\n",
                id, procesados, hilos, suma, ar ? "Ar" : "api");
    else fprintf(salida, "TAREA ERROR id=%" PRIu32 " bytes=%zu rc=%d modo=%s\n",
                 id, procesados, resultado, ar ? "Ar" : "api");
    fflush(salida);
    return resultado;
}
