#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { PUERTO_BASE = 27200, MAX_PROCESOS = 3 };

struct proceso {
    unsigned id;
    pid_t pid;
    FILE *salida;
    char texto[8192];
};

static int64_t ahora_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        exit(1);
    }
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void pausa(void)
{
    const struct timespec espera = {.tv_nsec = 20000000};
    nanosleep(&espera, NULL);
}

static int puerto_disponible(unsigned id)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    int uno = 1;
    struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)(PUERTO_BASE + id)),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    int rc = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &uno, sizeof uno);
    if (rc == 0) {
        rc = bind(fd, (const struct sockaddr *)&local, sizeof local);
    }
    int error = errno;
    close(fd);
    errno = error;
    return rc;
}

static int iniciar(struct proceso *p, const char *config, uint32_t reserva)
{
    p->salida = tmpfile();
    if (p->salida == NULL) {
        return -1;
    }
    int fd = fileno(p->salida);
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        return -1;
    }
    char id[16], reserva_texto[16];
    snprintf(id, sizeof id, "%u", p->id);
    snprintf(reserva_texto, sizeof reserva_texto, "%" PRIu32, reserva);
    p->pid = fork();
    if (p->pid < 0) {
        return -1;
    }
    if (p->pid == 0) {
        if (dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) {
            _exit(126);
        }
        if (reserva != 0) {
            execl("./bin/dsm_node", "dsm_node", config, id, "servir", reserva_texto, (char *)NULL);
        } else {
            execl("./bin/dsm_node", "dsm_node", config, id, "servir", (char *)NULL);
        }
        _exit(127);
    }
    return 0;
}

static int leer_log(struct proceso *p)
{
    ssize_t n = pread(fileno(p->salida), p->texto, sizeof p->texto - 1, 0);
    if (n < 0) {
        return -1;
    }
    p->texto[n] = '\0';
    return 0;
}

static int esperar_log(struct proceso *p, const char *texto, int64_t limite)
{
    do {
        if (leer_log(p) == 0 && strstr(p->texto, texto) != NULL) {
            return 0;
        }
        int estado;
        pid_t terminado = waitpid(p->pid, &estado, WNOHANG);
        if (terminado == p->pid) {
            p->pid = 0;
            break;
        }
        pausa();
    } while (ahora_ms() < limite);
    fprintf(stderr, "nodo %u no mostro: %s\n", p->id, texto);
    return -1;
}

static int contar(const char *texto, const char *frase)
{
    int n = 0;
    while ((texto = strstr(texto, frase)) != NULL) {
        n++;
        texto += strlen(frase);
    }
    return n;
}

static int detener(struct proceso *p, size_t n)
{
    int resultado = 0;
    for (size_t i = 0; i < n; i++) {
        if (p[i].pid > 0 && kill(p[i].pid, SIGINT) != 0) {
            resultado = -1;
        }
    }
    int64_t limite = ahora_ms() + 3500;
    for (size_t i = 0; i < n; i++) {
        if (p[i].pid <= 0) {
            continue;
        }
        pid_t pid = p[i].pid;
        pid_t rc;
        int estado = 0;
        do {
            do {
                rc = waitpid(pid, &estado, WNOHANG);
            } while (rc < 0 && errno == EINTR);
            if (rc == 0) {
                pausa();
            }
        } while (rc == 0 && ahora_ms() < limite);
        if (rc == 0) {
            kill(pid, SIGKILL);
            while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
            }
            resultado = -1;
        } else if (rc != pid || !WIFEXITED(estado) || WEXITSTATUS(estado) != 0 ||
                   leer_log(&p[i]) != 0 || strstr(p[i].texto, "servicio detenido") == NULL) {
            resultado = -1;
        }
        p[i].pid = 0;
    }
    return resultado;
}

static int verificar_permisos_hogar(pid_t pid)
{
    char ruta[64];
    snprintf(ruta, sizeof ruta, "/proc/%ld/maps", (long)pid);
    FILE *mapas = fopen(ruta, "r");
    if (mapas == NULL) {
        return -1;
    }
    int solo_lectura = 0, escribible = 0;
    char linea[512];
    while (fgets(linea, sizeof linea, mapas) != NULL) {
        char permisos[5];
        if (strstr(linea, "/memfd:dsm-reserva") != NULL &&
            sscanf(linea, "%*s %4s", permisos) == 1) {
            solo_lectura += strcmp(permisos, "r--s") == 0;
            escribible += strcmp(permisos, "rw-s") == 0;
        }
    }
    fclose(mapas);
    if (solo_lectura != 1 || escribible != 1) {
        fprintf(stderr, "se esperaba Ar de solo lectura y Cr escribible en el hogar\n");
        return -1;
    }
    return 0;
}

