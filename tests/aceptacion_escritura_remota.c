#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { MAX_NODOS = 4, PUERTO_BASE = 27200, SIN_POSEEDOR = 65535, LOG_BYTES = 65536 };

struct nodo {
    unsigned id;
    pid_t pid;
    int entrada;
    FILE *log;
    uintptr_t ar;
    char texto[LOG_BYTES];
    size_t bytes;
};

struct estado {
    char msi;
    unsigned version;
    unsigned poseedor;
    unsigned long long mascara;
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
    const struct timespec espera = {.tv_nsec = 10000000};
    nanosleep(&espera, NULL);
}

static int puerto_libre(unsigned id)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    int uno = 1;
    struct sockaddr_in direccion = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)(PUERTO_BASE + id)),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    int rc = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &uno, sizeof uno);
    if (rc == 0) {
        rc = bind(fd, (struct sockaddr *)&direccion, sizeof direccion);
    }
    int error = errno;
    close(fd);
    errno = error;
    return rc;
}

static int cargar_log(struct nodo *nodo)
{
    ssize_t n = pread(fileno(nodo->log), nodo->texto, sizeof nodo->texto - 1, 0);
    if (n < 0 || n == (ssize_t)sizeof nodo->texto - 1) {
        return -1;
    }
    nodo->bytes = (size_t)n;
    nodo->texto[n] = '\0';
    return 0;
}

static int esperar(struct nodo *nodo, size_t desde, const char *frase)
{
    int64_t limite = ahora_ms() + 8000;
    do {
        if (cargar_log(nodo) == 0 && desde <= nodo->bytes) {
            const char *encontrada = strstr(nodo->texto + desde, frase);
            if (encontrada != NULL && strchr(encontrada, '\n') != NULL) {
                return 0;
            }
        }
        pausa();
    } while (ahora_ms() < limite);
    fprintf(stderr, "nodo %u no completo %s\n", nodo->id, frase);
    return -1;
}

static int enviar(struct nodo *nodo, const char *comando, size_t *desde)
{
    if (cargar_log(nodo) != 0) {
        return -1;
    }
    *desde = nodo->bytes;
    size_t n = strlen(comando);
    ssize_t escrito;
    do {
        escrito = write(nodo->entrada, comando, n);
    } while (escrito < 0 && errno == EINTR);
    return escrito == (ssize_t)n ? 0 : -1;
}

static int comando(struct nodo *nodo, const char *orden, const char *respuesta)
{
    size_t desde;
    return enviar(nodo, orden, &desde) == 0 ? esperar(nodo, desde, respuesta) : -1;
}

static int iniciar(struct nodo *nodo, const char *ruta, uint32_t reserva)
{
    int entrada[2];
    nodo->entrada = -1;
    nodo->log = tmpfile();
    if (nodo->log == NULL) {
        return -1;
    }
    int fd = fileno(nodo->log);
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0 || pipe2(entrada, O_CLOEXEC) != 0) {
        return -1;
    }
    char id[16], reserva_texto[16];
    snprintf(id, sizeof id, "%u", nodo->id);
    snprintf(reserva_texto, sizeof reserva_texto, "%" PRIu32, reserva);
    nodo->pid = fork();
    if (nodo->pid < 0) {
        close(entrada[0]);
        close(entrada[1]);
        return -1;
    }
    if (nodo->pid == 0) {
        const struct rlimit limite = {0, 0};
        if (setrlimit(RLIMIT_CORE, &limite) != 0 || dup2(entrada[0], STDIN_FILENO) < 0 ||
            dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) {
            _exit(126);
        }
        if (reserva == 0) {
            execl("./bin/dsm_node", "dsm_node", ruta, id, "interactivo", (char *)NULL);
        } else {
            execl("./bin/dsm_node", "dsm_node", ruta, id, "interactivo", reserva_texto, (char *)NULL);
        }
        _exit(127);
    }
    close(entrada[0]);
    nodo->entrada = entrada[1];
    return 0;
}

