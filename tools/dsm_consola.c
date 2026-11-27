#include "config.h"
#include "dsm.h"
#include "consola_arranque.h"
#include "consola_consultas.h"
#include "consola_tareas.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { LINEA_MAX = 4096, LEER_MAX = 256 };
enum { ACTIVA, APERTURA_PENDIENTE, RETIRO_PENDIENTE };

struct reserva {
    uint32_t id;
    size_t bytes;
    void *base;
    int propia, estado;
    struct reserva *siguiente;
};

struct consola {
    dsm_context_t *ctx;
    dsm_config_t cfg;
    dsm_proc_id_t proceso;
    uint16_t nodo;
    struct reserva *reservas;
};

static int numero(const char *s, uint64_t maximo, uint64_t *valor)
{
    if (!s || *s < '0' || *s > '9') return DSM_ERR_INVAL;
    char *fin;
    errno = 0;
    uintmax_t n = strtoumax(s, &fin, 10);
    if (errno || *fin || n > maximo) return DSM_ERR_INVAL;
    *valor = (uint64_t)n; return DSM_OK;
}

static int tamano(const char *s, size_t *bytes)
{
    if (!s || *s < '0' || *s > '9') return DSM_ERR_INVAL;
    char *fin;
    errno = 0;
    uintmax_t n = strtoumax(s, &fin, 10), factor = 1;
    if (*fin == 'K' || *fin == 'k') { factor = 1024; fin++; }
    else if (*fin == 'M' || *fin == 'm') { factor = 1024 * 1024; fin++; }
    if (errno || *fin || !n || n > SIZE_MAX / factor) return DSM_ERR_INVAL;
    *bytes = (size_t)(n * factor); return DSM_OK;
}

static char *token(char **resto)
{
    char *s = *resto;
    while (isspace((unsigned char)*s)) s++;
    if (!*s) { *resto = s; return NULL; }
    char *inicio = s;
    while (*s && !isspace((unsigned char)*s)) s++;
    if (*s) *s++ = 0;
    *resto = s; return inicio;
}

static struct reserva **posicion(struct consola *c, uint32_t id)
{
    struct reserva **p = &c->reservas;
    while (*p && (*p)->id != id) p = &(*p)->siguiente;
    return p;
}

static int identificador(struct consola *c, const char *s, uint32_t *id)
{
    uint64_t n;
    if (numero(s, UINT32_MAX, &n) != DSM_OK || (n & UINT16_MAX) == 0 ||
        (n >> 16) >= c->cfg.cantidad_nodos) return DSM_ERR_INVAL;
    *id = (uint32_t)n; return DSM_OK;
}

static int buscar(struct consola *c, const char *s, struct reserva **r)
{
    uint32_t id;
    if (identificador(c, s, &id) != DSM_OK) return DSM_ERR_INVAL;
    *r = *posicion(c, id);
    if (!*r) return DSM_ERR_INVAL;
    return (*r)->estado == ACTIVA ? DSM_OK : DSM_ERR_OCUPADA;
}

static int refrescar(struct consola *c, struct reserva *r)
{
    dsm_reserva_estado_t estado;
    int rc = dsm_consultar_reserva(c->ctx, r->id, &estado);
    if (rc == DSM_OK) {
        r->bytes = estado.bytes;
        r->estado = estado.retirando ? RETIRO_PENDIENTE :
                    estado.provisional || !estado.base || !estado.bytes ? APERTURA_PENDIENTE : ACTIVA;
        r->base = r->estado == ACTIVA ? estado.base : NULL;
    } else { r->base = NULL; if (r->estado != RETIRO_PENDIENTE) r->estado = APERTURA_PENDIENTE; }
    return rc;
}

