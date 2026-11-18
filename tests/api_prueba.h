#ifndef DSM_API_PRUEBA_H
#define DSM_API_PRUEBA_H

#include "dsm.h"

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum api_operacion {
    API_INICIAR, API_RESERVAR, API_ABRIR, API_REGISTRAR, API_DESREGISTRAR,
    API_LEER_AR, API_ESCRIBIR_AR, API_LEER, API_ESCRIBIR, API_PERMISOS,
    API_DESVINCULAR, API_LIBERAR, API_CERRAR, API_CONCURRENCIA, API_UBICACION,
    API_TRADUCIR, API_TERMINAR
};

struct api_orden {
    enum api_operacion operacion;
    dsm_reserva_id_t id;
    size_t offset;
    size_t bytes;
    unsigned char datos[64];
};

struct api_respuesta {
    int resultado;
    dsm_reserva_id_t id;
    size_t bytes;
    uintptr_t ar;
    uintptr_t traducida;
    uint64_t gpn;
    dsm_node_id_t hogar;
    unsigned char datos[64];
    char permisos[4][5];
};

struct api_proceso {
    pid_t pid;
    int entrada;
    int salida;
    FILE *registro;
};

struct api_escritura {
    dsm_context_t *ctx;
    dsm_proc_id_t proceso;
    dsm_reserva_id_t reserva;
    volatile unsigned char *ar;
    pthread_barrier_t *barrera;
    size_t offset;
    unsigned char valor;
    int directa;
    int resultado;
};

static void *api_escribir_paralelo(void *arg)
{
    struct api_escritura *tarea = arg;
    tarea->resultado = dsm_hilo_registrar(tarea->ctx, tarea->proceso);
    pthread_barrier_wait(tarea->barrera);
    if (tarea->resultado != DSM_OK) {
        return NULL;
    }
    if (tarea->directa) {
        tarea->ar[tarea->offset] = tarea->valor;
    } else {
        tarea->resultado = dsm_escribir(tarea->ctx, tarea->reserva, tarea->offset,
                                        &tarea->valor, 1);
    }
    if (dsm_hilo_desregistrar(tarea->ctx) != DSM_OK) {
        tarea->resultado = DSM_ERR_ESTADO;
    }
    return NULL;
}

static int api_escribir_dos(dsm_context_t *ctx, dsm_proc_id_t proceso, uint32_t reserva,
                             void *base, int ambas_controladas)
{
    pthread_barrier_t barrera;
    if (pthread_barrier_init(&barrera, NULL, 2) != 0) {
        return DSM_ERR_ESTADO;
    }
    struct api_escritura tareas[2] = {
        {.ctx = ctx, .proceso = proceso, .reserva = reserva, .ar = base,
         .barrera = &barrera, .offset = 0, .valor = 0x41, .directa = !ambas_controladas},
        {.ctx = ctx, .proceso = proceso, .reserva = reserva, .ar = base,
         .barrera = &barrera, .offset = 64, .valor = 0x42},
    };
    pthread_t hilo;
    if (pthread_create(&hilo, NULL, api_escribir_paralelo, &tareas[0]) != 0) {
        pthread_barrier_destroy(&barrera);
        return DSM_ERR_ESTADO;
    }
    api_escribir_paralelo(&tareas[1]);
    int rc = pthread_join(hilo, NULL);
    pthread_barrier_destroy(&barrera);
    return rc == 0 && tareas[0].resultado == DSM_OK && tareas[1].resultado == DSM_OK
        ? DSM_OK : DSM_ERR_ESTADO;
}

static int api_transferir(int fd, void *buf, size_t bytes, int escribir)
{
    unsigned char *posicion = buf;
    while (bytes != 0) {
        ssize_t n = escribir ? write(fd, posicion, bytes) : read(fd, posicion, bytes);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return -1;
        }
        posicion += n;
        bytes -= (size_t)n;
    }
    return 0;
}

static int api_proteccion(const void *direccion, char permisos[5])
{
    FILE *mapas = fopen("/proc/self/maps", "r");
    if (mapas == NULL) {
        return -1;
    }
    uintptr_t objetivo = (uintptr_t)direccion;
    char linea[512];
    int resultado = -1;
    while (fgets(linea, sizeof linea, mapas) != NULL) {
        uintptr_t inicio, fin;
        char actual[5];
        if (sscanf(linea, "%" SCNxPTR "-%" SCNxPTR " %4s", &inicio, &fin, actual) == 3 &&
            inicio <= objetivo && objetivo < fin) {
            memcpy(permisos, actual, 5);
            resultado = 0;
            break;
        }
    }
    fclose(mapas);
    return resultado;
}