static int listo(struct nodo *nodo, uint32_t reserva)
{
    char frase[80];
    snprintf(frase, sizeof frase, "nodo %u listo reserva %" PRIu32 " Ar ", nodo->id, reserva);
    if (esperar(nodo, 0, frase) != 0) {
        return -1;
    }
    const char *linea = strstr(nodo->texto, frase);
    return sscanf(linea + strlen(frase), "%" SCNxPTR, &nodo->ar) == 1 && nodo->ar != 0 ? 0 : -1;
}

static int permiso(const struct nodo *nodo, const char *esperado)
{
    char ruta[64];
    snprintf(ruta, sizeof ruta, "/proc/%ld/maps", (long)nodo->pid);
    FILE *mapas = fopen(ruta, "r");
    if (mapas == NULL) {
        return -1;
    }
    char linea[512];
    int coincide = 0;
    while (fgets(linea, sizeof linea, mapas) != NULL) {
        uintptr_t inicio, fin;
        char permisos[5];
        if (sscanf(linea, "%" SCNxPTR "-%" SCNxPTR " %4s", &inicio, &fin, permisos) == 3 &&
            inicio <= nodo->ar && nodo->ar < fin) {
            coincide = strcmp(permisos, esperado) == 0;
            break;
        }
    }
    fclose(mapas);
    if (!coincide) {
        fprintf(stderr, "nodo %u Ar no tiene permisos %s\n", nodo->id, esperado);
        return -1;
    }
    return 0;
}

