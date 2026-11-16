#include "dsm.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc > 2) {
        fprintf(stderr, "uso: %s [config]\n", argv[0]);
        return 1;
    }
    const char *config = argc == 2 ? argv[1] : "config/lab.ini";
    dsm_context_t *ctx;
    int rc = dsm_iniciar(config, 0, &ctx);
    if (rc != DSM_OK) {
        fprintf(stderr, "dsm_iniciar: %d\n", rc);
        return 1;
    }

    const char mensaje[] = "Hola TEC desde Ar";
    dsm_proc_id_t proceso;
    dsm_reserva_id_t reserva;
    void *ar;
    rc = dsm_proc_abrir(ctx, &proceso);
    if (rc == DSM_OK) {
        rc = dsm_reservar(ctx, proceso, sizeof mensaje, &reserva, &ar);
    }
    if (rc == DSM_OK) {
        rc = dsm_hilo_registrar(ctx, proceso);
    }
    if (rc == DSM_OK) {
        memcpy(ar, mensaje, sizeof mensaje);
        printf("reserva %" PRIu32 ": %s\n", reserva, (char *)ar);
        rc = dsm_hilo_desregistrar(ctx);
    }
    if (rc == DSM_OK) {
        rc = dsm_liberar(ctx, reserva);
    }

    int cierre = dsm_cerrar(ctx);
    if (rc != DSM_OK || cierre != DSM_OK) {
        fprintf(stderr, "DSM: operacion %d, cierre %d\n", rc, cierre);
        return 1;
    }
    return 0;
}
