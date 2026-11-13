#include "signals.h"

#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

_Static_assert(ATOMIC_POINTER_LOCK_FREE == 2, "El manejador requiere punteros atomicos lock-free");
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "El manejador requiere enteros atomicos lock-free");

static _Thread_local _Atomic(dsm_solicitud_t *) solicitud_hilo;

static pthread_mutex_t control = PTHREAD_MUTEX_INITIALIZER;
static const dsm_acciones_t *acciones_activas;
static size_t registrados;

static _Noreturn void fallo_fatal(void)
{
    struct sigaction defecto = {.sa_handler = SIG_DFL};
    sigemptyset(&defecto.sa_mask);
    sigset_t segv;
    sigemptyset(&segv);
    sigaddset(&segv, SIGSEGV);
    if (sigaction(SIGSEGV, &defecto, NULL) == 0 &&
        pthread_sigmask(SIG_UNBLOCK, &segv, NULL) == 0) {
        raise(SIGSEGV);
    }
    _exit(128 + SIGSEGV);
}

static void despertar(int signo)
{
    (void)signo;
}

static void atender_fallo(int signo, siginfo_t *info, void *contexto)
{
    (void)signo;
    int errno_previo = errno;
    dsm_solicitud_t *solicitud = atomic_load_explicit(&solicitud_hilo, memory_order_acquire);
    if (solicitud == NULL || info->si_code != SEGV_ACCERR) {
        fallo_fatal();
    }

    const ucontext_t *uc = contexto;
    uintptr_t direccion = (uintptr_t)info->si_addr;
    /* Bits W/R e I/D del codigo de fallo de pagina de x86-64. */
    const greg_t escritura_o_ejecucion = (1u << 1) | (1u << 4);
    if (direccion < solicitud->base || direccion - solicitud->base >= solicitud->bytes ||
        (uc->uc_mcontext.gregs[REG_ERR] & escritura_o_ejecucion) != 0 ||
        atomic_load_explicit(&solicitud->estado, memory_order_acquire) != DSM_SOLICITUD_QUIETA) {
        fallo_fatal();
    }

    sigset_t espera = uc->uc_sigmask;
    sigdelset(&espera, SIGUSR1);
    sigaddset(&espera, SIGSEGV);
    solicitud->pagina = (direccion - solicitud->base) / solicitud->page_size;
    atomic_store_explicit(&solicitud->estado, DSM_SOLICITUD_PENDIENTE, memory_order_release);
    if (sem_post(solicitud->aviso) != 0) {
        fallo_fatal();
    }

    int estado;
    while ((estado = atomic_load_explicit(&solicitud->estado, memory_order_acquire)) ==
           DSM_SOLICITUD_PENDIENTE) {
        if (sigsuspend(&espera) != -1 || errno != EINTR) {
            fallo_fatal();
        }
    }
    if (estado != DSM_SOLICITUD_LISTA) {
        fallo_fatal();
    }
    atomic_store_explicit(&solicitud->estado, DSM_SOLICITUD_QUIETA, memory_order_release);
    errno = errno_previo;
}

static int fallo(int codigo)
{
    errno = codigo;
    return -1;
}

static int instalar_segv(struct sigaction *anterior)
{
    struct sigaction segv = {.sa_sigaction = atender_fallo, .sa_flags = SA_SIGINFO};
    sigemptyset(&segv.sa_mask);
    sigaddset(&segv.sa_mask, SIGUSR1);
    return sigaction(SIGSEGV, &segv, anterior);
}

int dsm_senales_instalar(dsm_acciones_t *acciones)
{
    if (acciones == NULL) {
        return fallo(EINVAL);
    }
    int error = pthread_mutex_lock(&control);
    if (error != 0) {
        return fallo(error);
    }

    if (acciones_activas != NULL) {
        pthread_mutex_unlock(&control);
        return fallo(EBUSY);
    }

    struct sigaction usr1 = {.sa_handler = despertar};
    sigemptyset(&usr1.sa_mask);
    int rc = sigaction(SIGUSR1, &usr1, &acciones->usr1);
    if (rc == 0) {
        rc = instalar_segv(&acciones->segv);
        if (rc == 0) {
            acciones_activas = acciones;
        } else {
            error = errno;
            sigaction(SIGUSR1, &acciones->usr1, NULL);
            errno = error;
        }
    }
    pthread_mutex_unlock(&control);
    return rc;
}

