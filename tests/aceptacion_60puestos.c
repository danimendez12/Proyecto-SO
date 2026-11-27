#include "dsm.h"
#include "config.h"
#include "directory.h"

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"60puestos:%d: %s\n",__LINE__,#x); goto fin; } } while (0)

static int io(int fd, void *buf, size_t n, int salida)
{
    unsigned char *p = buf;
    while (n) {
        ssize_t r = salida ? write(fd,p,n) : read(fd,p,n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}

int main(void)
{
    alarm(20);
    setvbuf(stdout,NULL,_IOLBF,0);
    int resultado = 1;
    FILE *f = tmpfile();
    dsm_config_t oficial, cfg, historica;
    dsm_directorio_t dir = {0};
    struct hijo { pid_t pid; int orden[2], respuesta[2]; uint16_t id; } h[3] = {
        {.orden={-1,-1},.respuesta={-1,-1}}, {.orden={-1,-1},.respuesta={-1,-1}},
        {.orden={-1,-1},.respuesta={-1,-1}}
    };
    CHECK(f != NULL && DSM_CONFIG_MAX_NODOS == 60);
    CHECK(dsm_config_cargar("config/pool-lab.ini",&oficial) == 0 && oficial.cantidad_nodos == 60);
    CHECK(dsm_config_base(&oficial,60) == UINT64_C(503316480));
    for (unsigned i=0;i<60;i++) {
        CHECK(oficial.nodos[i].id == i && oficial.nodos[i].puerto == 27200+i%3);
        CHECK(oficial.nodos[i].memoria == UINT64_C(8388608));
        if (i%3) CHECK(oficial.nodos[i].ipv4_host == oficial.nodos[i-1].ipv4_host);
    }
    CHECK(dsm_config_base(&oficial,59) == UINT64_C(494927872));
    CHECK(dsm_config_cargar("config/lab.ini",&historica) == 0 && historica.cantidad_nodos == 2);
    CHECK(dsm_config_cargar("config/pool.ini",&historica) == 0 && historica.cantidad_nodos == 40);
    fputs("page_size=4096\nmemoria_inicial=8388608\nmax_nodos=60\nmax_hilos=4\n"
          "heartbeat_intervalo_ms=1000\nheartbeat_fallos=3\nrpc_timeout_ms=2000\n"
          "puerto_rango_inicio=27200\npuerto_rango_fin=27299\nequipo=127.0.0.1\n",f);
    for (unsigned i=2;i<=20;i++) fprintf(f,"equipo=198.51.100.%u\n",i);
    CHECK(fflush(f) == 0);
    char ruta[64];
    snprintf(ruta,sizeof ruta,"/proc/self/fd/%d",fileno(f));
    CHECK(dsm_config_cargar(ruta,&cfg) == 0 && cfg.cantidad_nodos == 60);
    CHECK(dsm_directorio_init(&dir,&cfg,0) == 0);
    uint64_t mascara = 0;
    const uint16_t ids[] = {40,58,59};
    for (size_t i=0;i<sizeof ids/sizeof ids[0];i++) {
        mascara |= UINT64_C(1) << ids[i];
        dsm_directorio_par_fuera(&dir,ids[i]);
        CHECK(dir.fuera == mascara);
    }
    dsm_directorio_par_fuera(&dir,60);
    dsm_directorio_par_fuera(&dir,UINT16_MAX);
    CHECK(dir.fuera == mascara);
    for (size_t i=0;i<sizeof ids/sizeof ids[0];i++) {
        mascara &= ~(UINT64_C(1) << ids[i]);
        dsm_directorio_par_linea(&dir,ids[i]);
        CHECK(dir.fuera == mascara);
    }
    dsm_directorio_cerrar(&dir);
    for (unsigned i=0;i<3;i++) {
        CHECK(pipe(h[i].orden) == 0 && pipe(h[i].respuesta) == 0);
        h[i].pid = fork();
        CHECK(h[i].pid >= 0);
        if (h[i].pid == 0) {
            uint16_t id = UINT16_MAX;
            int rc = dsm_pool_reclamar(ruta,&id);
            char b;
            if (rc != DSM_OK || io(h[i].respuesta[1],&id,sizeof id,1) != 0 ||
                io(h[i].orden[0],&b,1,0) != 0) _exit(1);
            dsm_pool_liberar();
            _exit(0);
        }
    }
    unsigned usados = 0;
    for (unsigned i=0;i<3;i++) {
        CHECK(io(h[i].respuesta[0],&h[i].id,sizeof h[i].id,0) == 0 && h[i].id < 3);
        CHECK(!(usados & (1u << h[i].id)));
        usados |= 1u << h[i].id;
    }
    CHECK(usados == 7);
    uint16_t cuarto = UINT16_MAX;
    CHECK(dsm_pool_reclamar(ruta,&cuarto) == DSM_ERR_OCUPADA);
    char salir = 1;
    CHECK(io(h[0].orden[1],&salir,1,1) == 0);
    int st;
    CHECK(waitpid(h[0].pid,&st,0) == h[0].pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    h[0].pid = 0;
    CHECK(dsm_pool_reclamar(ruta,&cuarto) == DSM_OK && cuarto == h[0].id);
    dsm_pool_liberar();
    puts("60puestos: 20 IPv4, 3 cupos locales, cuarto rechazado, reuso e IDs40/58/59 OK");
    resultado = 0;
fin:
    dsm_pool_liberar();
    for (unsigned i=0;i<3;i++) {
        if (h[i].pid > 0) { kill(h[i].pid,SIGKILL); waitpid(h[i].pid,NULL,0); }
        for (unsigned j=0;j<2;j++) {
            if (h[i].orden[j] >= 0) close(h[i].orden[j]);
            if (h[i].respuesta[j] >= 0) close(h[i].respuesta[j]);
        }
    }
    if (dir.inicializado) dsm_directorio_cerrar(&dir);
    if (f) fclose(f);
    return resultado;
}