static int retirar(struct consola *c, struct reserva **p, int propia)
{
    struct reserva *r = *p;
    if (!r || r->propia != propia) return DSM_ERR_ESTADO;
    /* El API puede desmontar parte de una vista y fallar despues. Nunca se
     * vuelve a prestar el puntero, pero el ID queda visible para reintentar. */
    r->estado = RETIRO_PENDIENTE; r->base = NULL;
    uint32_t id = r->id;
    int rc = propia ? dsm_liberar(c->ctx, id) : dsm_desvincular_reserva(c->ctx, id);
    int existe = rc == DSM_OK ? DSM_ERR_INVAL : refrescar(c, r);
    if (rc == DSM_OK || existe == DSM_ERR_INVAL) {
        *p = r->siguiente; free(r);
        printf("%s OK id=%" PRIu32 "\n", propia ? "LIBERAR" : "DESVINCULAR", id);
        return DSM_OK;
    }
    printf("RETIRO %s id=%" PRIu32 " rc=%d reintento=permitido\n",
           existe == DSM_OK && r->estado == ACTIVA ? "REINTENTABLE" : "PENDIENTE", id, rc);
    return rc;
}

static int cerrar(struct consola *c)
{
    struct reserva **p = &c->reservas;
    int pendiente = DSM_OK;
    while (*p) {
        int otro = retirar(c, p, (*p)->propia);
        if (otro != DSM_OK) { if (pendiente == DSM_OK) pendiente = otro; p = &(*p)->siguiente; }
    }
    /* El cierre global solo se intenta sin IDs conocidos: no puede consumir
     * un prefijo y dejar entradas CLI que ya no correspondan a una vista. */
    if (c->reservas) {
        printf("CIERRE PENDIENTE nodo=%u rc=%d\n", c->nodo, pendiente);
        return pendiente;
    }
    int rc = dsm_cerrar(c->ctx);
    if (rc != DSM_OK) { printf("CIERRE PENDIENTE nodo=%u rc=%d\n", c->nodo, rc); return rc; }
    c->ctx = NULL;
    while (c->reservas) { struct reserva *r = c->reservas; c->reservas = r->siguiente; free(r); }
    printf("CIERRE OK nodo=%u\n", c->nodo);
    return DSM_OK;
}

static void ayuda(void)
{
    puts("ayuda | reservar TAMANO | reservas | abrir ID | leer ID OFFSET BYTES (maximo 256)");
    puts("escribir ID OFFSET TEXTO (bytes literales, sin terminador NUL) | llenar ID VALOR | verificar ID VALOR");
    puts("mapa ID | liberar ID | desvincular ID | tarea patron ID HILOS VALOR | salir");
    puts("estado | local | nodos | memoria | estadisticas [nodo ID]");
    puts("paginas local|global|nodo ID [DESDE [LIMITE]] | copias local [DESDE [LIMITE]]");
    puts("TAMANO: bytes, K=1024 bytes (KiB), M=1048576 bytes (MiB). VALOR: 0..255.");
    puts("La tarea es sincrona, escribe y verifica un patron por offset; conserva la reserva y el contexto.");
}

static int mapa(struct consola *c, const struct reserva *r)
{
    size_t paginas = 1 + (r->bytes - 1) / c->cfg.page_size, inicio = 0;
    uint16_t previo = UINT16_MAX;
    uint64_t base = 0, ultimo = 0;
    printf("MAPA INICIO id=%" PRIu32 " bytes=%zu paginas=%zu\n", r->id, r->bytes, paginas);
    for (size_t i = 0; i <= paginas; i++) {
        uint16_t hogar = UINT16_MAX;
        uint64_t gpn = 0;
        if (i < paginas) {
            int rc = dsm_ubicacion_pagina(c->ctx, r->id, i, &hogar, &gpn);
            if (rc != DSM_OK) return rc;
        }
        if (i && (i == paginas || hogar != previo || gpn != ultimo + 1u)) {
            printf("MAPA TRAMO id=%" PRIu32 " orden=%zu paginas=%zu hogar=%u gpn=%" PRIu64 "\n",
                   r->id, inicio, i - inicio, previo, base);
            inicio = i; base = gpn;
        } else if (!i) base = gpn;
        previo = hogar; ultimo = gpn;
    }
    printf("MAPA FIN id=%" PRIu32 " paginas=%zu\n", r->id, paginas);
    return DSM_OK;
}