int dsm_senales_restaurar(const dsm_acciones_t *acciones)
{
    if (acciones == NULL) {
        return fallo(EINVAL);
    }
    int error = pthread_mutex_lock(&control);
    if (error != 0) {
        return fallo(error);
    }

    if (acciones != acciones_activas || registrados != 0) {
        error = acciones != acciones_activas ? EINVAL : EBUSY;
        pthread_mutex_unlock(&control);
        return fallo(error);
    }

    int rc = sigaction(SIGSEGV, &acciones->segv, NULL);
    if (rc == 0) {
        rc = sigaction(SIGUSR1, &acciones->usr1, NULL);
        if (rc == 0) {
            acciones_activas = NULL;
        } else {
            error = errno;
            instalar_segv(NULL);
            errno = error;
        }
    }
    pthread_mutex_unlock(&control);
    return rc;
}

static void conjunto_usr1(sigset_t *usr1)
{
    sigemptyset(usr1);
    sigaddset(usr1, SIGUSR1);
}

int dsm_senales_registrar(dsm_solicitud_t *solicitud)
{
    if (solicitud == NULL || solicitud->aviso == NULL || solicitud->bytes == 0 ||
        solicitud->page_size == 0 || solicitud->bytes % solicitud->page_size != 0 ||
        solicitud->bytes - 1 > UINTPTR_MAX - solicitud->base ||
        atomic_load_explicit(&solicitud->estado, memory_order_relaxed) != DSM_SOLICITUD_QUIETA) {
        return fallo(EINVAL);
    }
    if (atomic_load_explicit(&solicitud_hilo, memory_order_relaxed) != NULL) {
        return fallo(EBUSY);
    }
    int error = pthread_mutex_lock(&control);
    if (error != 0) {
        return fallo(error);
    }
    if (acciones_activas == NULL) {
        pthread_mutex_unlock(&control);
        return fallo(EINVAL);
    }

    sigset_t usr1;
    sigset_t mascara;
    conjunto_usr1(&usr1);
    error = pthread_sigmask(SIG_BLOCK, &usr1, &mascara);
    if (error == 0) {
        solicitud->usr1_bloqueada = sigismember(&mascara, SIGUSR1) == 1;
        if (sigismember(&mascara, SIGSEGV) != 0) {
            if (!solicitud->usr1_bloqueada) {
                pthread_sigmask(SIG_UNBLOCK, &usr1, NULL);
            }
            error = EINVAL;
        } else {
            atomic_store_explicit(&solicitud_hilo, solicitud, memory_order_release);
            registrados++;
        }
    }
    pthread_mutex_unlock(&control);
    return error == 0 ? 0 : fallo(error);
}

static int drenar_usr1(const sigset_t *usr1)
{
    const struct timespec inmediato = {0};
    while (sigtimedwait(usr1, NULL, &inmediato) == SIGUSR1 || errno == EINTR) {
    }
    return errno == EAGAIN ? 0 : -1;
}

int dsm_senales_desregistrar(dsm_solicitud_t *solicitud)
{
    if (solicitud == NULL ||
        atomic_load_explicit(&solicitud_hilo, memory_order_relaxed) != solicitud) {
        return fallo(EINVAL);
    }
    int error = pthread_mutex_lock(&control);
    if (error != 0) {
        return fallo(error);
    }

    sigset_t usr1;
    sigset_t mascara;
    conjunto_usr1(&usr1);
    error = pthread_sigmask(SIG_BLOCK, &usr1, &mascara);
    if (error == 0 && drenar_usr1(&usr1) != 0) {
        error = errno;
        if (sigismember(&mascara, SIGUSR1) == 0) {
            pthread_sigmask(SIG_UNBLOCK, &usr1, NULL);
        }
    }
    if (error == 0) {
        atomic_store_explicit(&solicitud_hilo, NULL, memory_order_release);
        if (!solicitud->usr1_bloqueada) {
            error = pthread_sigmask(SIG_UNBLOCK, &usr1, NULL);
        }
        if (error == 0) {
            registrados--;
        } else {
            atomic_store_explicit(&solicitud_hilo, solicitud, memory_order_release);
        }
    }
    pthread_mutex_unlock(&control);
    return error == 0 ? 0 : fallo(error);
}
