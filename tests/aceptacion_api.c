#include "dsm.h"

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { TAMANO_PAGINA = 4096, TAMANO_LOGICO = TAMANO_PAGINA - 7 };

static const char configuracion[] = "config/lab.ini";
static const uint8_t directo[] = {0x13, 0x57, 0x9b};
static const uint8_t controlado[] = {0x21, 0x43, 0x65};

#define VERIFICAR(condicion) do {                                       \
    if (!(condicion)) {                                                  \
        fprintf(stderr, "aceptacion_api:%d: %s\n", __LINE__, #condicion); \
        goto terminar;                                                  \
    }                                                                   \
} while (0)

/* El llamador permanece vivo y registrado hasta que el otro hilo vuelve. */
struct intento_cierre {
    dsm_context_t *ctx;
    dsm_reserva_id_t reserva;
    int liberacion;
    int cierre;
};

static void *intentar_cierre(void *arg)
{
    struct intento_cierre *intento = arg;
    intento->liberacion = dsm_liberar(intento->ctx, intento->reserva);
    intento->cierre = dsm_cerrar(intento->ctx);
    return NULL;
}

static int segv_restaurada(const struct sigaction *anterior)
{
    struct sigaction actual;
    if (sigaction(SIGSEGV, NULL, &actual) != 0 ||
        ((anterior->sa_flags ^ actual.sa_flags) & SA_SIGINFO) != 0) {
        return 0;
    }
    return (actual.sa_flags & SA_SIGINFO) != 0 ? actual.sa_sigaction == anterior->sa_sigaction
                                               : actual.sa_handler == anterior->sa_handler;
}