static int llenar_verificar(struct consola *c, struct reserva *r, uint8_t valor, int verificar)
{
    unsigned char buffer[4096];
    memset(buffer, valor, sizeof buffer);
    for (size_t offset = 0; offset < r->bytes;) {
        size_t n = r->bytes - offset < sizeof buffer ? r->bytes - offset : sizeof buffer;
        int rc = verificar ? dsm_leer(c->ctx, r->id, offset, buffer, n) :
                             dsm_escribir(c->ctx, r->id, offset, buffer, n);
        if (rc != DSM_OK) { printf("OPERACION PARCIAL id=%" PRIu32 " bytes_completados=%zu rc=%d\n", r->id, offset, rc); return rc; }
        if (verificar) for (size_t i = 0; i < n; i++) if (buffer[i] != valor) {
            printf("VERIFICAR ERROR id=%" PRIu32 " offset=%zu actual=%u esperado=%u\n",
                   r->id, offset + i, (unsigned)buffer[i], (unsigned)valor);
            return DSM_ERR_ESTADO;
        }
        offset += n;
    }
    printf("%s OK id=%" PRIu32 " bytes=%zu valor=%u\n", verificar ? "VERIFICAR" : "LLENAR", r->id, r->bytes, (unsigned)valor);
    return DSM_OK;
}