static int verificar_trazas(struct proceso *lector, struct proceso *hogar,
                             uint32_t reserva, uint64_t gpn, int64_t limite)
{
    char linea[160];
    snprintf(linea, sizeof linea, "reserva_remota %" PRIu32 " gpn %" PRIu64 " hogar %u MSI I",
             reserva, gpn, hogar->id);
    if (esperar_log(lector, linea, limite) != 0) {
        return -1;
    }
    snprintf(linea, sizeof linea, "PAGE_REQUEST enviado hogar %u bytes 48", hogar->id);
    if (esperar_log(lector, linea, limite) != 0) {
        return -1;
    }
    snprintf(linea, sizeof linea, "PAGE_REQUEST recibido par %u gpn %" PRIu64 " bytes 48",
             lector->id, gpn);
    if (esperar_log(hogar, linea, limite) != 0) {
        return -1;
    }
    snprintf(linea, sizeof linea, "PAGE_DATA enviado par %u reserva %" PRIu32 " gpn %" PRIu64
                                  " bytes 4096 trama 4176", lector->id, reserva, gpn);
    if (esperar_log(hogar, linea, limite) != 0) {
        return -1;
    }
    snprintf(linea, sizeof linea, "PAGE_DATA recibido hogar %u reserva %" PRIu32 " gpn %" PRIu64
                                  " bytes 4096 trama 4176", hogar->id, reserva, gpn);
    if (esperar_log(lector, linea, limite) != 0 ||
        esperar_log(hogar, "MSI M -> S version 1", limite) != 0 ||
        esperar_log(lector, "MSI I -> S version 1", limite) != 0 ||
        esperar_log(lector, "imagen verificada 4096 bytes", limite) != 0 ||
        verificar_permisos_hogar(hogar->pid) != 0) {
        return -1;
    }
    snprintf(linea, sizeof linea, "segunda lectura reserva %" PRIu32 " gpn %" PRIu64
                                  " resultado Hola TEC\n", reserva, gpn);
    if (esperar_log(lector, linea, limite) != 0) {
        return -1;
    }
    snprintf(linea, sizeof linea, "nodo %u lectura reserva %" PRIu32 " gpn %" PRIu64
                                  " resultado Hola TEC\n", lector->id, reserva, gpn);
    if (esperar_log(lector, linea, limite) != 0) {
        return -1;
    }
    if (contar(lector->texto, "PAGE_REQUEST enviado") != 1 ||
        contar(lector->texto, "PAGE_DATA recibido") != 1 ||
        contar(hogar->texto, "PAGE_DATA enviado") != 1) {
        fprintf(stderr, "se esperaba una sola transferencia por dos lecturas\n");
        return -1;
    }
    return 0;
}

static int escenario(const char *config, const unsigned *orden, size_t n,
                     unsigned hogar_id, unsigned lector_id, uint64_t gpn)
{
    struct proceso procesos[MAX_PROCESOS] = {0};
    struct proceso *hogar = NULL, *lector = NULL;
    uint32_t reserva = ((uint32_t)hogar_id << 16) | 1u;
    int resultado = -1;
    for (size_t i = 0; i < n; i++) {
        if (puerto_disponible(orden[i]) != 0) {
            fprintf(stderr, "puerto TCP %u en conflicto: %s\n",
                    PUERTO_BASE + orden[i], strerror(errno));
            return -1;
        }
    }

    int64_t limite = ahora_ms() + 9000;
    for (size_t i = 0; i < n; i++) {
        struct proceso *p = &procesos[i];
        p->id = orden[i];
        if (p->id == hogar_id) {
            hogar = p;
        }
        if (p->id == lector_id) {
            lector = p;
        }
        if (iniciar(p, config, p == lector ? reserva : 0) != 0) {
            perror("iniciar nodo");
            goto salida;
        }
        char linea[80];
        snprintf(linea, sizeof linea, "escuchando puerto %u", PUERTO_BASE + p->id);
        if (esperar_log(p, linea, limite) != 0) {
            goto salida;
        }
    }
    for (size_t i = 0; i < n; i++) {
        for (size_t j = 0; j < n; j++) {
            if (i == j) {
                continue;
            }
            char linea[96];
            snprintf(linea, sizeof linea, "par %u hello 1 welcome 1 estado 2", procesos[j].id);
            if (esperar_log(&procesos[i], linea, limite) != 0) {
                goto salida;
            }
        }
    }
    resultado = verificar_trazas(lector, hogar, reserva, gpn, limite);

salida:
    if (detener(procesos, n) != 0) {
        fprintf(stderr, "algun nodo no cerro limpiamente\n");
        resultado = -1;
    }
    for (size_t i = 0; i < n; i++) {
        if (procesos[i].salida != NULL) {
            leer_log(&procesos[i]);
            fputs(procesos[i].texto, stdout);
            fclose(procesos[i].salida);
        }
        if (puerto_disponible(orden[i]) != 0) {
            fprintf(stderr, "puerto %u no liberado\n", PUERTO_BASE + orden[i]);
            resultado = -1;
        }
    }
    return resultado;
}

static int probar_tres(void)
{
    /* Deja 1 apagado. 2 arranca primero y lee la reserva del hogar 3. */
    char ruta[] = "/tmp/dsm-remoto-XXXXXX";
    int fd = mkstemp(ruta);
    if (fd < 0) {
        return -1;
    }
    int escrito = dprintf(fd,
        "page_size = 4096\n"
        "puerto_rango_inicio = 27200\n"
        "puerto_rango_fin = 27299\n"
        "heartbeat_intervalo_ms = 100\n"
        "nodo = 0 cero 127.0.0.1 27200 1048576\n"
        "nodo = 1 uno 127.0.0.1 27201 2097152\n"
        "nodo = 2 dos 127.0.0.1 27202 524288\n"
        "nodo = 3 tres 127.0.0.1 27203 1048576\n");
    int rc = close(fd);
    if (escrito >= 0 && rc == 0) {
        const unsigned orden[] = {2, 0, 3};
        rc = escenario(ruta, orden, 3, 3, 2, 896);
    } else {
        rc = -1;
    }
    unlink(ruta);
    return rc;
}

int main(void)
{
    const unsigned orden[] = {0, 1};
    if (escenario("config/lab.ini", orden, 2, 0, 1, 0) != 0 || probar_tres() != 0) {
        return 1;
    }
    puts("Paginacion remota correcta con 2 y 3 procesos");
    return 0;
}
