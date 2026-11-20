#include "dsm.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define EXIGIR(condicion) do { \
    if (!(condicion)) { \
        fprintf(stderr, "aceptacion_multipagina:%d: %s\n", __LINE__, #condicion); \
        goto terminar; \
    } \
} while (0)

/* Fallo exclusivo del enlace de esta aceptacion, en el segundo alias del memfd. */
static _Thread_local int inyectar_mmap;
static _Thread_local unsigned mapas_compartidos;
static _Thread_local void *ar_parcial;
static _Thread_local int fd_parcial;
static _Thread_local struct stat identidad_parcial;

static int contar_descriptores(void)
{
    DIR *directorio = opendir("/proc/self/fd");
    if (directorio == NULL) {
        return -1;
    }
    int cantidad = 0;
    struct dirent *entrada;
    while ((entrada = readdir(directorio)) != NULL) {
        if (entrada->d_name[0] != '.') {
            cantidad++;
        }
    }
    closedir(directorio);
    return cantidad;
}

void *__real_mmap(void *direccion, size_t bytes, int prot, int flags, int fd, off_t offset);

void *__wrap_mmap(void *direccion, size_t bytes, int prot, int flags, int fd, off_t offset)
{
    if (inyectar_mmap && (flags & MAP_SHARED) != 0 && fd >= 0) {
        mapas_compartidos++;
        if (mapas_compartidos == 2) {
            inyectar_mmap = 0;
            errno = ENOMEM;
            return MAP_FAILED;
        }
        ar_parcial = __real_mmap(direccion, bytes, prot, flags, fd, offset);
        fd_parcial = fd;
        if (fstat(fd, &identidad_parcial) != 0) {
            if (ar_parcial != MAP_FAILED) {
                munmap(ar_parcial, bytes);
                ar_parcial = MAP_FAILED;
            }
            return MAP_FAILED;
        }
        return ar_parcial;
    }
    return __real_mmap(direccion, bytes, prot, flags, fd, offset);
}

struct escritura_pagina {
    dsm_context_t *ctx;
    dsm_proc_id_t proceso;
    volatile unsigned char *ar;
    size_t bytes;
    unsigned char patron;
    int resultado;
};

static void *escribir_pagina(void *arg)
{
    struct escritura_pagina *t = arg;
    t->resultado = dsm_hilo_registrar(t->ctx, t->proceso);
    if (t->resultado != DSM_OK) {
        return NULL;
    }
    for (size_t i = 0; i < t->bytes; i++) {
        t->ar[i] = t->patron;
    }
    t->resultado = dsm_hilo_desregistrar(t->ctx);
    return NULL;
}

