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

static int mostrar(dsm_context_t *ctx, dsm_reserva_id_t reserva, size_t paginas, size_t pagina)
{
    for (size_t p = 0; p < paginas; p++) {
        char texto[64] = {0};
        int rc = dsm_leer(ctx, reserva, p * pagina, texto, sizeof texto - 1);
        if (rc != DSM_OK) {
            fprintf(stderr, "dsm_leer: pagina %zu, error DSM %d\n", p, rc);
            return rc;
        }
        printf("pagina %zu [%s]\n", p, texto);
    }
    return DSM_OK;
}

static int escribir(dsm_context_t *ctx, dsm_reserva_id_t reserva, size_t ordinal,
                    size_t pagina, const char *texto)
{
    int rc = dsm_escribir(ctx, reserva, ordinal * pagina, texto, strlen(texto) + 1);
    if (rc != DSM_OK) {
        fprintf(stderr, "dsm_escribir: pagina %zu, error DSM %d\n", ordinal, rc);
    }
    return rc;
}

static int compartir(dsm_context_t *ctx, dsm_proc_id_t proceso, dsm_reserva_id_t reserva,
                     size_t paginas, size_t pagina, int *registrado, int *liberada)
{
    puts("Comandos: leer, escribir, salir. El cliente debe salir antes de liberar.");
    char linea[80];
    while (fgets(linea, sizeof linea, stdin) != NULL) {
        linea[strcspn(linea, "\r\n")] = '\0';
        if (strcmp(linea, "leer") == 0 || strcmp(linea, "escribir") == 0) {
            int rc = DSM_OK;
            if (strcmp(linea, "escribir") == 0) {
                rc = escribir(ctx, reserva, paginas / 2, pagina, "Adios TEC");
            }
            if (rc == DSM_OK) {
                rc = mostrar(ctx, reserva, paginas, pagina);
            }
            if (rc != DSM_OK) {
                return rc;
            }
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
                rc = escribir(ctx, reserva, p, (size_t)pagina, texto);
                if (rc != DSM_OK) {
                    break;
                }
            }
            printf("nodo %" PRIu64 " reserva %" PRIu32 " bytes %" PRIu64 " Ar %p\n",
                   id, reserva, paginas * pagina, ar);
            if (rc == DSM_OK) {
                rc = mostrar(ctx, reserva, (size_t)paginas, (size_t)pagina);
            }
            if (rc == DSM_OK) {
                rc = compartir(ctx, proceso, reserva, (size_t)paginas, (size_t)pagina,
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
