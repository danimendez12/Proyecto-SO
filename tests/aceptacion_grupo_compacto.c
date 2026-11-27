#include "../apps/demanda_grupo_privado.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static const uint16_t nodos[] = {40,58,59};
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"grupo_compacto:%d: %s\n",__LINE__,#x); goto fin; } } while (0)

static int codec(void)
{
    demanda_grupo_t g = {.yo=59,.lider=0,.cantidad=60,.nonce=11,.token=22,.digest=33};
    struct miembro m, copia;
    uint8_t buf[DSM_ANUNCIO_DATOS];
    size_t n = 0;
    for (unsigned i=0;i<60;i++) g.ids[i]=(uint16_t)i;
    if (demanda_miembro_desde_grupo(&g,ACEPTAR,&m) != DSM_OK ||
        demanda_miembro_codificar(&m,buf,sizeof buf,&n) != DSM_OK ||
        n != 104 || n > DSM_ANUNCIO_DATOS ||
        demanda_miembro_decodificar(buf,n,&copia) != DSM_OK || copia.cantidad != 60 ||
        copia.ids[40] != 40 || copia.ids[58] != 58 || copia.ids[59] != 59 ||
        copia.nonce != 11 || copia.token != 22 || copia.digest != 33) return -1;
    for (unsigned i=0;i<60;i++) if (copia.ids[i] != i) return -1;
    const unsigned indices[] = {40,58,59};
    const unsigned invalidos[] = {60,255};
    for (size_t i=0;i<3;i++) for (size_t j=0;j<2;j++) {
        struct miembro mala = m;
        mala.ids[indices[i]]=(uint8_t)invalidos[j];
        if (demanda_miembro_decodificar(&mala,sizeof mala,&copia) != DSM_ERR_PROTO) return -1;
    }
    struct miembro mala = m;
    mala.ids[59]=58;
    if (demanda_miembro_decodificar(&mala,sizeof mala,&copia) != DSM_ERR_PROTO) return -1;
    mala=m; mala.ids[40]=39;
    if (demanda_miembro_decodificar(&mala,sizeof mala,&copia) != DSM_ERR_PROTO) return -1;
    mala=m; mala.lider=60;
    if (demanda_miembro_decodificar(&mala,sizeof mala,&copia) != DSM_ERR_PROTO) return -1;
    mala=m; mala.cantidad=61;
    if (demanda_miembro_decodificar(&mala,sizeof mala,&copia) != DSM_ERR_PROTO) return -1;
    mala=m; mala.magic=0x44475232;
    if (demanda_miembro_decodificar(&mala,sizeof mala,&copia) != DSM_ERR_PROTO) return -1;
    mala=m; mala.nonce=0;
    if (demanda_miembro_decodificar(&mala,sizeof mala,&copia) != DSM_ERR_PROTO) return -1;
    if (demanda_miembro_decodificar(buf,n-1,&copia) != DSM_ERR_PROTO ||
        demanda_miembro_decodificar(buf,n+1,&copia) != DSM_ERR_PROTO) return -1;
    g.ids[59]=256;
    if (demanda_miembro_desde_grupo(&g,ACEPTAR,&copia) != DSM_ERR_INVAL) return -1;
    g.ids[59]=59; g.cantidad=61;
    if (demanda_miembro_desde_grupo(&g,ACEPTAR,&copia) != DSM_ERR_INVAL) return -1;
    dsm_cabecera_t cab={.version=DSM_PROTO_VERSION,.tipo=DSM_TIPO_ANUNCIO,
                        .origen=40,.destino=59,.id_msg=1};
    dsm_anuncio_t anuncio={.autor=40,.sesion=123,.generacion=1,.largo=n,.datos=buf};
    strcpy(anuncio.clave,"dg3:compacto");
    uint8_t trama[DSM_ANUNCIO_VERSIONADO_BASE+DSM_ANUNCIO_DATOS];
    size_t bytes;
    if (dsm_anuncio_versionado_codificar(trama,sizeof trama,&cab,&anuncio,&bytes) != 0 ||
        dsm_anuncio_versionado_decodificar(trama,bytes,&cab,&anuncio) != 0 ||
        demanda_miembro_decodificar(anuncio.datos,anuncio.largo,&copia) != DSM_OK ||
        copia.ids[59] != 59) return -1;
    puts("grupo_compacto: propuesta60 de104B, ANN real, formatoDGR3 e IDs invalidos rechazados OK");
    return 0;
}

