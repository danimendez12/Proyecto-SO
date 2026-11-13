#include "memory.h"

#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int dsm_vistas_crear(size_t bytes, dsm_vistas_t *salida)
{
    if (salida == NULL) {
        return -1;
    }
    memset(salida, 0, sizeof *salida);
    salida->fd = -1;
    long pagina = sysconf(_SC_PAGESIZE);
    if (pagina <= 0 || bytes == 0 || (bytes % (size_t)pagina) != 0) {
        return -1;
    }
    int fd = memfd_create("dsm-reserva", MFD_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    if (ftruncate(fd, (off_t)bytes) != 0) {
        close(fd);
        return -1;
    }
    void *app = mmap(NULL, bytes, PROT_NONE, MAP_SHARED, fd, 0);
    if (app == MAP_FAILED) {
        close(fd);
        return -1;
    }
    void *gestor = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (gestor == MAP_FAILED) {
        munmap(app, bytes);
        close(fd);
        return -1;
    }
    memset(gestor, 0, bytes);
    salida->fd = fd;
    salida->bytes = bytes;
    salida->a_r = app;
    salida->c_r = gestor;
    return 0;
}

int dsm_vistas_proteger(dsm_vistas_t *vistas, int prot)
{
    if (vistas == NULL || vistas->a_r == NULL || vistas->bytes == 0) {
        return -1;
    }
    if (mprotect(vistas->a_r, vistas->bytes, prot) != 0) {
        return -1;
    }
    return 0;
}

int dsm_vistas_proteger_pagina(dsm_vistas_t *vistas, size_t indice, size_t page_size, int prot)
{
    /* El cociente limita el indice antes de calcular el desplazamiento. */
    if (vistas == NULL || vistas->a_r == NULL || page_size == 0 ||
        indice >= vistas->bytes / page_size) {
        return -1;
    }
    long pagina = sysconf(_SC_PAGESIZE);
    if (pagina <= 0 || page_size % (size_t)pagina != 0 ||
        (uintptr_t)vistas->a_r % (size_t)pagina != 0) {
        return -1;
    }
    uint8_t *inicio = (uint8_t *)vistas->a_r + indice * page_size;
    return mprotect(inicio, page_size, prot);
}

void dsm_vistas_cerrar(dsm_vistas_t *vistas)
{
    if (vistas == NULL) {
        return;
    }
    if (vistas->a_r != NULL && vistas->bytes > 0) {
        munmap(vistas->a_r, vistas->bytes);
    }
    if (vistas->c_r != NULL && vistas->bytes > 0) {
        munmap(vistas->c_r, vistas->bytes);
    }
    if (vistas->fd >= 0) {
        close(vistas->fd);
    }
    memset(vistas, 0, sizeof *vistas);
    vistas->fd = -1;
}
