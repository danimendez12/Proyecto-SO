#include "config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
    V_PAGE = 0,
    V_MEM,
    V_MAX_NODOS,
    V_MAX_HILOS,
    V_HB_MS,
    V_HB_FALLOS,
    V_RPC,
    V_RANGO_INI,
    V_RANGO_FIN,
    V_COUNT
};

static void fallo(int linea, const char *texto)
{
    fprintf(stderr, "config:%d: %s\n", linea, texto);
}

static char *recortar(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r') {
        s++;
    }
    char *fin = s + strlen(s);
    while (fin > s && (fin[-1] == ' ' || fin[-1] == '\t' || fin[-1] == '\r' || fin[-1] == '\n')) {
        *--fin = '\0';
    }
    return s;
}

static int potencia_de_dos(uint32_t v)
{
    return v != 0 && (v & (v - 1u)) == 0;
}

static int parse_u64(const char *s, uint64_t *salida)
{
    if (s == NULL || *s == '\0') {
        return -1;
    }
    errno = 0;
    char *fin = NULL;
    unsigned long long v = strtoull(s, &fin, 10);
    if (errno != 0 || fin == s || *fin != '\0') {
        return -1;
    }
    *salida = (uint64_t)v;
    return 0;
}

static int asignar_u32(const char *valor, uint32_t *destino, uint64_t tope)
{
    uint64_t v = 0;
    if (parse_u64(valor, &v) != 0 || v > tope) {
        return -1;
    }
    *destino = (uint32_t)v;
    return 0;
}

static int asignar_u16(const char *valor, uint16_t *destino)
{
    uint32_t v = 0;
    if (asignar_u32(valor, &v, 65535u) != 0) {
        return -1;
    }
    *destino = (uint16_t)v;
    return 0;
}

static void predeterminados(dsm_config_t *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->page_size = 4096;
    cfg->memoria_inicial = 1048576;
    cfg->max_nodos = 8;
    cfg->max_hilos = 32;
    cfg->heartbeat_intervalo_ms = 1000;
    cfg->heartbeat_fallos = 3;
    cfg->rpc_timeout_ms = 2000;
}

static int validar(const dsm_config_t *cfg, const int presente[])
{
    long pagina = sysconf(_SC_PAGESIZE);
    if (pagina < 0) {
        fallo(0, "no se lee el tamano de pagina del sistema");
        return -1;
    }
    if (!potencia_de_dos(cfg->page_size) || cfg->page_size < (uint32_t)pagina ||
        cfg->page_size > 65536u) {
        fallo(0, "page_size fuera de rango");
        return -1;
    }
    if (cfg->memoria_inicial == 0 || cfg->memoria_inicial > 67108864ull ||
        (cfg->memoria_inicial % cfg->page_size) != 0) {
        fallo(0, "memoria_inicial fuera de rango");
        return -1;
    }
    if (cfg->max_nodos < 1 || cfg->max_nodos > DSM_CONFIG_MAX_NODOS) {
        fallo(0, "max_nodos fuera de rango");
        return -1;
    }
    if (cfg->max_hilos < 1 || cfg->max_hilos > 64) {
        fallo(0, "max_hilos fuera de rango");
        return -1;
    }
    if (cfg->heartbeat_intervalo_ms < 100 || cfg->heartbeat_intervalo_ms > 60000u) {
        fallo(0, "heartbeat_intervalo_ms fuera de rango");
        return -1;
    }
    if (cfg->heartbeat_fallos < 1 || cfg->heartbeat_fallos > 10) {
        fallo(0, "heartbeat_fallos fuera de rango");
        return -1;
    }
    if (cfg->rpc_timeout_ms < 100 || cfg->rpc_timeout_ms > 60000u) {
        fallo(0, "rpc_timeout_ms fuera de rango");
        return -1;
    }
    if (cfg->puerto_rango_inicio == 0 || cfg->puerto_rango_fin == 0 ||
        cfg->puerto_rango_inicio > cfg->puerto_rango_fin) {
        fallo(0, "rango de puertos invalido");
        return -1;
    }
    if (cfg->cantidad_nodos == 0 || cfg->cantidad_nodos > cfg->max_nodos) {
        fallo(0, "los identificadores de nodo no son densos desde 0");
        return -1;
    }
    for (uint16_t i = 0; i < cfg->cantidad_nodos; i++) {
        if (!presente[i] || cfg->nodos[i].id != i) {
            fallo(0, "los identificadores de nodo no son densos desde 0");
            return -1;
        }
        if (cfg->nodos[i].puerto == 0 ||
            cfg->nodos[i].puerto < cfg->puerto_rango_inicio ||
            cfg->nodos[i].puerto > cfg->puerto_rango_fin) {
            fallo(0, "puerto de nodo invalido");
            return -1;
        }
        if (cfg->nodos[i].nombre[0] == '\0') {
            fallo(0, "nombre de nodo vacio");
            return -1;
        }
        for (uint16_t j = 0; j < i; j++) {
            if (cfg->nodos[j].puerto == cfg->nodos[i].puerto) {
                fallo(0, "puerto de nodo repetido");
                return -1;
            }
        }
    }
    return 0;
}

