#include "api_prueba.h"

#define MIB (1024u*1024u)
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"asignacion_disponibles:%d: %s\n",__LINE__,#x); goto fin; } } while (0)

static uint64_t ahora_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec;
}

struct alta_tardia { struct api_proceso *par; int rc; };
static void *incorporar(void *arg)
{
    struct alta_tardia *a=arg;
    const struct timespec pausa={.tv_nsec=350000000};
    nanosleep(&pausa,NULL); /* Lateness deliberada del fixture, no criterio de exito. */
    a->rc=api_operar(a->par,API_INICIAR,DSM_OK);
    return NULL;
}

static int escenario(uint16_t hogar,int tardio)
{
    int resultado=1,iniciado=0;
    FILE *f=tmpfile();
    struct api_proceso par={0};
    dsm_context_t *ctx=NULL;
    dsm_proc_id_t proc=0;
    dsm_reserva_id_t id=0,extra=0;
    void *base;
    pthread_t hilo;
    struct alta_tardia alta={.par=&par,.rc=-1};
    CHECK(f!=NULL);
    fputs("page_size=4096\nmemoria_inicial=8388608\nmax_nodos=60\nmax_hilos=4\n"
          "heartbeat_intervalo_ms=1000\nheartbeat_fallos=3\nrpc_timeout_ms=2000\n"
          "puerto_rango_inicio=27200\npuerto_rango_fin=27299\n",f);
    for(unsigned i=0;i<60;i++)fprintf(f,"nodo=%u disponible-%u %s %u\n",i,i,
        i==hogar&&tardio?"127.0.0.2":"127.0.0.1",27240+i);
    CHECK(fflush(f)==0);
    char ruta[64];snprintf(ruta,sizeof ruta,"/proc/self/fd/%d",fileno(f));
    /* Fork antes de crear contexto/hilos en el padre. */
    CHECK(api_iniciar_proceso(&par,ruta,hogar)==0);
    if(!tardio)CHECK(api_operar(&par,API_INICIAR,DSM_OK)==0);
    CHECK(dsm_iniciar(ruta,0,&ctx)==DSM_OK&&dsm_proc_abrir(ctx,&proc)==DSM_OK);
    CHECK(dsm_reservar(ctx,proc,4096,&id,&base)==DSM_OK);
    uint16_t donde;uint64_t gpn;
    CHECK(dsm_ubicacion_pagina(ctx,id,0,&donde,&gpn)==DSM_OK&&donde==0);
    CHECK(dsm_liberar(ctx,id)==DSM_OK);id=0;
    if(tardio) {CHECK(pthread_create(&hilo,NULL,incorporar,&alta)==0);iniciado=1;}
    else CHECK(dsm_sondear(ctx,500)==DSM_OK);
    uint64_t inicio=ahora_ns();
    int rc=dsm_reservar(ctx,proc,15u*MIB,&id,&base);
    uint64_t duracion=ahora_ns()-inicio;
    if(iniciado){pthread_join(hilo,NULL);iniciado=0;}
    CHECK(rc==DSM_OK&&duracion<UINT64_C(5000000000)&&(!tardio||alta.rc==0));
    printf("ASIGNACION activos=0,%u tardio=%d bytes=15728640 duracion_ns=%"PRIu64" DSM=%d\n",
           hogar,tardio,duracion,rc);
    CHECK(dsm_ubicacion_pagina(ctx,id,2047,&donde,&gpn)==DSM_OK&&donde==0&&gpn==2047);
    CHECK(dsm_ubicacion_pagina(ctx,id,2048,&donde,&gpn)==DSM_OK&&donde==hogar&&gpn==(uint64_t)hogar*2048u);
    CHECK(dsm_ubicacion_pagina(ctx,id,3839,&donde,&gpn)==DSM_OK&&donde==hogar&&gpn==(uint64_t)hogar*2048u+1791u);
    unsigned char datos[64],copia[64];
    for(unsigned i=0;i<64;i++)datos[i]=(unsigned char)(i+hogar);
    CHECK(dsm_escribir(ctx,id,8u*MIB-32u,datos,sizeof datos)==DSM_OK);
    CHECK(dsm_leer(ctx,id,8u*MIB-32u,copia,sizeof copia)==DSM_OK&&memcmp(datos,copia,sizeof datos)==0);
    CHECK(dsm_escribir(ctx,id,15u*MIB-64u,datos,sizeof datos)==DSM_OK);
    CHECK(dsm_leer(ctx,id,15u*MIB-64u,copia,sizeof copia)==DSM_OK&&memcmp(datos,copia,sizeof datos)==0);
    CHECK(dsm_reservar(ctx,proc,4096,&extra,&base)==DSM_OK&&extra!=id);
    CHECK(dsm_liberar(ctx,extra)==DSM_OK);extra=0;
    CHECK(dsm_liberar(ctx,id)==DSM_OK);id=0;
    /* Insuficiencia tras preparar ambos hogares: debe cancelar los aportes. */
    inicio=ahora_ns();
    rc=dsm_reservar(ctx,proc,17u*MIB,&id,&base);
    duracion=ahora_ns()-inicio;
    CHECK((rc==DSM_ERR_NODO||rc==DSM_ERR_NOMEM)&&duracion<UINT64_C(5000000000)&&id==0);
    CHECK(dsm_reservar(ctx,proc,16u*MIB,&id,&base)==DSM_OK);
    CHECK(dsm_liberar(ctx,id)==DSM_OK);id=0;
    CHECK(api_tiene_traza(&par,"hello 1 welcome 1"));
    CHECK(dsm_cerrar(ctx)==DSM_OK);ctx=NULL;
    CHECK(api_operar(&par,API_CERRAR,DSM_OK)==0);
    resultado=0;
fin:
    if(iniciado)pthread_join(hilo,NULL);
    if(ctx){if(extra)dsm_liberar(ctx,extra);if(id)dsm_liberar(ctx,id);dsm_cerrar(ctx);}
    if(api_detener_proceso(&par)!=0)resultado=1;
    if(f)fclose(f);
    return resultado;
}

int main(void)
{
    alarm(35);setvbuf(stdout,NULL,_IOLBF,0);
    if(escenario(59,0)!=0||escenario(58,1)!=0)return 1;
    puts("asignacion_disponibles: 60 configurados, IDs altos, otraIPv4 tardia, rollback y reuso16MiB OK");
    return 0;
}