static int probar_reserva(void)
{
    dsm_context_t *ctx = NULL;
    int registrado = 0;
    int resultado = 1;
    dsm_proc_id_t proc;
    dsm_reserva_id_t reserva;
    void *base;
    sigset_t mascara;
    struct sigaction anterior;
    struct sigaction instalada;

    VERIFICAR(sigaction(SIGSEGV, NULL, &anterior) == 0);
    VERIFICAR(dsm_iniciar(NULL, 0, &ctx) == DSM_ERR_INVAL);
    VERIFICAR(dsm_iniciar(configuracion, UINT16_MAX, &ctx) == DSM_ERR_CONFIG);
    VERIFICAR(dsm_iniciar(configuracion, 0, &ctx) == DSM_OK && ctx != NULL);
    VERIFICAR(sigaction(SIGSEGV, NULL, &instalada) == 0 &&
              (instalada.sa_flags & SA_SIGINFO) != 0 && !segv_restaurada(&anterior));
    dsm_context_t *duplicado = NULL;
    VERIFICAR(dsm_iniciar(configuracion, 1, &duplicado) == DSM_ERR_ESTADO);
    VERIFICAR(dsm_proc_abrir(ctx, &proc) == DSM_OK && proc != 0);
    dsm_proc_id_t otro_proc;
    VERIFICAR(dsm_proc_abrir(ctx, &otro_proc) == DSM_ERR_ESTADO);
    VERIFICAR(dsm_hilo_registrar(ctx, 0) == DSM_ERR_INVAL);

    VERIFICAR(pthread_sigmask(SIG_SETMASK, NULL, &mascara) == 0);
    int usr1_original = sigismember(&mascara, SIGUSR1);
    VERIFICAR(dsm_hilo_registrar(ctx, proc) == DSM_OK);
    registrado = 1;
    VERIFICAR(pthread_sigmask(SIG_SETMASK, NULL, &mascara) == 0 && sigismember(&mascara, SIGUSR1) == 1);

    VERIFICAR(dsm_reservar(ctx, proc, 0, &reserva, &base) == DSM_ERR_INVAL);
    VERIFICAR(dsm_reservar(ctx, proc, sizeof directo, NULL, &base) == DSM_ERR_INVAL);
    /* 11B admite varias paginas y rechaza tamanos que desbordan la capacidad. */
    VERIFICAR(dsm_reservar(ctx, proc, SIZE_MAX, &reserva, &base) == DSM_ERR_NOMEM);
    VERIFICAR(dsm_reservar(ctx, proc, TAMANO_LOGICO, &reserva, &base) == DSM_OK && base != NULL);
    dsm_reserva_id_t otra_reserva;
    void *otra_base;
    VERIFICAR(dsm_reservar(ctx, proc, 1, &otra_reserva, &otra_base) == DSM_OK &&
              otra_reserva != reserva && otra_base != NULL && otra_base != base);
    ((volatile uint8_t *)otra_base)[0] = 0x7a;

    void *abierta;
    size_t bytes;
    VERIFICAR(dsm_abrir_reserva(ctx, reserva, &abierta, &bytes) == DSM_OK &&
              abierta == base && bytes == TAMANO_LOGICO);
    dsm_reserva_id_t remota = (UINT32_C(1) << 16) | 1u;
    VERIFICAR(dsm_abrir_reserva(ctx, remota, &abierta, &bytes) == DSM_ERR_NODO);
    VERIFICAR(dsm_desvincular_reserva(ctx, reserva) == DSM_ERR_ESTADO);

    volatile uint8_t *ar = base;
    VERIFICAR(ar[0] == 0 && ar[TAMANO_LOGICO - 1] == 0);
    for (size_t i = 0; i < sizeof directo; i++) {
        ar[i] = directo[i];
    }
    uint8_t leido[sizeof directo];
    VERIFICAR(dsm_leer(ctx, reserva, 0, leido, sizeof leido) == DSM_OK &&
              memcmp(leido, directo, sizeof directo) == 0);

    VERIFICAR(dsm_escribir(ctx, reserva, sizeof directo, controlado, sizeof controlado) == DSM_OK);
    void *traducida;
    VERIFICAR(dsm_traducir(ctx, reserva, sizeof directo, &traducida) == DSM_OK &&
              traducida == (uint8_t *)base + sizeof directo);
    const volatile uint8_t *lectura = traducida;
    for (size_t i = 0; i < sizeof controlado; i++) {
        VERIFICAR(lectura[i] == controlado[i]);
    }

    VERIFICAR(dsm_traducir(ctx, reserva, TAMANO_LOGICO, &traducida) == DSM_ERR_INVAL);
    VERIFICAR(dsm_leer(ctx, reserva, 0, NULL, sizeof leido) == DSM_ERR_INVAL);
    VERIFICAR(dsm_leer(ctx, reserva, SIZE_MAX, leido, sizeof leido) == DSM_ERR_INVAL);
    VERIFICAR(dsm_escribir(ctx, reserva, TAMANO_LOGICO - 1, controlado, sizeof controlado) == DSM_ERR_INVAL);
    VERIFICAR(dsm_leer(ctx, reserva, TAMANO_LOGICO, NULL, 0) == DSM_OK);
    VERIFICAR(dsm_escribir(ctx, reserva, TAMANO_LOGICO, NULL, 0) == DSM_OK);
    VERIFICAR(ar[TAMANO_LOGICO - 1] == 0);

    struct intento_cierre intento = {.ctx = ctx, .reserva = reserva};
    pthread_t hilo;
    VERIFICAR(pthread_create(&hilo, NULL, intentar_cierre, &intento) == 0);
    VERIFICAR(pthread_join(hilo, NULL) == 0);
    /* Si el cierre tuvo exito incorrectamente, el contexto ya fue consumido. */
    if (intento.cierre == DSM_OK) {
        ctx = NULL;
    }
    VERIFICAR(intento.liberacion == DSM_ERR_OCUPADA && intento.cierre == DSM_ERR_OCUPADA);
    VERIFICAR(ar[0] == directo[0]);

    VERIFICAR(dsm_hilo_desregistrar(ctx) == DSM_OK);
    registrado = 0;
    VERIFICAR(pthread_sigmask(SIG_SETMASK, NULL, &mascara) == 0 &&
              sigismember(&mascara, SIGUSR1) == usr1_original);
    VERIFICAR(dsm_liberar(ctx, reserva) == DSM_OK);
    /* El contexto conserva otras reservas y su manejador hasta dsm_cerrar. */
    VERIFICAR(sigaction(SIGSEGV, NULL, &instalada) == 0 &&
              (instalada.sa_flags & SA_SIGINFO) != 0 && !segv_restaurada(&anterior));
    VERIFICAR(dsm_liberar(ctx, reserva) == DSM_ERR_INVAL);
    VERIFICAR(dsm_abrir_reserva(ctx, reserva, &abierta, &bytes) == DSM_ERR_INVAL);
    VERIFICAR(dsm_hilo_registrar(ctx, proc) == DSM_OK);
    registrado = 1;
    uint8_t conservado;
    VERIFICAR(dsm_leer(ctx, otra_reserva, 0, &conservado, 1) == DSM_OK && conservado == 0x7a);
    VERIFICAR(dsm_hilo_desregistrar(ctx) == DSM_OK);
    registrado = 0;
    VERIFICAR(dsm_liberar(ctx, otra_reserva) == DSM_OK);
    VERIFICAR(dsm_reservar(ctx, proc, 1, &otra_reserva, &otra_base) == DSM_OK &&
              otra_reserva != reserva);
    resultado = 0;

terminar:
    if (ctx != NULL) {
        if (registrado && dsm_hilo_desregistrar(ctx) != DSM_OK) {
            fprintf(stderr, "aceptacion_api: no se pudo desregistrar el hilo\n");
            resultado = 1;
        }
        if (dsm_cerrar(ctx) != DSM_OK) {
            fprintf(stderr, "aceptacion_api: no se pudo cerrar el contexto\n");
            resultado = 1;
        }
    }
    if (resultado == 0 && !segv_restaurada(&anterior)) {
        fprintf(stderr, "aceptacion_api: no se restauro SIGSEGV al cerrar\n");
        resultado = 1;
    }
    return resultado;
}

static int probar_reinicio(void)
{
    dsm_context_t *ctx = NULL;
    int resultado = 1;
    dsm_proc_id_t proc;
    dsm_reserva_id_t reserva;
    void *base;
    struct sigaction anterior;

    VERIFICAR(sigaction(SIGSEGV, NULL, &anterior) == 0);
    VERIFICAR(dsm_iniciar(configuracion, 1, &ctx) == DSM_OK && ctx != NULL);
    VERIFICAR(dsm_proc_abrir(ctx, &proc) == DSM_OK && (proc >> 16) == 1);
    VERIFICAR(dsm_reservar(ctx, proc, 1, &reserva, &base) == DSM_OK &&
              (reserva >> 16) == 1 && base != NULL);
    /* El cierre tambien debe liberar una reserva que siga publicada. */
    VERIFICAR(dsm_cerrar(ctx) == DSM_OK);
    ctx = NULL;
    VERIFICAR(segv_restaurada(&anterior));
    resultado = 0;

terminar:
    if (ctx != NULL && dsm_cerrar(ctx) != DSM_OK) {
        fprintf(stderr, "aceptacion_api: no se pudo cerrar el segundo contexto\n");
        resultado = 1;
    }
    return resultado;
}

int main(void)
{
    if (probar_reserva() != 0 || probar_reinicio() != 0) {
        return 1;
    }
    puts("API local: acceso por Ar, limites, restauracion de senales, cierre seguro y reinicio verificados");
    return 0;
}
