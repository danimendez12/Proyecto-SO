#include "coherence.h"
#include "config.h"
#include "directory.h"
#include "pager.h"

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char saludo[] = "Hola TEC";
static const char despedida[] = "Adios TEC";

struct servicio {
    const dsm_config_t *cfg;
    uint16_t id;
    dsm_directorio_t directorio;
    dsm_coherencia_t *coherencia;
    dsm_pager_t *pager;
    dsm_pagina_desc_t pagina;
    dsm_vistas_t *vista;
    int registrado;
};

static int fallo(int error)
{
    errno = error;
    return -1;
}

static int interrumpido(const sigset_t *interrupcion)
{
    const struct timespec inmediato = {0};
    return sigtimedwait(interrupcion, NULL, &inmediato) == SIGINT;
}

/* La espera de un par sigue atendiendo SIGINT aunque aun no haya arrancado. */
static int esperar_par(struct servicio *s, uint16_t par, const sigset_t *interrupcion)
{
    while (!interrumpido(interrupcion)) {
        if (dsm_coherencia_esperar(s->coherencia, par) == 0) {
            return interrumpido(interrupcion) ? 1 : 0;
        }
        if (errno != ETIMEDOUT) {
            return -1;
        }
    }
    return 1;
}

static int preparar_vista(struct servicio *s, uint32_t reserva)
{
    if (dsm_coherencia_abrir(s->coherencia, reserva, &s->pagina, &s->vista) != 0) {
        return -1;
    }
    if (dsm_pager_iniciar_vacio(&s->pager, s->cfg->page_size, s->cfg->max_hilos,
                                s->id, s->cfg->rpc_timeout_ms, dsm_coherencia_resolver,
                                s->coherencia) != 0 ||
        dsm_coherencia_asociar(s->coherencia, s->pager, reserva, s->vista, NULL) != 0) {
        return -1;
    }
    if (dsm_pager_registrar(s->pager) != 0) {
        return -1;
    }
    s->registrado = 1;
    return 0;
}

static int leer_prototipo(struct servicio *s)
{
    const volatile char *ar = s->vista->a_r;
    for (int intento = 0; intento < 2; intento++) {
        char texto[sizeof saludo + 1] = {0};
        for (size_t i = 0; i < sizeof saludo; i++) {
            texto[i] = ar[i];
        }
        printf("nodo %u %s reserva %" PRIu32 " gpn %" PRIu64 " resultado %s\n",
               s->id, intento == 0 ? "lectura" : "segunda lectura",
               s->pagina.reserva, s->pagina.gpn, texto);
        if (memcmp(texto, saludo, sizeof saludo) != 0) {
            return fallo(EPROTO);
        }
        if (intento == 0) {
            for (size_t i = sizeof saludo; i < s->vista->bytes; i++) {
                if (ar[i] != 0) {
                    return fallo(EPROTO);
                }
            }
            printf("nodo %u imagen verificada %zu bytes\n", s->id, s->vista->bytes);
        }
    }
    return 0;
}

static char letra_msi(uint8_t msi)
{
    return msi == DSM_MSI_M ? 'M' : msi == DSM_MSI_S ? 'S' : 'I';
}