static int probar(const char *configuracion, size_t pagina)
{
    size_t logico = 3 * pagina - 17;
    dsm_context_t *ctx = NULL;
    dsm_proc_id_t proc;
    dsm_reserva_id_t reserva, otra;
    void *base, *otra_base;
    int registrado = 0, resultado = 1;
    EXIGIR(dsm_iniciar(configuracion, 0, &ctx) == DSM_OK);
    EXIGIR(dsm_proc_abrir(ctx, &proc) == DSM_OK);
    EXIGIR(dsm_hilo_registrar(ctx, proc) == DSM_OK);
    registrado = 1;
    EXIGIR(dsm_reservar(ctx, proc, 0, &reserva, &base) == DSM_ERR_INVAL);
    EXIGIR(dsm_reservar(ctx, proc, SIZE_MAX, &reserva, &base) == DSM_ERR_NOMEM);
    EXIGIR(dsm_reservar(ctx, proc, logico, &reserva, &base) == DSM_OK && base != NULL);
    void *abierta;
    size_t bytes;
    EXIGIR(dsm_abrir_reserva(ctx, reserva, &abierta, &bytes) == DSM_OK &&
           abierta == base && bytes == logico);
    EXIGIR(dsm_reservar(ctx, proc, pagina, &otra, &otra_base) == DSM_OK &&
           otra != reserva && otra_base != base);
    volatile unsigned char *ar = base;
    for (size_t i = 0; i < logico; i++) {
        EXIGIR(ar[i] == 0);
    }
    struct escritura_pagina tareas[2] = {
        {.ctx = ctx, .proceso = proc, .ar = ar, .bytes = pagina, .patron = 0x31},
        {.ctx = ctx, .proceso = proc, .ar = ar + 2 * pagina, .bytes = pagina - 17, .patron = 0x73},
    };
    pthread_t hilos[2];
    EXIGIR(pthread_create(&hilos[0], NULL, escribir_pagina, &tareas[0]) == 0);
    if (pthread_create(&hilos[1], NULL, escribir_pagina, &tareas[1]) != 0) {
        pthread_join(hilos[0], NULL);
        goto terminar;
    }
    int union_cero = pthread_join(hilos[0], NULL);
    int union_uno = pthread_join(hilos[1], NULL);
    EXIGIR(union_cero == 0 && union_uno == 0 &&
           tareas[0].resultado == DSM_OK && tareas[1].resultado == DSM_OK);
    for (size_t i = 0; i < pagina; i++) {
        ar[pagina + i] = 0x52;
    }
    for (size_t i = 0; i < logico; i++) {
        unsigned char esperado = i < pagina ? 0x31 : i < 2 * pagina ? 0x52 : 0x73;
        EXIGIR(ar[i] == esperado);
    }
    const unsigned char cruce[] = {1, 3, 5, 7, 9, 11, 13, 15};
    unsigned char leido[sizeof cruce];
    EXIGIR(dsm_escribir(ctx, reserva, pagina - 4, cruce, sizeof cruce) == DSM_OK);
    EXIGIR(dsm_leer(ctx, reserva, pagina - 4, leido, sizeof leido) == DSM_OK &&
           memcmp(cruce, leido, sizeof cruce) == 0);
    EXIGIR(ar[pagina - 5] == 0x31 && ar[pagina + 4] == 0x52 && ar[2 * pagina] == 0x73);
    EXIGIR(dsm_escribir(ctx, reserva, 2 * pagina - 4, cruce, sizeof cruce) == DSM_OK);
    EXIGIR(dsm_leer(ctx, reserva, 2 * pagina - 4, leido, sizeof leido) == DSM_OK &&
           memcmp(cruce, leido, sizeof cruce) == 0);
    unsigned char solapada[32];
    unsigned char esperado_solapado[32];
    for (size_t i = 0; i < sizeof solapada; i++) {
        solapada[i] = (unsigned char)(0xb0 + i);
    }
    EXIGIR(dsm_escribir(ctx, reserva, pagina - 16, solapada, sizeof solapada) == DSM_OK);
    memcpy(esperado_solapado, solapada, sizeof solapada);
    memmove(esperado_solapado + 4, esperado_solapado + 8, 16);
    EXIGIR(dsm_escribir(ctx, reserva, pagina - 12, (unsigned char *)base + pagina - 8, 16) == DSM_OK);
    EXIGIR(dsm_leer(ctx, reserva, pagina - 16, solapada, sizeof solapada) == DSM_OK &&
           memcmp(solapada, esperado_solapado, sizeof solapada) == 0);
    memmove(esperado_solapado + 8, esperado_solapado + 4, 16);
    EXIGIR(dsm_leer(ctx, reserva, pagina - 12, (unsigned char *)base + pagina - 8, 16) == DSM_OK);
    EXIGIR(dsm_leer(ctx, reserva, pagina - 16, solapada, sizeof solapada) == DSM_OK &&
           memcmp(solapada, esperado_solapado, sizeof solapada) == 0);
    ar[logico - 1] = 0x9a;
    EXIGIR(dsm_traducir(ctx, reserva, logico - 1, &abierta) == DSM_OK &&
           abierta == (unsigned char *)base + logico - 1);
    EXIGIR(dsm_traducir(ctx, reserva, logico, &abierta) == DSM_ERR_INVAL);
    EXIGIR(dsm_leer(ctx, reserva, logico - 1, leido, 2) == DSM_ERR_INVAL);
    EXIGIR(dsm_escribir(ctx, reserva, logico - 1, cruce, 2) == DSM_ERR_INVAL);
    EXIGIR(dsm_leer(ctx, reserva, logico, NULL, 0) == DSM_OK);
    EXIGIR(dsm_leer(ctx, reserva, logico + 1, NULL, 0) == DSM_ERR_INVAL);
    EXIGIR(ar[logico - 1] == 0x9a);
    EXIGIR(dsm_liberar(ctx, reserva) == DSM_ERR_OCUPADA);
    EXIGIR(dsm_hilo_desregistrar(ctx) == DSM_OK);
    registrado = 0;
    EXIGIR(dsm_liberar(ctx, reserva) == DSM_OK);
    EXIGIR(dsm_hilo_registrar(ctx, proc) == DSM_OK);
    registrado = 1;
    ((volatile unsigned char *)otra_base)[0] = 0x64;
    EXIGIR(dsm_leer(ctx, otra, 0, leido, 1) == DSM_OK && leido[0] == 0x64);
    EXIGIR(dsm_hilo_desregistrar(ctx) == DSM_OK);
    registrado = 0;
    EXIGIR(dsm_liberar(ctx, otra) == DSM_OK);
    resultado = 0;
terminar:
    if (registrado && dsm_hilo_desregistrar(ctx) != DSM_OK) {
        resultado = 1;
    }
    if (ctx != NULL && dsm_cerrar(ctx) != DSM_OK) {
        resultado = 1;
    }
    return resultado;
}