static int ejecutar(struct consola *c, char *linea)
{
    char *resto = linea, *argv[16];
    int argc = 0;
    argv[argc] = token(&resto);
    if (!argv[argc]) return DSM_OK;
    argc++;
    if (strcmp(argv[0], "escribir") == 0) {
        char *id = token(&resto), *offset = token(&resto);
        while (isspace((unsigned char)*resto)) resto++;
        struct reserva *r;
        uint64_t o;
        int encontrado = buscar(c, id, &r);
        if (encontrado != DSM_OK) return encontrado;
        if (numero(offset, SIZE_MAX, &o) != DSM_OK || !*resto || o > r->bytes || strlen(resto) > r->bytes - (size_t)o)
            return DSM_ERR_INVAL;
        size_t n = strlen(resto);
        int rc = dsm_escribir(c->ctx, r->id, (size_t)o, resto, n);
        if (rc == DSM_OK) printf("ESCRIBIR OK id=%" PRIu32 " offset=%zu bytes=%zu\n", r->id, (size_t)o, n);
        return rc;
    }
    while (argc < 16 && (argv[argc] = token(&resto)) != NULL) argc++;
    if (argc == 16 && token(&resto) != NULL) return DSM_ERR_INVAL;
    if (!strcmp(argv[0], "ayuda") && argc == 1) { ayuda(); return DSM_OK; }
    if (!strcmp(argv[0], "salir") && argc == 1) return cerrar(c);
    if (!strcmp(argv[0], "reservas") && argc == 1) {
        size_t n = 0;
        for (struct reserva *r = c->reservas; r; r = r->siguiente) n++;
        printf("RESERVAS INICIO cantidad=%zu\n", n);
        for (struct reserva *r = c->reservas; r; r = r->siguiente)
            printf("RESERVA id=%" PRIu32 " origen=%s estado=%s bytes=%zu\n", r->id,
                   r->propia ? "propia" : "remota", r->estado == ACTIVA ? "activa" :
                   r->estado == APERTURA_PENDIENTE ? "apertura_pendiente" : "retiro_pendiente", r->bytes);
        printf("RESERVAS FIN cantidad=%zu\n", n); return DSM_OK;
    }
    if (!strcmp(argv[0], "reservar") && argc == 2) {
        size_t bytes;
        if (tamano(argv[1], &bytes) != DSM_OK) return DSM_ERR_INVAL;
        struct reserva *r = calloc(1, sizeof *r);
        if (!r) return DSM_ERR_NOMEM;
        int rc = dsm_reservar(c->ctx, c->proceso, bytes, &r->id, &r->base);
        if (rc != DSM_OK) { free(r); return rc; }
        r->bytes = bytes; r->propia = 1; r->estado = ACTIVA;
        r->siguiente = c->reservas; c->reservas = r;
        printf("RESERVA OK id=%" PRIu32 " bytes=%zu paginas=%zu creador=%u propietario=%" PRIu32 "\n",
               r->id, bytes, 1 + (bytes - 1) / c->cfg.page_size, c->nodo, c->proceso);
        return DSM_OK;
    }
    if (!strcmp(argv[0], "abrir") && argc == 2) {
        uint32_t id;
        if (identificador(c, argv[1], &id) != DSM_OK) return DSM_ERR_INVAL;
        struct reserva **p = posicion(c, id), *r = *p;
        if (r && r->estado == RETIRO_PENDIENTE) return DSM_ERR_OCUPADA;
        int nueva = r == NULL;
        if (nueva) { r = calloc(1, sizeof *r); if (!r) return DSM_ERR_NOMEM; r->id = id; r->propia = (id >> 16) == c->nodo; }
        int rc = dsm_abrir_reserva(c->ctx, id, &r->base, &r->bytes);
        if (rc != DSM_OK) {
            int existe = refrescar(c, r);
            if (existe == DSM_ERR_INVAL) {
                if (!nueva) *p = r->siguiente;
                free(r);
                printf("ABRIR ERROR id=%" PRIu32 " rc=%d vinculacion=no\n", id, rc);
                return rc;
            }
        }
        if (nueva) *p = r;
        if (rc == DSM_OK) {
            r->estado = ACTIVA;
            printf("ABRIR OK id=%" PRIu32 " bytes=%zu paginas=%zu\n", id, r->bytes, 1 + (r->bytes - 1) / c->cfg.page_size);
        } else printf("APERTURA %s id=%" PRIu32 " rc=%d\n", r->estado == ACTIVA ? "CONSERVADA" : "PENDIENTE", id, rc);
        return rc;
    }
    if ((!strcmp(argv[0], "liberar") || !strcmp(argv[0], "desvincular")) && argc == 2) {
        uint32_t id;
        if (identificador(c, argv[1], &id) != DSM_OK) return DSM_ERR_INVAL;
        struct reserva **p = posicion(c, id);
        if (!*p) return DSM_ERR_INVAL;
        return retirar(c, p, !strcmp(argv[0], "liberar"));
    }
    if (!strcmp(argv[0], "leer") && argc == 4) {
        struct reserva *r;
        int rc = buscar(c, argv[1], &r);
        uint64_t o, n;
        if (rc != DSM_OK) return rc;
        if (numero(argv[2], SIZE_MAX, &o) != DSM_OK || numero(argv[3], SIZE_MAX, &n) != DSM_OK ||
            o > r->bytes || n > r->bytes - (size_t)o) return DSM_ERR_INVAL;
        if (n > LEER_MAX) { printf("LECTURA LIMITE maximo=%u solicitado=%" PRIu64 "\n", LEER_MAX, n); return DSM_ERR_INVAL; }
        unsigned char buffer[LEER_MAX];
        rc = dsm_leer(c->ctx, r->id, (size_t)o, buffer, (size_t)n);
        if (rc != DSM_OK) return rc;
        flockfile(stdout);
        printf("LEER OK id=%" PRIu32 " offset=%zu bytes=%zu hex=", r->id, (size_t)o, (size_t)n);
        for (size_t i = 0; i < n; i++) printf("%02x", buffer[i]);
        printf(" texto=\"");
        for (size_t i = 0; i < n; i++) {
            if (buffer[i] == '"' || buffer[i] == '\\') printf("\\%c", buffer[i]);
            else if (buffer[i] >= 32 && buffer[i] <= 126) putchar(buffer[i]);
            else printf("\\x%02x", buffer[i]);
        }
        puts("\""); funlockfile(stdout); return DSM_OK;
    }
    if ((!strcmp(argv[0], "llenar") || !strcmp(argv[0], "verificar")) && argc == 3) {
        struct reserva *r;
        int rc = buscar(c, argv[1], &r);
        uint64_t valor;
        if (rc != DSM_OK) return rc;
        if (numero(argv[2], 255, &valor) != DSM_OK) return DSM_ERR_INVAL;
        return llenar_verificar(c, r, (uint8_t)valor, !strcmp(argv[0], "verificar"));
    }
    if (!strcmp(argv[0], "mapa") && argc == 2) {
        struct reserva *r; int rc = buscar(c, argv[1], &r);
        return rc == DSM_OK ? mapa(c, r) : rc;
    }
    if (!strcmp(argv[0], "tarea") && argc == 5 && !strcmp(argv[1], "patron")) {
        struct reserva *r;
        int rc = buscar(c, argv[2], &r);
        uint64_t hilos, valor;
        if (rc != DSM_OK) return rc;
        if (numero(argv[3], c->cfg.max_hilos, &hilos) != DSM_OK || !hilos || numero(argv[4], 255, &valor) != DSM_OK)
            return DSM_ERR_INVAL;
        return consola_tarea_patron(c->ctx, c->proceso, r->id, r->bytes, c->cfg.page_size,
                                    (size_t)hilos, (uint8_t)valor, 0, stdout);
    }
    int consulta = consola_consultar(c->ctx, &c->cfg, c->nodo, argc, argv, stdout);
    return consulta > 0 ? DSM_OK : consulta < 0 ? consulta : DSM_ERR_INVAL;
}