static int io(int fd,void *buf,size_t n,int salida)
{
    unsigned char *p=buf;
    while(n) {
        ssize_t r=salida?write(fd,p,n):read(fd,p,n);
        if(r<0&&errno==EINTR)continue;
        if(r<=0)return -1;
        p+=r;
        n-=(size_t)r;
    }
    return 0;
}

struct informe { int rc; uint16_t lider,cantidad,ids[3]; uint64_t token,digest; };
struct recursos { uint32_t control,datos; };

static struct informe calcular(const char *ruta,uint16_t nodo,int salida,int entrada)
{
    struct informe informe={.rc=DSM_ERR_ESTADO};
    dsm_context_t *ctx=NULL;
    dsm_proc_id_t proc=0;
    demanda_grupo_t g={0};
    struct recursos r={0};
    int propia=0,abiertas[2]={0};
    int rc=dsm_iniciar(ruta,nodo,&ctx);
    if(rc==DSM_OK)rc=dsm_proc_abrir(ctx,&proc);
    io(salida,&rc,sizeof rc,1);
    char b;
    if(rc==DSM_OK&&io(entrada,&b,1,0)!=0)rc=DSM_ERR_ESTADO;
    setenv("DSM_DEMANDA_ESPERA_MS","500",1);
    setenv("DSM_DEMANDA_PLAZO_SEG","20",1);
    setenv("DSM_DEMANDA_GRUPO","aceptacion60",1);
    if(rc==DSM_OK)rc=demanda_grupo_iniciar(&g,ctx,nodo,"compacto:60:altos");
    if(rc==DSM_OK && (g.lider!=40||g.cantidad!=3||memcmp(g.ids,nodos,sizeof nodos)))rc=DSM_ERR_PROTO;
    propia=nodo==g.lider;
    uint32_t *ids[]={&r.control,&r.datos};
    for(unsigned i=0;i<2&&rc==DSM_OK;i++) if(propia) {
        void *base;
        rc=dsm_reservar(ctx,proc,i?12288:4096,ids[i],&base);
        abiertas[i]=*ids[i]!=0;
    }
    if(rc==DSM_OK&&propia) {
        demanda_fase_t inicial={0};
        rc=demanda_fase_publicar(&g,r.control,&inicial);
        if(rc==DSM_OK)rc=demanda_grupo_publicar(&g,&r,sizeof r);
    }
    if(rc==DSM_OK&&!propia) {
        rc=demanda_grupo_recibir(&g,&r,sizeof r);
        for(unsigned i=0;i<2&&rc==DSM_OK;i++) {
            void *base;size_t bytes;
            rc=dsm_abrir_reserva(ctx,*ids[i],&base,&bytes);abiertas[i]=1;
            if(rc==DSM_OK&&bytes!=(i?12288u:4096u))rc=DSM_ERR_PROTO;
        }
    }
    unsigned char valor=(unsigned char)(0x40+nodo);
    if(rc==DSM_OK)rc=dsm_escribir(ctx,r.datos,g.rango*4096u,&valor,1);
    if(rc==DSM_OK)rc=demanda_marcar(&g,r.control,1,DSM_OK);
    if(rc==DSM_OK&&propia)rc=demanda_marcas_esperar(&g,r.control,1);
    if(rc==DSM_OK&&propia)for(unsigned i=0;i<3&&rc==DSM_OK;i++) {
        unsigned char v=0;rc=dsm_leer(ctx,r.datos,i*4096u,&v,1);
        if(rc==DSM_OK&&v!=(unsigned char)(0x40+nodos[i]))rc=DSM_ERR_ESTADO;
    }
    demanda_fase_t fase={.generacion=1,.tipo=DEMANDA_FASE_CERRAR};
    if(rc==DSM_OK)rc=propia?demanda_fase_publicar(&g,r.control,&fase):
                                demanda_fase_esperar(&g,r.control,1,&fase);
    if(rc==DSM_OK&&!propia) {
        rc=demanda_reserva_retirar(ctx,&r.datos,0,demanda_ahora_ms()+8000);
        if(rc==DSM_OK)abiertas[1]=0;
    }
    if(rc==DSM_OK)rc=demanda_marcar(&g,r.control,2,DSM_OK);
    if(rc==DSM_OK&&propia)rc=demanda_marcas_esperar(&g,r.control,2);
    if(rc!=DSM_OK)demanda_grupo_error(&g,rc);
    uint64_t limite=demanda_ahora_ms()+8000;
    for(unsigned i=2;i-->0;) if(abiertas[i]) {
        int otro=demanda_reserva_retirar(ctx,ids[i],propia,limite);
        if(rc==DSM_OK)rc=otro;
    }
    int otro=demanda_grupo_retirar(&g);if(rc==DSM_OK)rc=otro;
    otro=demanda_contexto_cerrar(&ctx,limite);if(rc==DSM_OK)rc=otro;
    informe.rc=rc;informe.lider=g.lider;informe.cantidad=g.cantidad;
    memcpy(informe.ids,g.ids,sizeof informe.ids);informe.token=g.token;informe.digest=g.digest;
    return informe;
}