static int imprimir_estado(struct servicio *s)
{
    dsm_pagina_desc_t pagina;
    if (dsm_directorio_describir(&s->directorio, s->pagina.gpn, &pagina) != 0) {
        return -1;
    }
    uint8_t local = pagina.mascara_copias & (UINT64_C(1) << s->id) ? pagina.msi : DSM_MSI_I;
    printf("nodo %u estado %c version %" PRIu32 " poseedor %u mascara %" PRIu64 "\n",
           s->id, letra_msi(local), pagina.version, pagina.poseedor, pagina.mascara_copias);
    unsigned omitidos = 0;
    for (uint16_t id = 0; id < s->cfg->cantidad_nodos; id++) {
        dsm_stats_reply_t msg;
        if (dsm_coherencia_stats(s->coherencia, id, DSM_STATS_HOGAR, &msg) != 0) {
            int error = errno;
            if (error == ENOTCONN || error == ETIMEDOUT || error == ECONNRESET || error == EPIPE) {
                printf("nodo %u sin respuesta (%s), disponibles 0 nominal_bytes %" PRIu64 "\n",
                       id, strerror(error), s->cfg->nodos[id].memoria);
            } else {
                printf("nodo %u consulta fallida (%s)\n", id, strerror(error));
            }
            omitidos++;
            continue;
        }
        uint32_t libres = 0;
        for (uint16_t i = 0; i < msg.n; i++) {
            const dsm_ficha_t *f = &msg.fichas[i];
            char nombre[DSM_NOMBRE_MAX];
            uint32_t aportados = f->totales >= f->libres ? f->totales - f->libres : 0;
            libres = f->libres;
            memcpy(nombre, f->nombre, sizeof nombre - 1);
            nombre[sizeof nombre - 1] = '\0';
            printf("nodo %u ficha estado %u puerto %u aportados %u libres %u nombre %s nominal_bytes %" PRIu64 " total_paginas %u\n",
                   f->id, f->estado, f->puerto, aportados, f->libres, nombre, f->memoria, f->totales);
        }
        printf("nodo %u contadores lectura %" PRIu64 " escritura %" PRIu64
               " inv_env %" PRIu64 " inv_rec %" PRIu64 " pag_env %" PRIu64
               " pag_rec %" PRIu64 " bytes_env %" PRIu64 " bytes_rec %" PRIu64 "\n",
               msg.emisor, msg.contadores[DSM_CONTA_LECTURA], msg.contadores[DSM_CONTA_ESCRITURA],
               msg.contadores[DSM_CONTA_INV_ENV], msg.contadores[DSM_CONTA_INV_REC],
               msg.contadores[DSM_CONTA_PAG_ENV], msg.contadores[DSM_CONTA_PAG_REC],
               msg.contadores[DSM_CONTA_BYTES_ENV], msg.contadores[DSM_CONTA_BYTES_REC]);
        for (uint32_t i = 0; i < msg.p; i++) {
            const dsm_pagina_consulta_t *p = &msg.paginas[i];
            printf("pagina gpn %" PRIu64 " hogar %u poseedor %u msi %c reserva %" PRIu32
                   " orden %" PRIu32 " proceso %" PRIu32 " memoria_libre %u version %" PRIu32 " mascara %" PRIu64 " permiso %u\n",
                   p->gpn, p->hogar, p->poseedor, letra_msi(p->msi), p->reserva, p->orden,
                   p->proc, libres, p->version, p->mascara, p->permiso);
        }
        free(msg.paginas);
    }
    printf("consulta %s omitidos %u\n", omitidos ? "parcial" : "completa", omitidos);
    return 0;
}

static int ejecutar_comando(struct servicio *s, const char *comando)
{
    volatile unsigned char *ar = s->vista->a_r;
    if (strcmp(comando, "leer") == 0) {
        char texto[64] = {0};
        size_t i;
        for (i = 0; i < sizeof texto - 1; i++) {
            texto[i] = (char)ar[i];
            if (texto[i] == '\0') {
                break;
            }
        }
        if (i == sizeof texto - 1) {
            return fallo(EOVERFLOW);
        }
        printf("nodo %u lectura [%s]\n", s->id, texto);
    } else if (strcmp(comando, "escribir") == 0) {
        for (size_t i = 0; i < sizeof despedida; i++) {
            ar[i] = (unsigned char)despedida[i];
        }
        printf("nodo %u escritura [%s]\n", s->id, despedida);
    } else if (strcmp(comando, "marca") == 0) {
        ar[64 + s->id] = (unsigned char)(s->id + 1);
        printf("nodo %u marca aplicada\n", s->id);
    } else if (strcmp(comando, "marcas") == 0) {
        /* Cada acceso es una lectura de Ar y puede provocar un fallo real. */
        unsigned marcas[DSM_CONFIG_MAX_NODOS];
        for (size_t i = 0; i < s->cfg->cantidad_nodos; i++) {
            marcas[i] = ar[64 + i];
        }
        flockfile(stdout);
        printf("nodo %u marcas", s->id);
        for (size_t i = 0; i < s->cfg->cantidad_nodos; i++) {
            printf(" %u", marcas[i]);
        }
        putchar('\n');
        funlockfile(stdout);
    } else if (strcmp(comando, "estado") == 0) {
        return imprimir_estado(s);
    } else if (*comando != '\0') {
        fprintf(stderr, "comando desconocido (leer, escribir, marca, marcas, estado, salir)\n");
        return fallo(EINVAL);
    }
    return 0;
}

