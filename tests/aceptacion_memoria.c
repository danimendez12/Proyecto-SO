#include "memory.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static int fallar(const char *mensaje)
{
    fprintf(stderr, "fallo: %s\n", mensaje);
    return 1;
}

static int proteccion(const void *p, char salida[8])
{
    FILE *mapas = fopen("/proc/self/maps", "r");
    if (mapas == NULL) {
        return -1;
    }
    unsigned long direccion = (unsigned long)p;
    char linea[512];
    int encontrada = 0;
    while (fgets(linea, sizeof linea, mapas) != NULL) {
        unsigned long ini = 0;
        unsigned long fin = 0;
        char perms[8];
        if (sscanf(linea, "%lx-%lx %7s", &ini, &fin, perms) != 3) {
            continue;
        }
        if (direccion >= ini && direccion < fin) {
            snprintf(salida, 8, "%s", perms);
            encontrada = 1;
            break;
        }
    }
    fclose(mapas);
    return encontrada ? 0 : -1;
}

static int permiso(const char *real, char lectura, char escritura, char compartida)
{
    return real[0] == lectura && real[1] == escritura && real[3] == compartida;
}

int main(void)
{
    dsm_vistas_t vistas;
    if (dsm_vistas_crear(4096, &vistas) != 0) {
        return fallar("reserva");
    }
    if (vistas.bytes != 4096 || vistas.a_r == NULL || vistas.c_r == NULL || vistas.a_r == vistas.c_r) {
        dsm_vistas_cerrar(&vistas);
        return fallar("vistas");
    }
    uint8_t *gestor = vistas.c_r;
    for (size_t i = 0; i < vistas.bytes; i++) {
        if (gestor[i] != 0) {
            dsm_vistas_cerrar(&vistas);
            return fallar("pagina inicial");
        }
    }
    char perm_a[8];
    char perm_c[8];
    if (proteccion(vistas.a_r, perm_a) != 0 || proteccion(vistas.c_r, perm_c) != 0 ||
        !permiso(perm_a, '-', '-', 's') || !permiso(perm_c, 'r', 'w', 's')) {
        fprintf(stderr, "fallo: proteccion inicial A_r %s C_r %s\n", perm_a, perm_c);
        dsm_vistas_cerrar(&vistas);
        return 1;
    }

    for (size_t i = 0; i < vistas.bytes; i++) {
        gestor[i] = (uint8_t)(i & 0xffu);
    }
    if (dsm_vistas_proteger(&vistas, PROT_READ) != 0) {
        dsm_vistas_cerrar(&vistas);
        return fallar("mprotect");
    }
    char perm_lectura[8];
    if (proteccion(vistas.a_r, perm_lectura) != 0 || !permiso(perm_lectura, 'r', '-', 's')) {
        fprintf(stderr, "fallo: proteccion de lectura %s\n", perm_lectura);
        dsm_vistas_cerrar(&vistas);
        return 1;
    }
    const uint8_t *app = vistas.a_r;
    uint64_t suma = 0;
    int iguales = memcmp(app, gestor, vistas.bytes) == 0;
    for (size_t i = 0; i < vistas.bytes; i++) {
        suma += app[i];
    }

    if (dsm_vistas_proteger(&vistas, PROT_READ | PROT_WRITE) != 0) {
        dsm_vistas_cerrar(&vistas);
        return fallar("mprotect escritura");
    }
    uint8_t *app_rw = vistas.a_r;
    app_rw[10] = 0x5a;
    int reflejo = gestor[10] == 0x5a && app_rw[10] == 0x5a;

    printf("reserva 4096\n");
    printf("A_r inicial %s\n", perm_a);
    printf("C_r %s\n", perm_c);
    printf("A_r lectura %s\n", perm_lectura);
    printf("checksum %llu\n", (unsigned long long)suma);
    printf("iguales %d\n", iguales);
    printf("reflejo %d\n", reflejo);
    dsm_vistas_cerrar(&vistas);
    if (!iguales || !reflejo || suma != 522240ull) {
        return fallar("bytes distintos");
    }
    return 0;
}
