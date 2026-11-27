#include "api_prueba.h"
#include "protocol.h"

#include <stdatomic.h>
#include <sys/socket.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"cierre_perdido:%d: %s\n",__LINE__,#x); goto fin; } } while (0)

_Static_assert(ATOMIC_INT_LOCK_FREE == 2,"Inyeccion de ACK requiere atomicos lock-free");
static atomic_int omitir_ack;
static int aviso_fd = -1;

static void armar(int signo)
{
    (void)signo;
    atomic_store_explicit(&omitir_ack,1,memory_order_relaxed);
    char b=1;
    if(aviso_fd>=0)(void)write(aviso_fd,&b,1);
}

ssize_t __real_send(int fd,const void *buf,size_t bytes,int flags);
ssize_t __wrap_send(int fd,const void *buf,size_t bytes,int flags)
{
    dsm_cabecera_t cab;
    if(bytes>=DSM_CABECERA_BYTES && atomic_load_explicit(&omitir_ack,memory_order_relaxed) &&
       dsm_cabecera_decodificar(buf,DSM_CABECERA_BYTES,&cab)==0 &&
       cab.tipo==DSM_TIPO_INVALIDATE_ACK &&
       atomic_exchange_explicit(&omitir_ack,0,memory_order_relaxed)) return (ssize_t)bytes;
    return __real_send(fd,buf,bytes,flags);
}

