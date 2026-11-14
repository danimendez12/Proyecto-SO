#include "pager.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

enum {
    RESERVA = 7,
    NODO = 2,
    GPN_INICIAL = 32,
    VERSION_INICIAL = 8,
    PATRON_LOCAL = 0x31,
    PATRON_VIGENTE = 0x75,
    ESCRITURA = 0xa5,
    TIMEOUT_MS = 500,
    TIMEOUT_PRUEBA_TARDIA_MS = 25
};

enum modo {
    CONCEDER,
    DENEGAR,
    TARDIA
};

struct resolucion {
    const dsm_vistas_t *vistas;
    size_t pagina;
    enum modo modo;
    unsigned *llamadas;
};

struct trabajador {
    dsm_pager_t *pager;
    volatile uint8_t *ar;
    pthread_barrier_t inicio;
    int error;
};

static int fallar(const char *motivo)
{
    fprintf(stderr, "fallo de escritura local (%s)\n", motivo);
    return 1;
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
        char permiso[5];
        if (sscanf(linea, "%" SCNxPTR "-%" SCNxPTR " %4s", &inicio, &fin, permiso) == 3 &&
            inicio <= direccion && direccion < fin) {
            coincide = strcmp(permiso, esperada) == 0;
            break;
        }
    }
    fclose(mapas);
    return coincide;
}

/* Proveedor controlado para ejercitar el paginador local. */
static int resolver(const dsm_pager_peticion_t *peticion, void *buf, size_t capacidad,
                    dsm_pager_concesion_t *concesion, void *arg)
{
    struct resolucion *ctx = arg;
    const dsm_pager_pagina_t *pagina = &peticion->pagina;
    if (pagina->gpn < GPN_INICIAL || pagina->gpn > GPN_INICIAL + 1 ||
        pagina->reserva_id != RESERVA || pagina->version != VERSION_INICIAL ||
        peticion->solicitante != NODO || peticion->acceso != DSM_FALLO_ESCRITURA ||
        capacidad != ctx->pagina) {
        errno = EPROTO;
        return -1;
    }

    size_t indice = (size_t)(pagina->gpn - GPN_INICIAL);
    uint8_t estado = indice == 0 ? DSM_MSI_S : DSM_MSI_I;
    const uint8_t *ar = ctx->vistas->a_r;
    if (pagina->msi != estado || !tiene_proteccion(ar + indice * ctx->pagina,
                                                   estado == DSM_MSI_S ? "r--s" : "---s")) {
        errno = EPROTO;
        return -1;
    }
    if (ctx->modo == TARDIA) {
        /* Devolver una concesion vencida con el mismo reloj del paginador. */
        struct timespec ahora;
        do {
            sched_yield();
            if (clock_gettime(CLOCK_MONOTONIC, &ahora) != 0) {
                return -1;
            }
        } while (ahora.tv_sec < peticion->plazo.tv_sec ||
                 (ahora.tv_sec == peticion->plazo.tv_sec && ahora.tv_nsec < peticion->plazo.tv_nsec));
    }
    size_t bytes_validos = estado == DSM_MSI_I ? ctx->pagina : 0;
    if (bytes_validos != 0) {
        memset(buf, PATRON_VIGENTE, bytes_validos);
    }
    *concesion = (dsm_pager_concesion_t){
        .version = VERSION_INICIAL + 1,
        .bytes_validos = bytes_validos,
        .exclusiva = ctx->modo != DENEGAR,
    };
    ctx->llamadas[indice]++;
    return 0;
}

static int iniciar(dsm_pager_t **pager, dsm_vistas_t *vistas, struct resolucion *ctx)
{
    const dsm_pager_pagina_t paginas[] = {
        {.reserva_id = RESERVA, .gpn = GPN_INICIAL, .version = VERSION_INICIAL, .msi = DSM_MSI_S},
        {.reserva_id = RESERVA, .gpn = GPN_INICIAL + 1, .version = VERSION_INICIAL, .msi = DSM_MSI_I},
    };
    uint32_t timeout_ms = ctx->modo == TARDIA ? TIMEOUT_PRUEBA_TARDIA_MS : TIMEOUT_MS;
    return dsm_pager_iniciar_msi(pager, vistas, ctx->pagina, 2, paginas, NODO, timeout_ms, resolver, ctx);
}

static void *escribir_compartida(void *arg)
{
    struct trabajador *t = arg;
    t->error = dsm_pager_registrar(t->pager);
    pthread_barrier_wait(&t->inicio);
    if (t->error == 0) {
        t->ar[1] = ESCRITURA;
        t->error = dsm_pager_desregistrar(t->pager);
    }
    return NULL;
}

static int escribir_en_paralelo(dsm_pager_t *pager, volatile uint8_t *ar)
{
    struct trabajador t = {.pager = pager, .ar = ar};
    if (pthread_barrier_init(&t.inicio, NULL, 2) != 0) {
        return fallar("barrera de trabajadores");
    }
    pthread_t hilo;
    if (pthread_create(&hilo, NULL, escribir_compartida, &t) != 0) {
        pthread_barrier_destroy(&t.inicio);
        return fallar("crear segundo trabajador");
    }
    pthread_barrier_wait(&t.inicio);
    ar[0] = ESCRITURA;
    pthread_join(hilo, NULL);
    pthread_barrier_destroy(&t.inicio);
    if (t.error != 0) {
        return fallar("registro del segundo trabajador");
    }
    return 0;
}