/* Lee una linea sin ocultar SIGINT tras una espera bloqueante en stdin. */
static int leer_comando(char *linea, size_t capacidad, const sigset_t *interrupcion)
{
    size_t usados = 0;
    while (!interrumpido(interrupcion)) {
        struct pollfd entrada = {.fd = STDIN_FILENO, .events = POLLIN};
        int listo = poll(&entrada, 1, 100);
        if (listo < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (listo == 0) {
            continue;
        }
        char letra;
        ssize_t leidos = read(STDIN_FILENO, &letra, 1);
        if (leidos == 0) {
            return 0;
        }
        if (leidos < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (letra == '\n') {
            linea[usados] = '\0';
            return 1;
        }
        if (letra == '\r') {
            continue;
        }
        if (usados + 1 == capacidad) {
            return fallo(EOVERFLOW);
        }
        linea[usados++] = letra;
    }
    return 0;
}

static int interactuar(struct servicio *s, const sigset_t *interrupcion)
{
    printf("nodo %u listo reserva %" PRIu32 " Ar %p\n", s->id, s->pagina.reserva, s->vista->a_r);
    char comando[80];
    int rc;
    while ((rc = leer_comando(comando, sizeof comando, interrupcion)) > 0) {
        if (strcmp(comando, "salir") == 0) {
            return 0;
        }
        if (ejecutar_comando(s, comando) != 0) {
            return -1;
        }
    }
    return rc;
}

static int servir(const dsm_config_t *cfg, const dsm_nodo_cfg_t *nodo,
                   uint32_t reserva, int interactivo)
{
    sigset_t interrupcion;
    sigemptyset(&interrupcion);
    sigaddset(&interrupcion, SIGINT);
    int error = pthread_sigmask(SIG_BLOCK, &interrupcion, NULL);
    struct sigaction accion = {.sa_handler = SIG_DFL};
    sigemptyset(&accion.sa_mask);
    if (error != 0 || sigaction(SIGINT, &accion, NULL) != 0) {
        fprintf(stderr, "no se pudo preparar SIGINT\n");
        return 1;
    }

    struct servicio s = {.cfg = cfg, .id = nodo->id};
    if (dsm_directorio_init(&s.directorio, cfg, nodo->id) != 0) {
        fprintf(stderr, "no se pudo iniciar el directorio del nodo %u\n", nodo->id);
        return 1;
    }
    int rc = 1;
    dsm_pagina_desc_t local;
    if (dsm_directorio_reservar_pagina(&s.directorio, &local) != 0) {
        fprintf(stderr, "no se pudo crear la reserva local del nodo %u\n", nodo->id);
        goto cerrar_directorio;
    }
    memcpy(local.ubicacion_local, saludo, sizeof saludo);
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (dsm_coherencia_iniciar(&s.coherencia, cfg, nodo->id, &s.directorio) != 0) {
        perror("iniciar coherencia");
        goto cerrar_directorio;
    }
    printf("nodo %u escuchando puerto %u reserva_local %" PRIu32 " gpn %" PRIu64 " MSI M\n",
           nodo->id, nodo->puerto, local.reserva, local.gpn);

    int espera = reserva == 0 ? 0 : esperar_par(&s, (uint16_t)(reserva >> 16), &interrupcion);
    if (espera > 0) {
        rc = 0;
        goto cerrar_coherencia;
    }
    if (espera < 0 || preparar_vista(&s, reserva == 0 ? local.reserva : reserva) != 0) {
        perror("abrir reserva");
        goto cerrar_paginador;
    }
    rc = 0;
    if (interactivo) {
        if (interactuar(&s, &interrupcion) != 0) {
            perror("interactivo");
            rc = 1;
        }
    } else {
        if (reserva != 0 && leer_prototipo(&s) != 0) {
            perror("lectura remota");
            rc = 1;
        }
        int signo;
        error = sigwait(&interrupcion, &signo);
        if (error != 0) {
            fprintf(stderr, "sigwait: %s\n", strerror(error));
            rc = 1;
        }
    }

cerrar_paginador:
    if (s.registrado && dsm_pager_desregistrar(s.pager) != 0) {
        perror("dsm_pager_desregistrar");
        return 1;
    }
    if (s.pager != NULL && dsm_pager_cerrar(s.pager) != 0) {
        perror("dsm_pager_cerrar");
        return 1;
    }
cerrar_coherencia:
    if (reserva != 0 && s.vista != NULL &&
        (dsm_coherencia_desvincular(s.coherencia, reserva) != 0 ||
         (dsm_directorio_desvincular(&s.directorio, reserva) != 0 && errno != ENOENT))) {
        perror("cierre de la vista remota al detener el nodo");
    }
    if (dsm_directorio_esperar_fin_aportes(&s.directorio) != 0) {
        perror("espera de los aportes al detener el nodo");
        rc = 1;
    }
    dsm_coherencia_cerrar(s.coherencia);
    printf("nodo %u servicio detenido\n", nodo->id);
cerrar_directorio:
    dsm_directorio_cerrar(&s.directorio);
    return rc;
}

static void uso(const char *argv0)
{
    fprintf(stderr, "uso: %s <config> <id-nodo> [servir|interactivo [id-reserva-remota]]\n", argv0);
}

int main(int argc, char **argv)
{
    if (argc < 3 || argc > 5 ||
        (argc >= 4 && strcmp(argv[3], "servir") != 0 && strcmp(argv[3], "interactivo") != 0)) {
        uso(argv[0]);
        return 1;
    }
    dsm_config_t cfg;
    if (dsm_config_cargar(argv[1], &cfg) != 0) {
        return 1;
    }
    errno = 0;
    char *fin = NULL;
    unsigned long id = strtoul(argv[2], &fin, 10);
    if (errno != 0 || argv[2][0] < '0' || argv[2][0] > '9' || *fin != '\0' || id > UINT16_MAX) {
        fprintf(stderr, "id de nodo invalido\n");
        return 1;
    }
    const dsm_nodo_cfg_t *nodo = dsm_config_nodo(&cfg, (uint16_t)id);
    if (nodo == NULL) {
        fprintf(stderr, "el nodo %lu no esta en la configuracion\n", id);
        return 1;
    }
    if (argc >= 4) {
        uint32_t reserva = 0;
        if (argc == 5) {
            errno = 0;
            unsigned long valor = strtoul(argv[4], &fin, 10);
            uint16_t creador = (uint16_t)(valor >> 16);
            if (errno || argv[4][0] < '0' || argv[4][0] > '9' || *fin != '\0' ||
                valor > UINT32_MAX || (valor & UINT16_MAX) == 0 || creador == nodo->id ||
                dsm_config_nodo(&cfg, creador) == NULL) {
                fprintf(stderr, "id de reserva remota invalido\n");
                return 1;
            }
            reserva = (uint32_t)valor;
        }
        return servir(&cfg, nodo, reserva, strcmp(argv[3], "interactivo") == 0);
    }
    printf("nodo %u\n", (unsigned)nodo->id);
    printf("puerto %u\n", (unsigned)nodo->puerto);
    printf("memoria %llu\n", (unsigned long long)nodo->memoria);
    return 0;
}
