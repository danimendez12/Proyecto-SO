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
    return real[0] == lectura && real[1] == escritura && real[2] == '-' && real[3] == compartida;
}

int main(void)
{
    const size_t pagina = 4096;
    dsm_vistas_t vistas;
    if (dsm_vistas_crear(2 * pagina, &vistas) != 0) {
        return fallar("reserva");
    }
    if (vistas.bytes != 2 * pagina || vistas.a_r == NULL || vistas.c_r == NULL || vistas.a_r == vistas.c_r) {
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
    char perm_a[8] = {0};
    char perm_c[8] = {0};
    if (proteccion(vistas.a_r, perm_a) != 0 || proteccion(vistas.c_r, perm_c) != 0 ||
        !permiso(perm_a, '-', '-', 's') || !permiso(perm_c, 'r', 'w', 's')) {
        fprintf(stderr, "fallo: proteccion inicial A_r %s C_r %s\n", perm_a, perm_c);
        dsm_vistas_cerrar(&vistas);
        return 1;
    }

    for (size_t i = 0; i < vistas.bytes; i++) {
        gestor[i] = (uint8_t)(i & 0xffu);
    }
    if (dsm_vistas_proteger_pagina(&vistas, 0, pagina, PROT_READ | PROT_WRITE) != 0 ||
        dsm_vistas_proteger_pagina(&vistas, 0, 0, PROT_READ) == 0 ||
        dsm_vistas_proteger_pagina(&vistas, 0, pagina + 1, PROT_READ) == 0 ||
        dsm_vistas_proteger_pagina(&vistas, 2, pagina, PROT_READ) == 0 ||
        dsm_vistas_proteger_pagina(&vistas, SIZE_MAX / pagina + 1, pagina, PROT_READ) == 0) {
        dsm_vistas_cerrar(&vistas);
        return fallar("mprotect por pagina o validacion de argumentos");
    }
    const uint8_t *app = vistas.a_r;
    char perm_primera[8] = {0};
    char perm_segunda[8] = {0};
    char perm_gestor_segunda[8] = {0};
    if (proteccion(app, perm_primera) != 0 || !permiso(perm_primera, 'r', 'w', 's') ||
        proteccion(app + pagina, perm_segunda) != 0 || !permiso(perm_segunda, '-', '-', 's') ||
        proteccion(gestor + pagina, perm_gestor_segunda) != 0 ||
        !permiso(perm_gestor_segunda, 'r', 'w', 's')) {
        dsm_vistas_cerrar(&vistas);
        return fallar("proteccion aislada de paginas");
    }

    if (dsm_vistas_proteger(&vistas, PROT_READ) != 0) {
        dsm_vistas_cerrar(&vistas);
        return fallar("mprotect");
    }
    char perm_lectura[8] = {0};
    if (proteccion(vistas.a_r, perm_lectura) != 0 || !permiso(perm_lectura, 'r', '-', 's')) {
        fprintf(stderr, "fallo: proteccion de lectura %s\n", perm_lectura);
        dsm_vistas_cerrar(&vistas);
        return 1;
    }
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

    char perm_primera_final[8] = {0};
    char perm_segunda_final[8] = {0};
    if (dsm_vistas_proteger_pagina(&vistas, 1, pagina, PROT_NONE) != 0 ||
        proteccion(app, perm_primera_final) != 0 || !permiso(perm_primera_final, 'r', 'w', 's') ||
        proteccion(app + pagina, perm_segunda_final) != 0 || !permiso(perm_segunda_final, '-', '-', 's')) {
        dsm_vistas_cerrar(&vistas);
        return fallar("mprotect de la segunda pagina");
    }

    printf("reserva %zu\n", vistas.bytes);
    printf("A_r inicial %s\n", perm_a);
    printf("C_r %s\n", perm_c);
    printf("A_r primera %s segunda %s C_r segunda %s\n", perm_primera, perm_segunda, perm_gestor_segunda);
    printf("A_r lectura %s\n", perm_lectura);
    printf("checksum %llu\n", (unsigned long long)suma);
    printf("iguales %d\n", iguales);
    printf("reflejo %d\n", reflejo);
    dsm_vistas_cerrar(&vistas);
    if (!iguales || !reflejo || suma != 2 * 522240ull) {
        return fallar("bytes distintos");
    }
    return 0;
}