static int consultar(struct nodo *nodo, struct estado *estado)
{
    size_t desde;
    char frase[64];
    snprintf(frase, sizeof frase, "nodo %u estado ", nodo->id);
    if (enviar(nodo, "estado\n", &desde) != 0 || esperar(nodo, desde, frase) != 0) {
        return -1;
    }
    const char *linea = strstr(nodo->texto + desde, frase) + strlen(frase);
    if (sscanf(linea, "%c version %u poseedor %u mascara %llu", &estado->msi,
               &estado->version, &estado->poseedor, &estado->mascara) != 4) {
        return -1;
    }
    return 0;
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

static int dos_nodos(struct nodo *nodos)
{
    struct nodo *hogar = &nodos[0], *escritor = &nodos[1];
    struct estado casa, remoto;
    if (listo(hogar, 1) != 0 || listo(escritor, 1) != 0 ||
        permiso(hogar, "rw-s") != 0 || permiso(escritor, "---s") != 0 ||
        comando(escritor, "leer\n", "lectura [Hola TEC]") != 0 ||
        consultar(hogar, &casa) != 0 || consultar(escritor, &remoto) != 0 ||
        casa.msi != 'S' || remoto.msi != 'S' || casa.version != remoto.version ||
        casa.poseedor != SIN_POSEEDOR || remoto.poseedor != SIN_POSEEDOR ||
        permiso(hogar, "r--s") != 0 || permiso(escritor, "r--s") != 0) {
        return -1;
    }
    unsigned version_s = casa.version;
    if (comando(escritor, "escribir\n", "escritura [Adios TEC]") != 0 ||
        consultar(escritor, &remoto) != 0 || consultar(hogar, &casa) != 0 ||
        remoto.msi != 'M' || casa.msi != 'I' || remoto.poseedor != escritor->id ||
        casa.poseedor != escritor->id || remoto.version <= version_s ||
        remoto.version != casa.version || remoto.mascara != 2 || casa.mascara != 2 ||
        permiso(escritor, "rw-s") != 0 ||
        permiso(hogar, "---s") != 0 ||
        esperar(escritor, 0, "fallo SIGSEGV escritura") != 0 ||
        esperar(escritor, 0, "INVALIDATE enviado") != 0 ||
        esperar(hogar, 0, "INVALIDATE recibido") != 0 ||
        esperar(hogar, 0, "INVALIDATE_ACK enviado") != 0 ||
        esperar(escritor, 0, "INVALIDATE_ACK recibido") != 0 ||
        esperar(escritor, 0, "PAGE_REQUEST enviado") != 0 ||
        esperar(escritor, 0, "PAGE_DATA recibido") != 0 ||
        esperar(hogar, 0, "PAGE_REQUEST escritura par 1") != 0 ||
        esperar(hogar, 0, "MSI -> M poseedor 1") != 0) {
        return -1;
    }
    unsigned version_m = remoto.version;
    if (comando(hogar, "leer\n", "lectura [Adios TEC]") != 0 ||
        consultar(hogar, &casa) != 0 || consultar(escritor, &remoto) != 0 ||
        casa.msi != 'S' || remoto.msi != 'S' || casa.version != remoto.version ||
        casa.version <= version_m || remoto.version <= version_m ||
        permiso(hogar, "r--s") != 0 || permiso(escritor, "r--s") != 0 ||
        esperar(hogar, 0, "fallo SIGSEGV lectura") != 0 ||
        esperar(escritor, 0, "PAGE_DOWNGRADE recibido par 0") != 0) {
        return -1;
    }
    if (cargar_log(hogar) != 0) {
        return -1;
    }
    int solicitudes = contar(hogar->texto, "PAGE_REQUEST enviado");
    if (comando(hogar, "leer\n", "lectura [Adios TEC]") != 0 || cargar_log(hogar) != 0 ||
        contar(hogar->texto, "PAGE_REQUEST enviado") != solicitudes) {
        fprintf(stderr, "una lectura S genero otra transferencia\n");
        return -1;
    }
    return 0;
}

static int cuatro_nodos(struct nodo *nodos)
{
    uint32_t reserva = (UINT32_C(3) << 16) | 1u;
    for (size_t i = 0; i < MAX_NODOS; i++) {
        if (listo(&nodos[i], reserva) != 0 ||
            comando(&nodos[i], "leer\n", "lectura [Hola TEC]") != 0) {
            return -1;
        }
    }
    for (size_t i = 0; i < MAX_NODOS; i++) {
        struct estado estado;
        if (consultar(&nodos[i], &estado) != 0 || estado.msi != 'S' ||
            permiso(&nodos[i], "r--s") != 0) {
            return -1;
        }
    }
    size_t desde_uno, desde_dos;
    if (enviar(&nodos[1], "marca\n", &desde_uno) != 0 ||
        enviar(&nodos[2], "marca\n", &desde_dos) != 0 ||
        esperar(&nodos[1], desde_uno, "marca aplicada") != 0 ||
        esperar(&nodos[2], desde_dos, "marca aplicada") != 0) {
        return -1;
    }

    unsigned version = 0, modificadas = 0;
    for (size_t i = 0; i < MAX_NODOS; i++) {
        struct estado estado;
        if (consultar(&nodos[i], &estado) != 0) {
            return -1;
        }
        modificadas += estado.msi == 'M';
        if (estado.msi != 'I' &&
            (estado.msi != 'M' || estado.poseedor != nodos[i].id ||
             estado.mascara != (UINT64_C(1) << nodos[i].id))) {
            return -1;
        }
        if (estado.version > version) {
            version = estado.version;
        }
        if (permiso(&nodos[i], estado.msi == 'M' ? "rw-s" : "---s") != 0) {
            return -1;
        }
    }
    if (modificadas != 1 || version < 2 ||
        comando(&nodos[0], "marcas\n", "marcas 0 2 3 0") != 0 ||
        comando(&nodos[3], "marcas\n", "marcas 0 2 3 0") != 0 ||
        comando(&nodos[0], "leer\n", "lectura [Hola TEC]") != 0) {
        fprintf(stderr, "se perdieron escrituras concurrentes o hubo multiples copias M\n");
        return -1;
    }
    struct estado lector, hogar;
    if (consultar(&nodos[0], &lector) != 0 || consultar(&nodos[3], &hogar) != 0 ||
        lector.msi != 'S' || hogar.msi != 'S' || lector.version != hogar.version ||
        permiso(&nodos[0], "r--s") != 0 || permiso(&nodos[3], "r--s") != 0 ||
        esperar(&nodos[2], 0, "fallo SIGSEGV escritura") != 0) {
        return -1;
    }
    return 0;
}

static int detener(struct nodo *nodos, size_t cantidad)
{
    int rc = 0;
    for (size_t i = 0; i < cantidad; i++) {
        if (nodos[i].entrada >= 0) {
            size_t desde;
            if (enviar(&nodos[i], "salir\n", &desde) != 0) {
                rc = -1;
            }
            close(nodos[i].entrada);
            nodos[i].entrada = -1;
        }
    }
    int64_t limite = ahora_ms() + 3500;
    for (size_t i = 0; i < cantidad; i++) {
        if (nodos[i].pid <= 0) {
            continue;
        }
        int estado;
        pid_t terminado;
        do {
            terminado = waitpid(nodos[i].pid, &estado, WNOHANG);
            if (terminado == 0) {
                pausa();
            }
        } while ((terminado == 0 || (terminado < 0 && errno == EINTR)) && ahora_ms() < limite);
        if (terminado == 0) {
            kill(nodos[i].pid, SIGKILL);
            while (waitpid(nodos[i].pid, NULL, 0) < 0 && errno == EINTR) {
            }
            rc = -1;
        } else if (terminado != nodos[i].pid || !WIFEXITED(estado) || WEXITSTATUS(estado) != 0) {
            rc = -1;
        }
        nodos[i].pid = 0;
    }
    return rc;
}

static int escenario(unsigned cantidad, unsigned hogar)
{
    char ruta[] = "/tmp/dsm-escritura-XXXXXX";
    int fd = mkstemp(ruta);
    if (fd < 0) {
        return -1;
    }
    int rc = dprintf(fd, "page_size = 4096\nheartbeat_intervalo_ms = 100\nrpc_timeout_ms = 1500\n"
                         "puerto_rango_inicio = 27200\npuerto_rango_fin = 27299\n");
    struct nodo nodos[MAX_NODOS] = {0};
    for (unsigned i = 0; i < MAX_NODOS; i++) {
        nodos[i].id = i;
        nodos[i].entrada = -1;
    }
    for (unsigned i = 0; i < cantidad && rc >= 0; i++) {
        rc = dprintf(fd, "nodo = %u nodo%u 127.0.0.1 %u 1048576\n", i, i, PUERTO_BASE + i);
    }
    if (close(fd) != 0 || rc < 0) {
        unlink(ruta);
        return -1;
    }
    rc = -1;
    for (unsigned i = 0; i < cantidad; i++) {
        if (puerto_libre(i) != 0) {
            fprintf(stderr, "puerto %u ocupado: %s\n", PUERTO_BASE + i, strerror(errno));
            goto limpiar;
        }
    }
    /* El hogar arranca al final para ejercitar el reintento de los otros nodos. */
    for (unsigned i = 0; i < cantidad; i++) {
        unsigned id = (hogar + 1 + i) % cantidad;
        uint32_t reserva = id == hogar ? 0 : ((uint32_t)hogar << 16) | 1u;
        if (iniciar(&nodos[id], ruta, reserva) != 0) {
            perror("iniciar nodo interactivo");
            goto limpiar;
        }
    }
    rc = cantidad == 2 ? dos_nodos(nodos) : cuatro_nodos(nodos);
    if (rc != 0) {
        fprintf(stderr, "fallo el escenario de %u nodos con hogar %u\n", cantidad, hogar);
    }

limpiar:
    if (detener(nodos, cantidad) != 0) {
        fprintf(stderr, "un proceso no termino ordenadamente\n");
        rc = -1;
    }
    for (unsigned i = 0; i < cantidad; i++) {
        if (nodos[i].log != NULL) {
            if (cargar_log(&nodos[i]) == 0) {
                fputs(nodos[i].texto, stdout);
            }
            fclose(nodos[i].log);
        }
        if (puerto_libre(i) != 0) {
            rc = -1;
        }
    }
    unlink(ruta);
    return rc;
}

int main(void)
{
    struct sigaction accion = {.sa_handler = SIG_IGN};
    sigemptyset(&accion.sa_mask);
    if (sigaction(SIGPIPE, &accion, NULL) != 0 || escenario(2, 0) != 0 || escenario(4, 3) != 0) {
        return 1;
    }
    puts("Escritura remota coherente, relectura y escritores concurrentes verificados en localhost");
    return 0;
}
