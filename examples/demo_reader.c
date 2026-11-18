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

static void mostrar(const void *base, size_t bytes, size_t pagina)
{
    const volatile unsigned char *ar = base;
    for (size_t p = 0; p < bytes / pagina; p++) {
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

static void escribir(void *base, size_t bytes, size_t pagina)
{
    const char texto[] = "Adios TEC";
    volatile unsigned char *ar = base;
    size_t inicio = (bytes / pagina / 2) * pagina;
    for (size_t i = 0; i < sizeof texto; i++) {
        ar[inicio + i] = (unsigned char)texto[i];
    }
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    uint64_t nodo, reserva, pagina = 4096;
    if (argc < 4 || argc > 5 || numero(argv[2], UINT16_MAX, &nodo) != 0 ||
        numero(argv[3], UINT32_MAX, &reserva) != 0 || reserva == 0 ||
        (argc == 5 && (numero(argv[4], SIZE_MAX, &pagina) != 0 || pagina < 64))) {
        fprintf(stderr, "uso: %s CONFIG NODO RESERVA [PAGE_SIZE]\n", argv[0]);
        return 1;
    }
    dsm_context_t *ctx;
    int rc = dsm_iniciar(argv[1], (dsm_node_id_t)nodo, &ctx);
    if (rc != DSM_OK) {
        fprintf(stderr, "dsm_iniciar: %d\n", rc);
        return 1;
    }
    dsm_proc_id_t proceso;
    void *ar;
    size_t bytes;
    int registrada = 0, abierta = 0;
    rc = dsm_proc_abrir(ctx, &proceso);
    if (rc == DSM_OK) {
        rc = dsm_hilo_registrar(ctx, proceso);
        registrada = rc == DSM_OK;
    }
    if (rc == DSM_OK) {
        rc = dsm_abrir_reserva(ctx, (dsm_reserva_id_t)reserva, &ar, &bytes);
        abierta = rc == DSM_OK;
    }
    if (rc == DSM_OK && (bytes == 0 || bytes % (size_t)pagina != 0)) {
        fputs("El tamano abierto debe ser multiplo del PAGE_SIZE indicado.\n", stderr);
        rc = DSM_ERR_INVAL;
    }
    if (rc == DSM_OK) {
        printf("nodo %" PRIu64 " reserva %" PRIu64 " bytes %zu Ar %p\n", nodo, reserva, bytes, ar);
        mostrar(ar, bytes, (size_t)pagina);
        puts("Comandos: leer, escribir (pagina central), salir.");
        char linea[80];
        while (fgets(linea, sizeof linea, stdin) != NULL) {
            linea[strcspn(linea, "\r\n")] = '\0';
            if (strcmp(linea, "salir") == 0) {
                break;
            }
            if (strcmp(linea, "escribir") == 0) {
                escribir(ar, bytes, (size_t)pagina);
                mostrar(ar, bytes, (size_t)pagina);
            } else if (strcmp(linea, "leer") == 0) {
                mostrar(ar, bytes, (size_t)pagina);
            } else if (*linea != '\0') {
                puts("Comandos: leer, escribir, salir.");
            }
        }
        if (ferror(stdin)) {
            rc = DSM_ERR_ESTADO;
        }
    }
    if (registrada) {
        int desregistro = dsm_hilo_desregistrar(ctx);
        if (rc == DSM_OK) {
            rc = desregistro;
        }
    }
    if (abierta) {
        int desvincular = dsm_desvincular_reserva(ctx, (dsm_reserva_id_t)reserva);
        if (rc == DSM_OK) {
            rc = desvincular;
        }
    }
    int cierre = dsm_cerrar(ctx);
    if (rc != DSM_OK || cierre != DSM_OK) {
        fprintf(stderr, "DSM: operacion %d, cierre %d\n", rc, cierre);
        return 1;
    }
    return 0;
}