static int verificar_escrituras(dsm_pager_t *pager, const dsm_vistas_t *vistas, size_t pagina)
{
    volatile uint8_t *ar = vistas->a_r;
    const uint8_t *base = vistas->a_r;
    const uint8_t *cr = vistas->c_r;

    if (!tiene_proteccion(base, "r--s") || !tiene_proteccion(base + pagina, "---s") ||
        ar[0] != PATRON_LOCAL) {
        return fallar("permisos iniciales S e I");
    }
    if (escribir_en_paralelo(pager, ar) != 0) {
        return 1;
    }
    dsm_pager_pagina_t estado;
    if (dsm_pager_consultar_pagina(pager, 0, &estado) != 0 || estado.msi != DSM_MSI_M ||
        estado.version != VERSION_INICIAL + 1 || !tiene_proteccion(base, "rw-s") ||
        !tiene_proteccion(base + pagina, "---s") || ar[0] != ESCRITURA || ar[1] != ESCRITURA ||
        cr[0] != ESCRITURA || cr[1] != ESCRITURA || cr[2] != PATRON_LOCAL ||
        cr[pagina] != PATRON_LOCAL) {
        return fallar("S a M y aislamiento de la pagina vecina");
    }

    ar[2] = ESCRITURA;
    ar[pagina] = ESCRITURA;
    if (dsm_pager_consultar_pagina(pager, 1, &estado) != 0 || estado.msi != DSM_MSI_M ||
        estado.version != VERSION_INICIAL + 1 || !tiene_proteccion(base + pagina, "rw-s") ||
        cr[2] != ESCRITURA || ar[pagina] != ESCRITURA || cr[pagina] != ESCRITURA) {
        return fallar("I a M y escritura directa desde M");
    }
    for (size_t i = 1; i < pagina; i++) {
        if (ar[pagina + i] != PATRON_VIGENTE || cr[pagina + i] != PATRON_VIGENTE) {
            return fallar("imagen vigente de I a M");
        }
    }
    return 0;
}

static int ejecutar(dsm_vistas_t *vistas, struct resolucion *ctx)
{
    dsm_pager_t *pager;
    if (iniciar(&pager, vistas, ctx) != 0) {
        perror("dsm_pager_iniciar_msi");
        return 1;
    }
    int resultado = 1;
    if (dsm_pager_registrar(pager) != 0) {
        perror("dsm_pager_registrar");
    } else {
        if (ctx->modo == CONCEDER) {
            resultado = verificar_escrituras(pager, vistas, ctx->pagina);
        } else {
            volatile uint8_t *ar = vistas->a_r;
            ar[0] = ESCRITURA;
            fallar("se completo una escritura sin concesion valida");
        }
        if (dsm_pager_desregistrar(pager) != 0) {
            perror("dsm_pager_desregistrar");
            resultado = 1;
        }
    }
    if (dsm_pager_cerrar(pager) != 0) {
        perror("dsm_pager_cerrar");
        return 1;
    }
    return resultado;
}

static int verificar_rechazo(dsm_vistas_t *vistas, struct resolucion *ctx)
{
    pid_t hijo = fork();
    if (hijo < 0) {
        perror("fork");
        return 1;
    }
    if (hijo == 0) {
        alarm(3);
        const struct rlimit limite = {0, 0};
        if (setrlimit(RLIMIT_CORE, &limite) != 0) {
            perror("setrlimit");
            _exit(1);
        }
        _exit(ejecutar(vistas, ctx));
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
    if (!WIFSIGNALED(estado) || WTERMSIG(estado) != SIGSEGV ||
        ctx->llamadas[0] != 1 || ctx->llamadas[1] != 0) {
        return fallar("concesion rechazada tras un unico fallo de escritura real");
    }
    const uint8_t *cr = vistas->c_r;
    for (size_t i = 0; i < vistas->bytes; i++) {
        if (cr[i] != PATRON_LOCAL) {
            return fallar("contenido modificado tras una concesion invalida");
        }
    }
    return 0;
}

static int probar(size_t pagina, enum modo modo)
{
    dsm_vistas_t vistas;
    if (dsm_vistas_crear(2 * pagina, &vistas) != 0) {
        perror("dsm_vistas_crear");
        return 1;
    }
    unsigned *llamadas = mmap(NULL, 2 * sizeof *llamadas, PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (llamadas == MAP_FAILED) {
        perror("mmap de evidencia");
        dsm_vistas_cerrar(&vistas);
        return 1;
    }

    memset(vistas.c_r, PATRON_LOCAL, vistas.bytes);
    struct resolucion ctx = {.vistas = &vistas, .pagina = pagina, .modo = modo, .llamadas = llamadas};
    int resultado;
    if (modo == CONCEDER) {
        resultado = ejecutar(&vistas, &ctx);
        if (resultado == 0 && (llamadas[0] != 1 || llamadas[1] != 1)) {
            resultado = fallar("una concesion por pagina y ninguna escritura adicional desde M");
        }
    } else {
        resultado = verificar_rechazo(&vistas, &ctx);
    }
    munmap(llamadas, 2 * sizeof *llamadas);
    dsm_vistas_cerrar(&vistas);
    return resultado;
}

int main(void)
{
    alarm(10);
    long pagina = sysconf(_SC_PAGESIZE);
    if (pagina <= 0 || (size_t)pagina > SIZE_MAX / 2) {
        return fallar("tamano de pagina del sistema");
    }
    if (probar((size_t)pagina, DENEGAR) != 0 || probar((size_t)pagina, TARDIA) != 0 ||
        probar((size_t)pagina, CONCEDER) != 0) {
        return 1;
    }
    puts("escritura local S -> M e I -> M, exclusividad denegada y plazo vencido verificados");
    alarm(0);
    return 0;
}