static int interrupcion(const sigset_t *senales)
{
    const struct timespec cero = {0};
    int n = sigtimedwait(senales, NULL, &cero);
    return n == SIGINT || n == SIGTERM;
}

/* -2: linea demasiado larga; -3: señal; -4: NUL; 0: EOF; 1: linea. */
static int leer_linea(char linea[LINEA_MAX], const sigset_t *senales)
{
    size_t n = 0;
    int larga = 0;
    for (;;) {
        if (interrupcion(senales)) return -3;
        struct pollfd p = {.fd = STDIN_FILENO, .events = POLLIN};
        int rc = poll(&p, 1, 100);
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0) return -1;
        if (!rc) continue;
        char c;
        ssize_t leidos = read(STDIN_FILENO, &c, 1);
        if (leidos < 0 && errno == EINTR) continue;
        if (leidos < 0) return -1;
        if (!leidos || c == '\n') { linea[n] = 0; return larga == 2 ? -4 : larga ? -2 : leidos || n ? 1 : 0; }
        if (c == '\0') { larga = 2; continue; }
        if (c == '\r') continue;
        if (n + 1 < LINEA_MAX) linea[n++] = c; else if (larga != 2) larga = 1;
    }
}

static int notificar(int fd, const struct consola *c, int resultado)
{
    if (fd < 0) return 0;
    consola_alta_t alta = {.magic = CONSOLA_ALTA_MAGIC, .resultado = resultado,
        .pid = (int32_t)getpid(), .nodo = c->nodo,
        .puerto = c->nodo < c->cfg.cantidad_nodos ? c->cfg.nodos[c->nodo].puerto : 0};
    const unsigned char *p = (const unsigned char *)&alta;
    size_t faltan = sizeof alta;
    while (faltan) {
        ssize_t n = write(fd, p, faltan);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); return -1; }
        p += n; faltan -= (size_t)n;
    }
    close(fd); return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    int servir = 0, alta_fd = -1;
    if (argc < 2) { fprintf(stderr, "uso: %s CONFIG [--servir]\n", argv[0]); return 1; }
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--servir") && !servir) servir = 1;
        else if (!strcmp(argv[i], "--alta-fd") && alta_fd == -1 && ++i < argc) {
            uint64_t fd;
            if (numero(argv[i], INT_MAX, &fd) != DSM_OK || fd < 3 || fcntl((int)fd, F_GETFD) < 0) return 1;
            alta_fd = (int)fd;
        } else return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    sigset_t senales;
    sigemptyset(&senales); sigaddset(&senales, SIGINT); sigaddset(&senales, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &senales, NULL) != 0) return 1;
    struct consola c = {.nodo = UINT16_MAX};
    int rc = dsm_config_cargar(argv[1], &c.cfg) == 0 ? DSM_OK : DSM_ERR_CONFIG;
    if (rc == DSM_OK) rc = dsm_pool_reclamar(argv[1], &c.nodo);
    if (rc == DSM_OK) rc = dsm_iniciar(argv[1], c.nodo, &c.ctx);
    if (rc == DSM_OK) rc = dsm_proc_abrir(c.ctx, &c.proceso);
    if (rc == DSM_OK) printf("ALTA OK pid=%ld nodo=%u puerto=%u\n", (long)getpid(), c.nodo, c.cfg.nodos[c.nodo].puerto);
    else fprintf(stderr, "ALTA ERROR pid=%ld nodo=%u rc=%d\n", (long)getpid(), c.nodo, rc);
    if (notificar(alta_fd, &c, rc) != 0 && rc == DSM_OK) rc = DSM_ERR_ESTADO;
    if (rc != DSM_OK) {
        if (c.ctx) (void)dsm_cerrar(c.ctx); else dsm_pool_liberar();
        return 1;
    }
    uint32_t ip = c.cfg.nodos[c.nodo].ipv4_host;
    printf("DSM CONSOLA nodo=%u pid=%ld IPv4=%u.%u.%u.%u puerto=%u memoria_bytes=%" PRIu64 " pagina=%u proceso=%" PRIu32 "\n",
           c.nodo, (long)getpid(), (ip >> 24) & 255u, (ip >> 16) & 255u, (ip >> 8) & 255u, ip & 255u,
           c.cfg.nodos[c.nodo].puerto, c.cfg.nodos[c.nodo].memoria, c.cfg.page_size, c.proceso);
    puts("Escriba ayuda para ver los comandos.");
    int automatico = 0, fallo_entrada = 0;
    while (c.ctx) {
        if (servir || automatico) {
            if (!automatico && !interrupcion(&senales)) { const struct timespec t = {.tv_nsec = 100000000}; nanosleep(&t, NULL); continue; }
            automatico = 1;
            rc = cerrar(&c);
            if (rc != DSM_OK) { const struct timespec t = {.tv_nsec = 500000000}; nanosleep(&t, NULL); }
            continue;
        }
        printf("DSM[%u]>%s", c.nodo, isatty(STDIN_FILENO) ? " " : "\n"); fflush(stdout);
        char linea[LINEA_MAX];
        int lectura = leer_linea(linea, &senales);
        if (lectura == 0 || lectura == -3) { automatico = 1; continue; }
        rc = lectura > 0 ? ejecutar(&c, linea) : DSM_ERR_INVAL;
        if (rc != DSM_OK) printf("ERROR comando rc=%d%s\n", rc, lectura == -2 ?
            " linea demasiado larga (maximo 4095 bytes)" : lectura == -4 ? " byte NUL no permitido" : "");
        printf("COMANDO FIN rc=%d\n", rc);
        if (lectura == -1) { automatico = 1; fallo_entrada = 1; }
    }
    return fallo_entrada;
}
