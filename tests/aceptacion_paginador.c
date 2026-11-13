#include "pager.h"
#include "memory.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

enum { PATRON = 0xa5 };

struct preparacion {
    void *gestor;
    size_t bytes;
    pthread_t trabajador;
    atomic_uint llamadas;
};

static int fallar(const char *motivo)
{
    fprintf(stderr, "fallo de paginacion (%s)\n", motivo);
    return 1;
}

static int preparar(void *pagina_cr, size_t indice, size_t bytes, void *arg)
{
    struct preparacion *datos = arg;
    if (pthread_equal(pthread_self(), datos->trabajador) ||
        indice != 0 || pagina_cr != datos->gestor || bytes != datos->bytes) {
        fprintf(stderr, "el callback recibio un hilo o una pagina incorrectos\n");
        return -1;
    }
    memset(pagina_cr, PATRON, bytes);
    atomic_fetch_add_explicit(&datos->llamadas, 1, memory_order_relaxed);
    return 0;
}

static int tiene_proteccion(const void *p, const char *esperada)
{
    FILE *mapas = fopen("/proc/self/maps", "r");
    if (mapas == NULL) {
        return 0;
    }
    uintptr_t direccion = (uintptr_t)p;
    char linea[512];
    int coincide = 0;
    while (fgets(linea, sizeof linea, mapas) != NULL) {
        uintptr_t inicio;
        uintptr_t fin;
        char permisos[5];
        if (sscanf(linea, "%" SCNxPTR "-%" SCNxPTR " %4s", &inicio, &fin, permisos) == 3 &&
            inicio <= direccion && direccion < fin) {
            coincide = strcmp(permisos, esperada) == 0;
            break;
        }
    }
    fclose(mapas);
    return coincide;
}

static int leer_reserva(const dsm_vistas_t *vistas, const struct preparacion *datos)
{
    const uint8_t *base = vistas->a_r;
    if (!tiene_proteccion(base, "---s") || !tiene_proteccion(base + datos->bytes, "---s") ||
        atomic_load_explicit(&datos->llamadas, memory_order_relaxed) != 0) {
        return fallar("ambas paginas deben iniciar en PROT_NONE y sin preparar");
    }
    const volatile uint8_t *app = base;
    for (size_t i = 0; i < datos->bytes; i++) {
        if (app[i] != PATRON) {
            return fallar("la lectura no obtuvo el patron del paginador");
        }
    }
    if (atomic_load_explicit(&datos->llamadas, memory_order_relaxed) != 1) {
        return fallar("se esperaba una sola llamada de preparacion");
    }
    if (!tiene_proteccion(base, "r--s") || !tiene_proteccion(base + datos->bytes, "---s")) {
        return fallar("solo la primera pagina debe quedar legible");
    }
    return 0;
}

static int leer_fuera_de_reserva(size_t pagina)
{
    void *fuera = mmap(NULL, pagina, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (fuera == MAP_FAILED) {
        perror("mmap de la pagina ajena");
        return 1;
    }
    const volatile uint8_t *app = fuera;
    (void)app[0];
    munmap(fuera, pagina);
    return fallar("se completo una lectura ajena a la reserva");
}

static int probar_paginador(size_t pagina, int fallo_ajeno)
{
    dsm_vistas_t vistas;
    if (dsm_vistas_crear(2 * pagina, &vistas) != 0) {
        perror("dsm_vistas_crear");
        return 1;
    }
    struct preparacion datos = {
        .gestor = vistas.c_r,
        .bytes = pagina,
        .trabajador = pthread_self(),
    };
    atomic_init(&datos.llamadas, 0);

    dsm_pager_t *pager = NULL;
    if (dsm_pager_iniciar(&pager, &vistas, pagina, 1, preparar, &datos) != 0) {
        perror("dsm_pager_iniciar");
        dsm_vistas_cerrar(&vistas);
        return 1;
    }
    int resultado = 1;
    if (dsm_pager_registrar(pager) != 0) {
        perror("dsm_pager_registrar");
    } else {
        resultado = fallo_ajeno ? leer_fuera_de_reserva(pagina) : leer_reserva(&vistas, &datos);
        if (dsm_pager_desregistrar(pager) != 0) {
            perror("dsm_pager_desregistrar");
            resultado = 1;
        }
    }
    if (dsm_pager_cerrar(pager) != 0) {
        perror("dsm_pager_cerrar");
        return 1;
    }
    dsm_vistas_cerrar(&vistas);
    return resultado;
}

static int verificar_fallo_ajeno(size_t pagina)
{
    pid_t hijo = fork();
    if (hijo < 0) {
        perror("fork");
        return 1;
    }
    if (hijo == 0) {
        alarm(5);
        const struct rlimit limite = {0, 0};
        if (setrlimit(RLIMIT_CORE, &limite) != 0) {
            perror("setrlimit");
            _exit(1);
        }
        _exit(probar_paginador(pagina, 1));
    }

    int estado;
    pid_t esperado;
    do {
        esperado = waitpid(hijo, &estado, 0);
    } while (esperado < 0 && errno == EINTR);
    if (esperado < 0) {
        perror("waitpid");
        return 1;
    }
    if (!WIFSIGNALED(estado) || WTERMSIG(estado) != SIGSEGV) {
        return fallar("el acceso ajeno debe terminar con SIGSEGV");
    }
    return 0;
}

int main(void)
{
    alarm(10);
    long pagina = sysconf(_SC_PAGESIZE);
    if (pagina <= 0 || (size_t)pagina > SIZE_MAX / 2) {
        return fallar("tamano de pagina del sistema");
    }
    if (verificar_fallo_ajeno((size_t)pagina) != 0 || probar_paginador((size_t)pagina, 0) != 0) {
        return 1;
    }
    puts("lectura recuperada, segunda pagina protegida y SIGSEGV ajena fatal");
    alarm(0);
    return 0;
}