static int probar_rollback(const char *configuracion, size_t pagina)
{
    dsm_context_t *ctx = NULL;
    dsm_proc_id_t proc;
    dsm_reserva_id_t reserva;
    void *base;
    int resultado = 1;
    EXIGIR(dsm_iniciar(configuracion, 0, &ctx) == DSM_OK);
    EXIGIR(dsm_proc_abrir(ctx, &proc) == DSM_OK);
    mapas_compartidos = 0;
    ar_parcial = MAP_FAILED;
    fd_parcial = -1;
    inyectar_mmap = 1;
    int rc = dsm_reservar(ctx, proc, 2 * pagina, &reserva, &base);
    inyectar_mmap = 0;
    EXIGIR(rc == DSM_ERR_NOMEM && mapas_compartidos == 2 && ar_parcial != MAP_FAILED);
    unsigned char residente;
    EXIGIR(mincore(ar_parcial, (size_t)sysconf(_SC_PAGESIZE), &residente) == -1 && errno == ENOMEM);
    struct stat actual;
    int fd_existe = fstat(fd_parcial, &actual);
    EXIGIR((fd_existe == -1 && errno == EBADF) ||
           (fd_existe == 0 && (actual.st_dev != identidad_parcial.st_dev ||
                              actual.st_ino != identidad_parcial.st_ino)));
    size_t bytes;
    EXIGIR(dsm_abrir_reserva(ctx, 1, &base, &bytes) == DSM_ERR_INVAL);
    /* La contribucion local completa sigue disponible tras desmontar el alias parcial. */
    EXIGIR(dsm_reservar(ctx, proc, 1048576, &reserva, &base) == DSM_OK);
    EXIGIR(dsm_liberar(ctx, reserva) == DSM_OK);
    resultado = 0;
terminar:
    inyectar_mmap = 0;
    if (ctx != NULL && dsm_cerrar(ctx) != DSM_OK) {
        resultado = 1;
    }
    return resultado;
}

int main(void)
{
    alarm(40);
    int descriptores = contar_descriptores();
    long pagina_so = sysconf(_SC_PAGESIZE);
    if (descriptores < 0 || pagina_so <= 0 || probar("config/lab.ini", (size_t)pagina_so) != 0 ||
        probar_rollback("config/lab.ini", (size_t)pagina_so) != 0) {
        return 1;
    }
    FILE *config = tmpfile();
    if (config == NULL) {
        return 1;
    }
    int rc = fprintf(config,
        "page_size=8192\nmemoria_inicial=1048576\npuerto_rango_inicio=27200\n"
        "puerto_rango_fin=27299\nnodo=0 unico 127.0.0.1 27200\n");
    char ruta[64];
    snprintf(ruta, sizeof ruta, "/proc/self/fd/%d", fileno(config));
    int resultado = rc < 0 || fflush(config) != 0 || probar(ruta, 8192) != 0 ||
                    probar_rollback(ruta, 8192) != 0;
    fclose(config);
    if (resultado != 0) {
        return 1;
    }
    if (contar_descriptores() != descriptores) {
        fprintf(stderr, "aceptacion_multipagina: descriptores sin cerrar\n");
        return 1;
    }
    puts("Multipagina local: tres paginas, dos trabajadores, limites y rollback mmap con P=4096/8192 verificados");
    return 0;
}