int main(void)
{
    alarm(45);setvbuf(stdout,NULL,_IOLBF,0);
    int resultado=1;
    FILE *f=tmpfile();
    struct hijo {pid_t pid;int entrada[2],salida[2];} h[3]={
        {.entrada={-1,-1},.salida={-1,-1}}, {.entrada={-1,-1},.salida={-1,-1}},
        {.entrada={-1,-1},.salida={-1,-1}}};
    CHECK(codec()==0&&f!=NULL);
    fputs("page_size=4096\nmemoria_inicial=65536\nmax_nodos=60\nmax_hilos=4\n"
          "heartbeat_intervalo_ms=250\nheartbeat_fallos=8\nrpc_timeout_ms=1000\n"
          "puerto_rango_inicio=27200\npuerto_rango_fin=27299\n",f);
    for(unsigned i=0;i<60;i++)fprintf(f,"nodo=%u compacto-%u 127.0.0.1 %u\n",i,i,27240+i);
    CHECK(fflush(f)==0);
    char ruta[64];snprintf(ruta,sizeof ruta,"/proc/self/fd/%d",fileno(f));
    for(unsigned i=0;i<3;i++) {
        CHECK(pipe(h[i].entrada)==0&&pipe(h[i].salida)==0);
        h[i].pid=fork();CHECK(h[i].pid>=0);
        if(!h[i].pid) {
            close(h[i].entrada[1]);close(h[i].salida[0]);
            struct informe r=calcular(ruta,nodos[i],h[i].salida[1],h[i].entrada[0]);
            io(h[i].salida[1],&r,sizeof r,1);_exit(r.rc==DSM_OK?0:1);
        }
        close(h[i].entrada[0]);h[i].entrada[0]=-1;
        close(h[i].salida[1]);h[i].salida[1]=-1;
    }
    for(unsigned i=0;i<3;i++) {int rc;CHECK(io(h[i].salida[0],&rc,sizeof rc,0)==0&&rc==DSM_OK);}
    for(unsigned i=0;i<3;i++) {char b=1;CHECK(io(h[i].entrada[1],&b,1,1)==0);}
    uint64_t token=0,digest=0;
    for(unsigned i=0;i<3;i++) {
        struct pollfd p={.fd=h[i].salida[0],.events=POLLIN};
        CHECK(poll(&p,1,30000)>0);
        struct informe r;
        CHECK(io(h[i].salida[0],&r,sizeof r,0)==0&&r.rc==DSM_OK&&r.cantidad==3&&r.lider==40&&
              memcmp(r.ids,nodos,sizeof nodos)==0&&r.token!=0);
        if(!i){token=r.token;digest=r.digest;}else CHECK(token==r.token&&digest==r.digest);
        int st;CHECK(waitpid(h[i].pid,&st,0)==h[i].pid&&WIFEXITED(st)&&WEXITSTATUS(st)==0);h[i].pid=0;
    }
    puts("grupo_compacto: acuerdo real40/58/59, digest canonico, marcas DSM, MSI y liberacion OK");
    resultado=0;
fin:
    for(unsigned i=0;i<3;i++) {
        if(h[i].pid>0){kill(h[i].pid,SIGKILL);waitpid(h[i].pid,NULL,0);}
        for(unsigned j=0;j<2;j++){if(h[i].entrada[j]>=0)close(h[i].entrada[j]);if(h[i].salida[j]>=0)close(h[i].salida[j]);}
    }
    if(f)fclose(f);
    return resultado;
}
