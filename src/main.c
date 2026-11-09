#include "config.h"
#include "directory.h"
#include "dsm.h"
#include "network.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void uso(const char *argv0)
{
    fprintf(stderr, "uso: %s <config> <id-nodo> [unir]\n", argv0);
}

static int conectar_con_reintentos(uint32_t ip, uint16_t puerto, int *conn)
{
    for (int i = 0; i < 50; i++) {
        if (dsm_conectar(ip, puerto, conn) == 0) {
            return 0;
        }
        struct timespec pausa = {.tv_sec = 0, .tv_nsec = 100000000L};
        nanosleep(&pausa, NULL);
    }
    return -1;
}

static int prueba_dos_nodos(const dsm_config_t *cfg, const dsm_nodo_cfg_t *yo)
{
    if (cfg->cantidad_nodos != 2) {
        fprintf(stderr, "unir necesita exactamente dos nodos en la configuracion\n");
        return 1;
    }
    const dsm_nodo_cfg_t *par = dsm_config_nodo(cfg, yo->id == 0 ? 1 : 0);
    if (par == NULL) {
        fprintf(stderr, "no hay par para el nodo %u\n", yo->id);
        return 1;
    }

    int escucha = -1;
    int conn = -1;
    if (yo->id < par->id) {
        printf("conectando al nodo %u en el puerto %u\n", par->id, par->puerto);
        fflush(stdout);
        if (conectar_con_reintentos(par->ipv4_host, par->puerto, &conn) != 0) {
            fprintf(stderr, "no se pudo conectar al nodo %u\n", par->id);
            return 1;
        }
    } else {
        printf("esperando en el puerto %u\n", yo->puerto);
        fflush(stdout);
        if (dsm_escuchar(yo->ipv4_host, yo->puerto, &escucha) != 0 || dsm_aceptar(escucha, &conn) != 0) {
            fprintf(stderr, "no se pudo aceptar la conexion\n");
            if (escucha >= 0) {
                close(escucha);
            }
            return 1;
        }
        close(escucha);
    }

    dsm_alta_resultado_t alta;
    if (dsm_alta_par(conn, cfg, yo->id, par->id, &alta) != 0) {
        fprintf(stderr, "fallo el intercambio inicial\n");
        close(conn);
        return 1;
    }
    printf("par %u estado %u puerto %u memoria %llu\n", alta.par.id, alta.par.estado, alta.par.puerto,
           (unsigned long long)alta.par.memoria);
    fflush(stdout);

    dsm_directorio_t dir;
    if (dsm_directorio_init(&dir, cfg, yo->id) != 0) {
        close(conn);
        return 1;
    }
    uint32_t seq = alta.siguiente_msg;
    int rc = 1;
    if (yo->id == 0) {
        dsm_pagina_desc_t desc;
        if (dsm_directorio_reservar_pagina(&dir, &desc) == 0 &&
            dsm_directorio_publicar(&dir, conn, &seq) == 0) {
            printf("reserva %u gpn %llu reservas_globales %u\n", desc.reserva, (unsigned long long)desc.gpn,
                   dir.reservas_globales);
            rc = (dir.reservas_globales == 1) ? 0 : 1;
        } else {
            fprintf(stderr, "fallo al publicar la reserva\n");
        }
    } else {
        dsm_cabecera_t cab;
        dsm_region_info_t info;
        if (dsm_directorio_traer(&dir, conn, 1, &seq, &cab, &info) == 0 && dir.npaginas == 1) {
            printf("REGION_INFO tipo 0x%02x reserva %u gpn %llu reservas_globales %u\n", cab.tipo,
                   info.reserva_id, (unsigned long long)dir.paginas[0].gpn, dir.reservas_globales);
            rc = (cab.tipo == DSM_TIPO_REGION_INFO && dir.reservas_globales == 0) ? 0 : 1;
        } else {
            fprintf(stderr, "fallo al recibir REGION_INFO\n");
        }
    }
    fflush(stdout);
    dsm_directorio_cerrar(&dir);
    close(conn);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc != 3 && argc != 4) {
        uso(argv[0]);
        return 1;
    }
    int unir = 0;
    if (argc == 4) {
        if (strcmp(argv[3], "unir") != 0) {
            uso(argv[0]);
            return 1;
        }
        unir = 1;
    }
    dsm_config_t cfg;
    if (dsm_config_cargar(argv[1], &cfg) != 0) {
        return 1;
    }
    errno = 0;
    char *fin = NULL;
    unsigned long id = strtoul(argv[2], &fin, 10);
    if (errno != 0 || fin == argv[2] || *fin != '\0' || id > 65535ul) {
        fprintf(stderr, "id de nodo invalido\n");
        return 1;
    }
    const dsm_nodo_cfg_t *nodo = dsm_config_nodo(&cfg, (uint16_t)id);
    if (nodo == NULL) {
        fprintf(stderr, "el nodo %lu no esta en la configuracion\n", id);
        return 1;
    }
    printf("nodo %u\n", (unsigned)nodo->id);
    printf("puerto %u\n", (unsigned)nodo->puerto);
    printf("memoria %llu\n", (unsigned long long)cfg.memoria_inicial);
    fflush(stdout);
    if (!unir) {
        return 0;
    }
    return prueba_dos_nodos(&cfg, nodo);
}