int main(void)
{
    alarm(30);setvbuf(stdout,NULL,_IOLBF,0);signal(SIGPIPE,SIG_IGN);
    int resultado=1,detenido=0;
    FILE *f=tmpfile();
    int aviso[2]={-1,-1};
    struct api_proceso cliente={0},hogar={0};
    struct api_orden orden;
    struct api_respuesta r;
    CHECK(f!=NULL && pipe(aviso)==0);
    aviso_fd=aviso[1];
    CHECK(signal(SIGUSR2,armar)!=SIG_ERR);
    fputs("page_size=4096\nmemoria_inicial=65536\nmax_nodos=2\nmax_hilos=4\n"
          "heartbeat_intervalo_ms=250\nheartbeat_fallos=3\nrpc_timeout_ms=400\n"
          "puerto_rango_inicio=27200\npuerto_rango_fin=27299\n"
          "nodo=0 cliente 127.0.0.1 27200\nnodo=1 hogar 127.0.0.1 27201\n",f);
    CHECK(fflush(f)==0);
    char ruta[64];snprintf(ruta,sizeof ruta,"/proc/self/fd/%d",fileno(f));
    CHECK(api_iniciar_proceso(&cliente,ruta,0)==0 && api_iniciar_proceso(&hogar,ruta,1)==0);
    CHECK(api_operar(&hogar,API_INICIAR,DSM_OK)==0 && api_operar(&cliente,API_INICIAR,DSM_OK)==0);
    orden=(struct api_orden){.operacion=API_RESERVAR,.bytes=4096};
    CHECK(api_ejecutar(&hogar,&orden,&r)==0 && r.resultado==DSM_OK);
    dsm_reserva_id_t id=r.id;
    for(unsigned i=0;i<60 && !api_tiene_traza(&cliente,"hello 1 welcome 1");i++) {
        const struct timespec p={.tv_nsec=50000000};nanosleep(&p,NULL);
    }
    CHECK(api_tiene_traza(&cliente,"hello 1 welcome 1"));
    orden=(struct api_orden){.operacion=API_ABRIR,.id=id};
    CHECK(api_ejecutar(&cliente,&orden,&r)==0 && r.resultado==DSM_OK);
    orden=(struct api_orden){.operacion=API_ESCRIBIR,.bytes=1,.datos={0x65}};
    CHECK(api_ejecutar(&cliente,&orden,&r)==0 && r.resultado==DSM_OK);
    orden=(struct api_orden){.operacion=API_PERMISOS,.bytes=1};
    CHECK(api_ejecutar(&cliente,&orden,&r)==0 && strcmp(r.permisos[0],"rw-s")==0);
    /* M fue concedido e instalado realmente; ahora su devolucion se vuelve
     * ambigua al detener el poseedor durante la bajada pedida por el hogar. */
    CHECK(kill(cliente.pid,SIGSTOP)==0);detenido=1;
    int st;CHECK(waitpid(cliente.pid,&st,WUNTRACED)==cliente.pid&&WIFSTOPPED(st));
    orden=(struct api_orden){.operacion=API_LEER,.bytes=1};
    CHECK(api_ejecutar(&hogar,&orden,&r)==0 &&
          (r.resultado==DSM_ERR_TIEMPO||r.resultado==DSM_ERR_NODO));
    const struct timespec pausa={.tv_sec=1};nanosleep(&pausa,NULL);
    CHECK(kill(cliente.pid,SIGCONT)==0);detenido=0;
    for(unsigned i=0;i<100 && api_contar_traza(&cliente,"hello 1 welcome 1")<2;i++) {
        const struct timespec p={.tv_nsec=50000000};nanosleep(&p,NULL);
    }
    CHECK(api_contar_traza(&cliente,"hello 1 welcome 1")>=2);
    CHECK(api_ejecutar(&hogar,&orden,&r)==0 && r.resultado==DSM_ERR_NODO);
    CHECK(api_ejecutar(&cliente,&orden,&r)==0 && r.resultado==DSM_ERR_NODO);
    orden=(struct api_orden){.operacion=API_PERMISOS,.bytes=1};
    CHECK(api_ejecutar(&cliente,&orden,&r)==0 && strcmp(r.permisos[0],"---s")==0);
    CHECK(kill(cliente.pid,SIGUSR2)==0);
    struct pollfd p={.fd=aviso[0],.events=POLLIN};
    CHECK(poll(&p,1,2000)>0);
    char b;CHECK(read(aviso[0],&b,1)==1);
    orden=(struct api_orden){.operacion=API_DESVINCULAR,.id=id};
    CHECK(api_ejecutar(&cliente,&orden,&r)==0 && r.resultado==DSM_ERR_TIEMPO);
    orden=(struct api_orden){.operacion=API_LIBERAR,.id=id};
    CHECK(api_ejecutar(&hogar,&orden,&r)==0 && r.resultado==DSM_ERR_OCUPADA);
    puts("cierre_perdido: ACK descartado conserva pin y pagina aislada OK");
    orden=(struct api_orden){.operacion=API_DESVINCULAR,.id=id};
    CHECK(api_ejecutar(&cliente,&orden,&r)==0 && r.resultado==DSM_OK);
    orden=(struct api_orden){.operacion=API_LEER,.bytes=1};
    CHECK(api_ejecutar(&hogar,&orden,&r)==0 && r.resultado==DSM_ERR_NODO);
    orden=(struct api_orden){.operacion=API_PERMISOS,.bytes=1};
    CHECK(api_ejecutar(&hogar,&orden,&r)==0 && strcmp(r.permisos[0],"---s")==0);
    orden=(struct api_orden){.operacion=API_LIBERAR,.id=id};
    CHECK(api_ejecutar(&hogar,&orden,&r)==0 && r.resultado==DSM_OK);
    orden=(struct api_orden){.operacion=API_RESERVAR,.bytes=4096};
    CHECK(api_ejecutar(&hogar,&orden,&r)==0 && r.resultado==DSM_OK && r.id!=id);
    id=r.id;
    orden=(struct api_orden){.operacion=API_LEER,.bytes=1};
    CHECK(api_ejecutar(&hogar,&orden,&r)==0 && r.resultado==DSM_OK && r.datos[0]==0);
    orden=(struct api_orden){.operacion=API_ABRIR,.id=id};
    CHECK(api_ejecutar(&cliente,&orden,&r)==0 && r.resultado==DSM_OK);
    orden=(struct api_orden){.operacion=API_ESCRIBIR,.bytes=1,.datos={0x76}};
    CHECK(api_ejecutar(&cliente,&orden,&r)==0 && r.resultado==DSM_OK);
    orden=(struct api_orden){.operacion=API_DESVINCULAR,.id=id};
    CHECK(api_ejecutar(&cliente,&orden,&r)==0 && r.resultado==DSM_OK);
    orden=(struct api_orden){.operacion=API_LEER,.bytes=1};
    CHECK(api_ejecutar(&hogar,&orden,&r)==0 && r.resultado==DSM_OK && r.datos[0]==0x76);
    orden=(struct api_orden){.operacion=API_LIBERAR,.id=id};
    CHECK(api_ejecutar(&hogar,&orden,&r)==0 && r.resultado==DSM_OK);
    CHECK(api_operar(&cliente,API_CERRAR,DSM_OK)==0 && api_operar(&hogar,API_CERRAR,DSM_OK)==0);
    puts("cierre_perdido: revocacion confirmada, sin recuperacion ficticia, nueva identidad y reuso OK");
    resultado=0;
fin:
    if(detenido)kill(cliente.pid,SIGCONT);
    if(api_detener_proceso(&cliente)!=0)resultado=1;
    if(api_detener_proceso(&hogar)!=0)resultado=1;
    if(f)fclose(f);
    if(aviso[0]>=0)close(aviso[0]);
    if(aviso[1]>=0)close(aviso[1]);
    return resultado;
}