static void api_aplicacion(const char *config, uint16_t nodo, int entrada, int salida)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    dsm_context_t *ctx = NULL;
    dsm_proc_id_t proceso = 0;
    dsm_reserva_id_t reserva = 0;
    void *base = NULL;
    size_t bytes = 0;
    int registrado = 0;
    long pagina = sysconf(_SC_PAGESIZE);
    struct api_orden orden;
    while (api_transferir(entrada, &orden, sizeof orden, 0) == 0) {
        struct api_respuesta respuesta = {0};
        switch (orden.operacion) {
        case API_INICIAR:
            respuesta.resultado = dsm_iniciar(config, nodo, &ctx);
            if (respuesta.resultado == DSM_OK) {
                respuesta.resultado = dsm_proc_abrir(ctx, &proceso);
            }
            break;
        case API_RESERVAR:
            respuesta.resultado = dsm_reservar(ctx, proceso, orden.bytes, &reserva, &base);
            if (respuesta.resultado == DSM_OK) {
                bytes = orden.bytes;
            }
            break;
        case API_ABRIR:
        {
            void *abierta;
            size_t tamano;
            respuesta.resultado = dsm_abrir_reserva(ctx, orden.id, &abierta, &tamano);
            if (respuesta.resultado == DSM_OK) {
                reserva = orden.id;
                base = abierta;
                bytes = tamano;
            }
            break;
        }
        case API_REGISTRAR:
            respuesta.resultado = dsm_hilo_registrar(ctx, proceso);
            if (respuesta.resultado == DSM_OK) {
                registrado = 1;
            }
            break;
        case API_DESREGISTRAR:
            respuesta.resultado = dsm_hilo_desregistrar(ctx);
            if (respuesta.resultado == DSM_OK) {
                registrado = 0;
            }
            break;
        case API_LEER_AR:
        case API_ESCRIBIR_AR:
            if (!registrado || base == NULL || orden.bytes > sizeof orden.datos ||
                orden.offset > bytes || orden.bytes > bytes - orden.offset) {
                respuesta.resultado = DSM_ERR_INVAL;
                break;
            }
            for (size_t i = 0; i < orden.bytes; i++) {
                volatile unsigned char *ar = base;
                if (orden.operacion == API_ESCRIBIR_AR) {
                    ar[orden.offset + i] = orden.datos[i];
                } else {
                    respuesta.datos[i] = ar[orden.offset + i];
                }
            }
            break;
        case API_LEER:
            respuesta.resultado = orden.bytes <= sizeof respuesta.datos
                ? dsm_leer(ctx, reserva, orden.offset, respuesta.datos, orden.bytes)
                : DSM_ERR_INVAL;
            break;
        case API_ESCRIBIR:
            respuesta.resultado = orden.bytes <= sizeof orden.datos
                ? dsm_escribir(ctx, reserva, orden.offset, orden.datos, orden.bytes)
                : DSM_ERR_INVAL;
            break;
        case API_PERMISOS:
            respuesta.resultado = DSM_OK;
            for (size_t i = 0; i < orden.bytes && i < 4; i++) {
                if (pagina <= 0 || api_proteccion((unsigned char *)base + i * (size_t)pagina,
                                                  respuesta.permisos[i]) != 0) {
                    respuesta.resultado = DSM_ERR_ESTADO;
                }
            }
            break;
        case API_DESVINCULAR:
            respuesta.resultado = dsm_desvincular_reserva(ctx, orden.id);
            break;
        case API_LIBERAR:
            respuesta.resultado = dsm_liberar(ctx, orden.id);
            break;
        case API_CERRAR:
            respuesta.resultado = dsm_cerrar(ctx);
            if (respuesta.resultado == DSM_OK) {
                ctx = NULL;
            }
            break;
        case API_CONCURRENCIA:
            if (registrado || base == NULL || bytes <= 64) {
                respuesta.resultado = DSM_ERR_INVAL;
            } else {
                respuesta.resultado = api_escribir_dos(ctx, proceso, reserva, base,
                                                       orden.bytes != 0);
            }
            break;
        case API_UBICACION:
            respuesta.resultado = dsm_ubicacion_pagina(ctx, reserva, orden.offset,
                                                       &respuesta.hogar, &respuesta.gpn);
            break;
        case API_TRADUCIR:
        {
            void *direccion = NULL;
            respuesta.resultado = dsm_traducir(ctx, reserva, orden.offset, &direccion);
            respuesta.traducida = (uintptr_t)direccion;
            break;
        }
        case API_TERMINAR:
            if (registrado && dsm_hilo_desregistrar(ctx) != DSM_OK) {
                _exit(1);
            }
            if (ctx != NULL && dsm_cerrar(ctx) != DSM_OK) {
                _exit(1);
            }
            _exit(0);
        }
        respuesta.id = reserva;
        respuesta.bytes = bytes;
        respuesta.ar = (uintptr_t)base;
        if (api_transferir(salida, &respuesta, sizeof respuesta, 1) != 0) {
            break;
        }
    }
    _exit(1);
}

