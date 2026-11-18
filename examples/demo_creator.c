#include "dsm.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int numero(const char *texto, uint64_t maximo, uint64_t *salida)
{
    char *fin;
    errno = 0;
    unsigned long long valor = strtoull(texto, &fin, 10);
    if (*texto < '0' || *texto > '9' || errno != 0 || *fin != '\0' || valor > maximo) {
        return -1;
    }
    *salida = valor;
    return 0;
}

static void mostrar(const void *base, size_t paginas, size_t pagina)
{
    const volatile unsigned char *ar = base;
    for (size_t p = 0; p < paginas; p++) {
        char texto[64] = {0};
        for (size_t i = 0; i < sizeof texto - 1; i++) {
            texto[i] = (char)ar[p * pagina + i];
            if (texto[i] == '\0') {
                break;
            }
        }
        printf("pagina %zu [%s]\n", p, texto);
    }
}

static void escribir(void *base, size_t pagina, const char *texto)
{
    volatile unsigned char *ar = base;
    for (size_t i = 0; i <= strlen(texto); i++) {
        ar[pagina + i] = (unsigned char)texto[i];
    }
}

static int compartir(dsm_context_t *ctx, dsm_proc_id_t proceso, dsm_reserva_id_t reserva,
                     void *ar, size_t paginas, size_t pagina, int *registrado, int *liberada)
{
    puts("Comandos: leer, escribir, salir. El cliente debe salir antes de liberar.");
    char linea[80];
    while (fgets(linea, sizeof linea, stdin) != NULL) {
        linea[strcspn(linea, "\r\n")] = '\0';
        if (strcmp(linea, "leer") == 0) {
            mostrar(ar, paginas, pagina);
        } else if (strcmp(linea, "escribir") == 0) {
            escribir(ar, (paginas / 2) * pagina, "Adios TEC");
            mostrar(ar, paginas, pagina);
        } else if (strcmp(linea, "salir") == 0) {
            int rc = dsm_hilo_desregistrar(ctx);
            if (rc != DSM_OK) {
                return rc;
            }
            *registrado = 0;
            rc = dsm_liberar(ctx, reserva);
            if (rc == DSM_OK) {
                *liberada = 1;
                return DSM_OK;
            }
            if (rc != DSM_ERR_OCUPADA) {
                return rc;
            }
            puts("La reserva sigue abierta en otro nodo. Cierre el cliente y repita salir.");
            rc = dsm_hilo_registrar(ctx, proceso);
            if (rc != DSM_OK) {
                return rc;
            }
            *registrado = 1;
        } else if (*linea != '\0') {
            puts("Comandos: leer, escribir, salir.");
        }
    }
    return ferror(stdin) ? DSM_ERR_ESTADO : DSM_OK;
}

static int aportar(dsm_context_t *ctx)
{
    puts("Hogar disponible. Escriba salir despues de liberar la reserva.");
    char linea[80];
    while (fgets(linea, sizeof linea, stdin) != NULL) {
        linea[strcspn(linea, "\r\n")] = '\0';
        if (strcmp(linea, "salir") == 0) {
            int rc = dsm_cerrar(ctx);
            if (rc != DSM_ERR_OCUPADA) {
                return rc;
            }
            puts("Hay paginas aportadas en uso. Repita salir cuando el creador las libere.");
        }
    }
    return dsm_cerrar(ctx);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    int modo_compartido = argc >= 4 && argc <= 6 && strcmp(argv[2], "compartir") == 0;
    int solo_aporte = argc == 4 && strcmp(argv[2], "aportar") == 0;
    uint64_t id = 0, paginas = 1, pagina = 4096;
    if ((argc > 2 && !modo_compartido && !solo_aporte) ||
        ((modo_compartido || solo_aporte) && (numero(argv[3], UINT16_MAX, &id) != 0 ||
         (argc >= 5 && (numero(argv[4], SIZE_MAX, &paginas) != 0 || paginas == 0)) ||
         (argc == 6 && (numero(argv[5], SIZE_MAX, &pagina) != 0 || pagina < 64)) ||
         (paginas > 1 && argc != 6) || paginas > SIZE_MAX / pagina))) {
        fprintf(stderr, "uso: %s [config]\n       %s CONFIG compartir NODO [PAGINAS [PAGE_SIZE]]\n"
                        "       %s CONFIG aportar NODO\n"
                        "Indique PAGE_SIZE de la configuracion cuando use varias paginas.\n", argv[0], argv[0], argv[0]);
        return 1;
    }
    const char *config = argc >= 2 ? argv[1] : "config/lab.ini";
    dsm_context_t *ctx;
    int rc = dsm_iniciar(config, (dsm_node_id_t)id, &ctx);
    if (rc != DSM_OK) {
        fprintf(stderr, "dsm_iniciar: %d\n", rc);
        return 1;
    }

    if (solo_aporte) {
        rc = aportar(ctx);
        if (rc != DSM_OK) {
            fprintf(stderr, "cierre del hogar: %d\n", rc);
        }
        return rc == DSM_OK ? 0 : 1;
    }
    const char mensaje[] = "Hola TEC desde Ar";
    dsm_proc_id_t proceso;
    dsm_reserva_id_t reserva;
    void *ar;
    int registrado = 0, liberada = 0;
    rc = dsm_proc_abrir(ctx, &proceso);
    if (rc == DSM_OK) {
        size_t bytes = modo_compartido ? (size_t)(paginas * pagina) : sizeof mensaje;
        rc = dsm_reservar(ctx, proceso, bytes, &reserva, &ar);
    }
    if (rc == DSM_OK) {
        rc = dsm_hilo_registrar(ctx, proceso);
    }
    if (rc == DSM_OK) {
        registrado = 1;
        if (modo_compartido) {
            for (size_t p = 0; p < (size_t)paginas; p++) {
                dsm_node_id_t hogar;
                uint64_t gpn;
                rc = dsm_ubicacion_pagina(ctx, reserva, p, &hogar, &gpn);
                if (rc != DSM_OK) {
                    break;
                }
                printf("reserva %" PRIu32 " pagina %zu hogar %u GPN %" PRIu64 "\n",
                       reserva, p, (unsigned)hogar, gpn);
                char texto[64];
                if (p == 0) {
                    snprintf(texto, sizeof texto, "Hola TEC");
                } else {
                    snprintf(texto, sizeof texto, "Pagina %zu original", p);
                }
                escribir(ar, p * (size_t)pagina, texto);
            }
            printf("nodo %" PRIu64 " reserva %" PRIu32 " bytes %" PRIu64 " Ar %p\n",
                   id, reserva, paginas * pagina, ar);
            if (rc == DSM_OK) {
                mostrar(ar, (size_t)paginas, (size_t)pagina);
                rc = compartir(ctx, proceso, reserva, ar, (size_t)paginas, (size_t)pagina,
                               &registrado, &liberada);
            }
        } else {
            memcpy(ar, mensaje, sizeof mensaje);
            printf("reserva %" PRIu32 ": %s\n", reserva, (char *)ar);
        }
    }
    if (registrado) {
        int desregistro = dsm_hilo_desregistrar(ctx);
        if (rc == DSM_OK) {
            rc = desregistro;
        }
    }
    if (rc == DSM_OK && !liberada) {
        rc = dsm_liberar(ctx, reserva);
    }

    int cierre = dsm_cerrar(ctx);
    if (rc != DSM_OK || cierre != DSM_OK) {
        fprintf(stderr, "DSM: operacion %d, cierre %d\n", rc, cierre);
        return 1;
    }
    return 0;
}