static int leer_nodo(dsm_config_t *cfg, int *presente, const char *valor, int linea)
{
    unsigned id = 0;
    unsigned puerto = 0;
    char nombre[DSM_CONFIG_NOMBRE];
    char ip[80];
    int usado = 0;
    memset(nombre, 0, sizeof nombre);
    if (sscanf(valor, "%u %63s %79s %u %n", &id, nombre, ip, &puerto, &usado) != 4 ||
        valor[usado] != '\0') {
        fallo(linea, "nodo mal formado");
        return -1;
    }
    if (id >= DSM_CONFIG_MAX_NODOS || puerto > 65535u) {
        fallo(linea, "nodo fuera de rango");
        return -1;
    }
    if (presente[id]) {
        fallo(linea, "nodo repetido");
        return -1;
    }
    uint8_t octetos[4];
    if (inet_pton(AF_INET, ip, octetos) != 1) {
        fallo(linea, "IPv4 invalida");
        return -1;
    }
    cfg->nodos[id].id = (uint16_t)id;
    cfg->nodos[id].puerto = (uint16_t)puerto;
    cfg->nodos[id].ipv4_host = ((uint32_t)octetos[0] << 24) | ((uint32_t)octetos[1] << 16) |
                               ((uint32_t)octetos[2] << 8) | (uint32_t)octetos[3];
    memcpy(cfg->nodos[id].nombre, nombre, sizeof nombre);
    presente[id] = 1;
    if ((uint16_t)id + 1u > cfg->cantidad_nodos) {
        cfg->cantidad_nodos = (uint16_t)(id + 1u);
    }
    return 0;
}

int dsm_config_cargar(const char *ruta, dsm_config_t *salida)
{
    if (ruta == NULL || salida == NULL) {
        return -1;
    }
    dsm_config_t cfg;
    predeterminados(&cfg);
    int visto[V_COUNT];
    int presente[DSM_CONFIG_MAX_NODOS];
    memset(visto, 0, sizeof visto);
    memset(presente, 0, sizeof presente);

    FILE *archivo = fopen(ruta, "r");
    if (archivo == NULL) {
        fprintf(stderr, "config: no se abre %s\n", ruta);
        return -1;
    }

    char linea_buf[512];
    int numero = 0;
    while (fgets(linea_buf, sizeof linea_buf, archivo) != NULL) {
        numero++;
        if (strchr(linea_buf, '\n') == NULL && !feof(archivo)) {
            fallo(numero, "linea demasiado larga");
            fclose(archivo);
            return -1;
        }
        char *s = recortar(linea_buf);
        if (*s == '\0' || *s == '#') {
            continue;
        }
        char *igual = strchr(s, '=');
        if (igual == NULL) {
            fallo(numero, "se esperaba clave = valor");
            fclose(archivo);
            return -1;
        }
        *igual = '\0';
        char *clave = recortar(s);
        char *valor = recortar(igual + 1);
        int indice = -1;
        if (strcmp(clave, "page_size") == 0) {
            indice = V_PAGE;
        } else if (strcmp(clave, "memoria_inicial") == 0) {
            indice = V_MEM;
        } else if (strcmp(clave, "max_nodos") == 0) {
            indice = V_MAX_NODOS;
        } else if (strcmp(clave, "max_hilos") == 0) {
            indice = V_MAX_HILOS;
        } else if (strcmp(clave, "heartbeat_intervalo_ms") == 0) {
            indice = V_HB_MS;
        } else if (strcmp(clave, "heartbeat_fallos") == 0) {
            indice = V_HB_FALLOS;
        } else if (strcmp(clave, "rpc_timeout_ms") == 0) {
            indice = V_RPC;
        } else if (strcmp(clave, "puerto_rango_inicio") == 0) {
            indice = V_RANGO_INI;
        } else if (strcmp(clave, "puerto_rango_fin") == 0) {
            indice = V_RANGO_FIN;
        } else if (strcmp(clave, "nodo") == 0) {
            if (leer_nodo(&cfg, presente, valor, numero) != 0) {
                fclose(archivo);
                return -1;
            }
            continue;
        } else {
            fallo(numero, "clave desconocida");
            fclose(archivo);
            return -1;
        }
        if (visto[indice]) {
            fallo(numero, "clave repetida");
            fclose(archivo);
            return -1;
        }
        visto[indice] = 1;
        int rc = 0;
        if (indice == V_PAGE) {
            rc = asignar_u32(valor, &cfg.page_size, 65536u);
        } else if (indice == V_MEM) {
            rc = parse_u64(valor, &cfg.memoria_inicial);
        } else if (indice == V_MAX_NODOS) {
            rc = asignar_u16(valor, &cfg.max_nodos);
        } else if (indice == V_MAX_HILOS) {
            rc = asignar_u16(valor, &cfg.max_hilos);
        } else if (indice == V_HB_MS) {
            rc = asignar_u32(valor, &cfg.heartbeat_intervalo_ms, 60000u);
        } else if (indice == V_HB_FALLOS) {
            rc = asignar_u32(valor, &cfg.heartbeat_fallos, 10u);
        } else if (indice == V_RPC) {
            rc = asignar_u32(valor, &cfg.rpc_timeout_ms, 60000u);
        } else if (indice == V_RANGO_INI) {
            rc = asignar_u16(valor, &cfg.puerto_rango_inicio);
        } else if (indice == V_RANGO_FIN) {
            rc = asignar_u16(valor, &cfg.puerto_rango_fin);
        }
        if (rc != 0) {
            fallo(numero, "valor numerico invalido");
            fclose(archivo);
            return -1;
        }
    }
    fclose(archivo);
    if (validar(&cfg, presente) != 0) {
        return -1;
    }
    *salida = cfg;
    return 0;
}

const dsm_nodo_cfg_t *dsm_config_nodo(const dsm_config_t *cfg, uint16_t id)
{
    if (cfg == NULL || id >= cfg->cantidad_nodos) {
        return NULL;
    }
    if (cfg->nodos[id].id != id) {
        return NULL;
    }
    return &cfg->nodos[id];
}