static int api_iniciar_proceso(struct api_proceso *hijo, const char *config, uint16_t nodo)
{
    hijo->registro = tmpfile();
    if (hijo->registro == NULL) {
        return -1;
    }
    int entrada[2], salida[2];
    if (pipe(entrada) != 0) {
        fclose(hijo->registro);
        hijo->registro = NULL;
        return -1;
    }
    if (pipe(salida) != 0) {
        close(entrada[0]);
        close(entrada[1]);
        fclose(hijo->registro);
        hijo->registro = NULL;
        return -1;
    }
    hijo->pid = fork();
    if (hijo->pid == 0) {
        close(entrada[1]);
        close(salida[0]);
        if (dup2(fileno(hijo->registro), STDOUT_FILENO) < 0 ||
            dup2(fileno(hijo->registro), STDERR_FILENO) < 0) {
            _exit(1);
        }
        api_aplicacion(config, nodo, entrada[0], salida[1]);
    }
    close(entrada[0]);
    close(salida[1]);
    if (hijo->pid < 0) {
        close(entrada[1]);
        close(salida[0]);
        fclose(hijo->registro);
        hijo->registro = NULL;
        return -1;
    }
    hijo->entrada = entrada[1];
    hijo->salida = salida[0];
    return 0;
}

static int api_ejecutar(struct api_proceso *hijo, const struct api_orden *orden,
                        struct api_respuesta *respuesta)
{
    struct api_orden copia = *orden;
    if (api_transferir(hijo->entrada, &copia, sizeof copia, 1) != 0) {
        return -1;
    }
    struct pollfd espera = {.fd = hijo->salida, .events = POLLIN};
    int rc;
    do {
        rc = poll(&espera, 1, 8000);
    } while (rc < 0 && errno == EINTR);
    return rc > 0 ? api_transferir(hijo->salida, respuesta, sizeof *respuesta, 0) : -1;
}

static int api_operar(struct api_proceso *hijo, enum api_operacion operacion, int esperado)
{
    const struct api_orden orden = {.operacion = operacion};
    struct api_respuesta respuesta;
    return api_ejecutar(hijo, &orden, &respuesta) == 0 && respuesta.resultado == esperado ? 0 : -1;
}

static int api_tiene_traza(struct api_proceso *hijo, const char *frase)
{
    char texto[32768];
    ssize_t n = pread(fileno(hijo->registro), texto, sizeof texto - 1, 0);
    if (n < 0 || n == (ssize_t)sizeof texto - 1) {
        return 0;
    }
    texto[n] = '\0';
    return strstr(texto, frase) != NULL;
}

static inline size_t api_contar_traza(struct api_proceso *hijo, const char *frase)
{
    char texto[32768];
    ssize_t n = pread(fileno(hijo->registro), texto, sizeof texto - 1, 0);
    if (n < 0 || n == (ssize_t)sizeof texto - 1) {
        return SIZE_MAX;
    }
    texto[n] = '\0';
    size_t cantidad = 0;
    const char *posicion = texto;
    while ((posicion = strstr(posicion, frase)) != NULL) {
        cantidad++;
        posicion += strlen(frase);
    }
    return cantidad;
}

static int api_detener_proceso(struct api_proceso *hijo)
{
    if (hijo->pid <= 0) {
        return 0;
    }
    struct api_orden orden = {.operacion = API_TERMINAR};
    int rc = api_transferir(hijo->entrada, &orden, sizeof orden, 1);
    close(hijo->entrada);
    close(hijo->salida);
    int estado;
    pid_t terminado;
    for (unsigned i = 0; i < 300; i++) {
        terminado = waitpid(hijo->pid, &estado, WNOHANG);
        if (terminado == hijo->pid || (terminado < 0 && errno != EINTR)) {
            break;
        }
        const struct timespec pausa = {.tv_nsec = 10000000};
        nanosleep(&pausa, NULL);
    }
    if (terminado == 0) {
        kill(hijo->pid, SIGKILL);
        while (waitpid(hijo->pid, &estado, 0) < 0 && errno == EINTR) {
        }
        rc = -1;
    }
    hijo->pid = 0;
    char linea[512];
    rewind(hijo->registro);
    while (fgets(linea, sizeof linea, hijo->registro) != NULL) {
        fputs(linea, stdout);
    }
    fclose(hijo->registro);
    hijo->registro = NULL;
    return rc == 0 && terminado > 0 && WIFEXITED(estado) && WEXITSTATUS(estado) == 0 ? 0 : -1;
}

#endif
