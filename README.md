# Proyecto-SO

Proyecto de los entregables 2 y 3 del curso IC-6600 Principios de Sistemas Operativos.
El objetivo es implementar en C para Linux un sistema de Memoria Compartida
Distribuida (DSM) que permita a procesos en distintos nodos acceder a una región
de memoria compartida.

El Prototipo 2 permite compartir una reserva contigua entre aplicaciones C
independientes en Linux x86-64. Varios nodos pueden aportar páginas a la
misma reserva. Un fallo en la vista de aplicación activa al paginador y
el hogar de la página coordina por TCP sus permisos MSI.

## Entrega final

Cada terminal reclama un puesto del INI común. Para iniciar un donante usá
`./bin/dsm_unir config/pool-lab.ini`. Matrices y Merge Sort también reclaman su
puesto y acuerdan los participantes. La plantilla contempla 20 equipos y
40 puestos; reemplazá las IP de ejemplo antes de ejecutarla en el laboratorio.

[Guía de instalación y demostración](docs/guia-laboratorio.md) ·
[Arquitectura, MSI y protocolo](docs/arquitectura-final.md) ·
[Validación del Paso 13B](docs/validacion-13b.md)

## Organización

| Directorio | Contenido |
| --- | --- |
| `include/` | Cabeceras de los módulos. |
| `src/` | Módulos en C y punto de entrada del nodo (`main.c`). |
| `examples/` | Aplicaciones creadora y lectora que usan el API público. |
| `apps/matrices/` | Multiplicación de matrices con Pthreads, reservas DSM multihogar y verificación numérica. |
| `apps/mergesort/` | Merge Sort con Pthreads, fusión por niveles en DSM y comparación exhaustiva con una referencia independiente. |
| `tests/` | Pruebas de aceptación de red, memoria, directorio y paginación. |
| `config/` | INI común del laboratorio y configuraciones internas de aceptación. |
| `docs/` | Documentación técnica y evidencias del laboratorio. |
| `bin/` | Ejecutables generados durante la compilación. |

## Módulos

| Módulo | Responsabilidad |
| --- | --- |
| `dsm` | Coordinar el ciclo de vida del nodo y sus módulos. |
| `config` | Cargar y validar el INI y expandir los equipos a puestos. |
| `pool` | Reclamar un puesto cuyo endpoint sea local. |
| `aviso` | Publicar, retirar y reproducir anuncios de la sesión del autor. |
| `protocol` | Definir el formato y la codificación de los mensajes entre nodos. |
| `network` | Gestionar las conexiones TCP, los latidos y las reincorporaciones. |
| `coherence` | Correlacionar las solicitudes del paginador con el hogar y aplicar las respuestas al directorio local. |
| `memory` | Administrar la región de memoria local y las protecciones de página. |
| `directory` | Mantener los metadatos de páginas y sus estados de coherencia. |
| `pager` | Coordinar la atención de fallos de página y la recuperación de datos. |
| `signals` | Gestionar las señales asociadas con los fallos de acceso a memoria. |

## Compilación en Linux

Se requiere GCC con soporte para C11, GNU Make y el soporte para POSIX Threads
de la herramienta de compilación. Los objetivos de ejemplos y pruebas compilan
los ejecutables correspondientes.

| Comando | Resultado |
| --- | --- |
| `make` | Compila el nodo, `dsm_unir` y ambas aplicaciones. |
| `make examples` | Genera los ejemplos en `bin/examples/`. |
| `make apps` | Compila matrices, su referencia secuencial original y Merge Sort con sus modos distribuido y secuencial. |
| `make tests` | Genera los programas de prueba en `bin/tests/`. |
| `make aceptacion` | Compila las 17 aceptaciones previas y las regresiones del Paso 13B. |
| `make clean` | Elimina el directorio de salida `bin/`. |

## Demostración de paginación remota

Compilá con `make` y ejecutá un comando en cada terminal desde la raíz del
proyecto.

~~~bash
# Terminal 1, creador en el puerto 27200
./bin/dsm_node config/lab.ini 0 servir

# Terminal 2, lector en el puerto 27201
./bin/dsm_node config/lab.ini 1 servir 1
~~~

Cada proceso crea una reserva local de una página y escribe `Hola TEC`. El
último `1` del segundo comando es el ID de la reserva creada por el nodo 0.
El nodo lector abre la reserva con `REGION_LOOKUP` y accede directamente a Ar.
El fallo de lectura genera un `PAGE_REQUEST` TCP de 48 bytes. El hogar retira
la escritura de su página y envía `PAGE_DATA`, con 4176 bytes de trama para
una carga de 4096 bytes. La traza muestra los cambios `M → S` e `I → S` y
dos lecturas de `Hola TEC` con una transferencia. Ambos nodos siguen sirviendo
peticiones. Cerrá cada proceso con Ctrl+C.

La forma `./bin/dsm_node config/lab.ini 0` muestra la configuración y termina.

## Configuración del laboratorio

El rango de escucha asignado es **27200–27299**. Cada nodo se identifica por
su ID global y escucha en la IPv4 y el puerto de su entrada. Una misma IPv4
puede corresponder a varios procesos con puertos distintos. Los puertos de
origen de las conexiones salientes los elige Linux.

La plantilla oficial es `config/pool-lab.ini`. Incluye las IPv4 potenciales
y asigna puestos al inicio de cada proceso. Los archivos `lab.ini` y
`lab-ocho.ini` conservan el formato de puestos explícitos para pruebas
internas. En ese formato cada entrada se escribe así.

~~~text
nodo = id nombre IPv4 puerto [memoria_bytes]
~~~

Si omitís la contribución, se usa `memoria_inicial`. Las bases lógicas se
calculan sumando los aportes de los IDs anteriores. La capacidad de
configuración es de hasta 40 nodos, limitada en cada archivo por `max_nodos`.
El menor ID inicia cada conexión y reintenta el alta. El mayor conserva su
puerto de escucha para sus pares.

## Demostración de escritura coherente

Ejecutá ambos procesos desde la raíz del proyecto y dejá ambas terminales
abiertas. El nodo 0 crea la reserva 1 e inicializa la página con `Hola TEC`.
El último argumento del nodo 1 indica qué reserva abre.

~~~bash
# Terminal del nodo 0
./bin/dsm_node config/lab.ini 0 interactivo

# Terminal del nodo 1
./bin/dsm_node config/lab.ini 1 interactivo 1
~~~

Cuando ambos indiquen `listo reserva 1`, ingresá un comando por línea en la
terminal indicada y esperá su resultado antes de pasar al siguiente.

| Paso | Terminal | Comando | Resultado observable |
| --- | --- | --- | --- |
| 1 | Nodo 1 | `leer` | Lee `Hola TEC` tras recibir 4096 bytes. |
| 2 | Ambas | `estado` | Ambas copias están S, versión 1. |
| 3 | Nodo 1 | `escribir` | Un fallo de escritura sobre Ar inicia la exclusividad y la instrucción escribe `Adios TEC`. |
| 4 | Ambas | `estado` | Nodo 0 local I y nodo 1 M. El hogar identifica al poseedor 1 en la versión 2. |
| 5 | Nodo 0 | `leer` | El poseedor devuelve su imagen y el hogar lee `Adios TEC`. |
| 6 | Ambas | `estado` | S en ambos, versión 3. Otra lectura local conserva estos permisos. |
| 7 | Ambas | `salir` | Cada proceso desregistra su trabajador y cierra el servicio. |

Para repetirlo en dos computadoras Linux Mint, usá el mismo código e INI en
ambas. Cambiá únicamente las dos IPv4 de `config/lab.ini` por las de los
equipos y ejecutá cada comando en la máquina del nodo correspondiente.
Los puertos configurados son 27200 y 27201. El modo `servir` del Prototipo 1
sigue disponible con su interfaz anterior.

## Paginador y directorio del Prototipo 2

En Linux x86-64, el manejador distingue la lectura de la escritura con el bit
de escritura de `REG_ERR`. Publica el fallo en la ranura del trabajador,
notifica al paginador con `sem_post` y espera con `sigsuspend`. El paginador
reanuda al hilo con `SIGUSR1`. Un fallo ajeno a la ventana o una solicitud
que fracasa termina el proceso con `SIGSEGV`.

El paginador distribuido delega cada fallo al directorio a través de
`coherence`. El directorio controla conjuntamente los datos, los metadatos y
el permiso real de Ar. I corresponde a `PROT_NONE`, S a `PROT_READ` y M a
`PROT_READ | PROT_WRITE`. El modo MSI local del Paso 10 conserva su adaptador
para ejercitar las concesiones con un proveedor controlado.

El hogar serializa las solicitudes de su página. Para una promoción remota
desde S, el escritor inicia la operación pidiendo al hogar retirar su propia
copia mediante `INVALIDATE`. El ACK identifica la solicitud de escritura.
Al procesar `PAGE_REQUEST`, el hogar vuelve a examinar todas las copias,
invalida las réplicas y espera sus confirmaciones. Una contribución en S
puede conservar una imagen vigente en Cr aunque su Ar esté revocada.
Cuando el poseedor M es remoto, el hogar le pide `PAGE_DOWNGRADE`, recibe
la imagen vigente y su confirmación, y responde al solicitante. Una
promoción S vigente puede recibir una concesión sin carga. Desde I se exige
la imagen completa antes de habilitar Ar.

Cada conexión tiene un lector. Las respuestas de página se correlacionan e
instalan en ese lector. Dos colas por nodo separan las campañas iniciadas
como hogar de las revocaciones que debe atender como réplica. Una página
con poseedor perdido queda indisponible. Una copia compartida desconectada
sigue contando hasta que el hogar obtiene una confirmación de invalidación.

## API público del Prototipo 2

Cada aplicación incluye `dsm.h` e inicia su nodo con `dsm_iniciar`. El mismo
proceso escucha por TCP, administra sus propias vistas y atiende sus fallos
de página. Hay un contexto por proceso. Cada contexto admite varias reservas.

| Operación | Semántica |
| --- | --- |
| Crear | `dsm_proc_abrir` obtiene el ID de proceso. `dsm_reservar` devuelve un ID global y una Ar contigua. El creador usa su capacidad libre y pide el resto a otros hogares. |
| Abrir | `dsm_abrir_reserva` consulta al creador por TCP, recibe el manifiesto y crea vistas locales. Las páginas remotas comienzan en I. Reabrir el mismo ID devuelve la misma Ar y el tamaño lógico. Basta una desvinculación por nodo. |
| Acceder | Registrá el hilo antes de usar directamente Ar o el puntero de `dsm_traducir`. Cada fallo resuelve el GPN de esa página. La aplicación sincroniza los accesos directos concurrentes a los mismos datos. |
| Consultar el mapa | `dsm_ubicacion_pagina(ctx, id, orden, &hogar, &gpn)` devuelve el hogar y el GPN del ordinal lógico, empezando en cero. Está disponible al crear o abrir la reserva. |
| Copiar | `dsm_leer` y `dsm_escribir` validan el rango lógico y adquieren permisos MSI por página; admiten cruces, solapamiento como `memmove` y registro temporal del hilo. Una lectura a memoria ordinaria conserva el destino si falla el DSM. Una escritura intenta restaurar localmente cada trozo solo si conserva M, la misma versión y los bytes que escribió, sin RPC ni recuperar permisos; si hubo invalidación, pérdida o cambios ajenos puede quedar una copia parcial y se devuelve el fallo original. Leer hacia otra vista DSM también puede dejar trozos modificados al fallar. Las operaciones no son transacciones ni instantáneas distribuidas. La aplicación debe sincronizar los accesos directos Ar locales solapados y los buffers ordinarios durante toda la operación, incluida su restauración. |
| Desvincular | `dsm_desvincular_reserva` devuelve las copias S o M a sus respectivos hogares y confirma el cierre con el creador. El nodo que también aportó páginas continúa sirviéndolas. |
| Liberar | Solo el creador puede llamar a `dsm_liberar`. Una apertura remota vigente devuelve `DSM_ERR_OCUPADA`. Después de despublicar, el creador espera la devolución de capacidad en cada hogar. Ante un timeout, reintentá con el mismo ID. |
| Cerrar | Desregistrá los trabajadores y llamá a `dsm_cerrar`. El contexto desvincula las vistas de cliente y libera las reservas propias. Si el nodo aún aporta páginas a otro creador, devuelve `DSM_ERR_OCUPADA`. Un cierre parcial puede haber retirado otras reservas. Al completar el cierre se detienen los hilos y se restauran las señales. |

El tamaño lógico es el solicitado, aunque el respaldo use páginas completas.
Los punteros directos deben mantenerse dentro del rango lógico. Linux solo
puede proteger páginas completas, incluido el relleno de la última. Si un
fallo directo sobre Ar resulta irresoluble, el proceso recibe SIGSEGV. Para
obtener un código de error de la operación, usá las funciones de copia.

| Código del API | Situación |
| --- | --- |
| `DSM_ERR_INVAL` | Tamaño cero al reservar, ID inexistente o rango lógico inválido. |
| `DSM_ERR_NOMEM` | La capacidad o los tramos disponibles entre los participantes son insuficientes, o falla la creación de las vistas. |
| `DSM_ERR_NODO` | El par está desconectado o la página quedó aislada al perder el hogar. |
| `DSM_ERR_TIEMPO` | Una solicitud enviada quedó sin respuesta dentro del plazo. |
| `DSM_ERR_PARCIAL` | Una consulta global de páginas o impresión solo pudo observar una parte de los puestos configurados. |
| `DSM_ERR_OCUPADA` | Hay trabajadores o aperturas que impiden retirar la reserva. |

Después de perder el hogar, una vista antigua permanece protegida aunque se
restablezca TCP. El ID de nodo carece de una época persistente que identifique
la misma instancia del creador. Una página cuyo último poseedor M se perdió
puede quedar indisponible.

El manifiesto contiene tramos de GPN con su ordinal inicial. Cada tramo
pertenece a un solo hogar y puede cubrir miles de páginas. Una reserva admite
hasta 16 tramos por hogar, hasta 40 hogares configurados y 256 tramos en total. Cada hogar administra
su capacidad y el MSI de sus páginas. El creador publica el mapa completo
después de que todos hayan confirmado la asignación.

### Dos aplicaciones desde terminales independientes

En Ubuntu, desde la raíz del proyecto, compilá con `make examples` y ejecutá
`DSM_INI=config/lab.ini` en cada terminal para usar los dos nodos en loopback.

En **dos computadoras Linux Mint**, compilá la misma revisión en ambas y
ejecutá este bloque en cada equipo. Ingresá las mismas dos direcciones en el
mismo orden. Cada terminal genera su propia copia del INI. Se conservan los
puertos TCP 27200 y 27201 y el tamaño de página de 4096.

```bash
make examples
read -rp 'IPv4 real del nodo 0: ' DSM_IP0
read -rp 'IPv4 real del nodo 1: ' DSM_IP1
DSM_INI=$(mktemp)
awk -v a="$DSM_IP0" -v b="$DSM_IP1" \
  '$1 == "nodo" && $3 == 0 {$5=a} $1 == "nodo" && $3 == 1 {$5=b} {print}' \
  config/lab.ini > "$DSM_INI"
```

| Escenario | Terminal del creador, nodo 0 | Terminal del cliente, nodo 1 |
| --- | --- | --- |
| Una página | `./bin/examples/demo_creator "$DSM_INI" compartir 0 1 4096` | `./bin/examples/demo_reader "$DSM_INI" 1 1 4096` |
| Tres páginas | `./bin/examples/demo_creator "$DSM_INI" compartir 0 3 4096` | `./bin/examples/demo_reader "$DSM_INI" 1 1 4096` |

Arrancá el creador primero y esperá que anuncie el ID de reserva. El segundo
`1` del cliente es ese ID, que vale 1 para la primera reserva del nodo 0 en
un contexto nuevo. El último argumento de ambos comandos debe coincidir con
`page_size` del INI. Ejecutá un escenario completo antes de iniciar el otro.

1. El cliente muestra los datos iniciales. Ingresá `escribir` en su terminal.
2. Ingresá `leer` en el creador. Con una página debe leer `Adios TEC` donde
   antes tenía `Hola TEC`. Con tres páginas debe mostrar, en orden,
   `Hola TEC`, `Adios TEC` y `Pagina 2 original`.
3. Ingresá `salir` en el cliente y esperá su terminación. Luego ingresá
   `salir` en el creador para liberar la reserva.

El ejemplo local breve también se ejecuta con
`./bin/examples/demo_creator config/lab.ini`.

### Demostración multihogar en tres terminales

En Ubuntu o Linux Mint x86-64, compilá con `make examples`. La configuración
`config/lab-multihogar.ini` aporta dos páginas en el nodo 0, dos en el 1 y
una en el 2. Una reserva de cuatro páginas necesita dos hogares.

Para probar en una máquina, ejecutá `DSM_INI=config/lab-multihogar.ini` en
cada terminal. Para usar dos o tres computadoras del laboratorio, ejecutá el
siguiente bloque en cada terminal desde su propia copia del proyecto.
Ingresá las mismas tres IPv4 en el mismo orden. Dos nodos lógicos pueden
compartir una IPv4. Los puertos TCP son 27200, 27201 y 27202.

```bash
make examples
read -rp 'IPv4 del creador (nodo 0): ' DSM_IP0
read -rp 'IPv4 del segundo hogar (nodo 1): ' DSM_IP1
read -rp 'IPv4 del lector (nodo 2): ' DSM_IP2
DSM_INI=$(mktemp)
awk -v a="$DSM_IP0" -v b="$DSM_IP1" -v c="$DSM_IP2" \
  '$1=="nodo" {$5=($3==0 ? a : ($3==1 ? b : c))} {print}' \
  config/lab-multihogar.ini > "$DSM_INI"
```

Ejecutá cada comando en su terminal y en este orden.

```bash
# Terminal del nodo 1 · dejar el hogar disponible
./bin/examples/demo_creator "$DSM_INI" aportar 1

# Terminal del nodo 0 · crear una sola reserva de 16 KiB
./bin/examples/demo_creator "$DSM_INI" compartir 0 4 4096

# Terminal del nodo 2 · abrir el ID anunciado por el creador (1 al inicio)
./bin/examples/demo_reader "$DSM_INI" 2 1 4096
```

El creador muestra el mapa. Con el INI anterior y los hogares libres, los
ordinales 0 y 1 pertenecen al nodo 0 y los ordinales 2 y 3 al nodo 1.
Ingresá `escribir` en el lector y luego `leer` en el creador. Deben aparecer
`Hola TEC`, `Pagina 1 original`, `Adios TEC` y `Pagina 3 original`.
Para cerrar, ingresá `salir` en el lector, esperá que termine, hacé lo
mismo en el creador y finalmente en el hogar 1.

## Pruebas de aceptación

Para el Paso 12A, consultá la guía de la aplicación con los comandos de
tres terminales, las configuraciones para el TEC y las mediciones.
[Multiplicación de matrices con Pthreads y DSM](apps/matrices/README.md)

El Paso 12B incluye una segunda aplicación, con instrucciones para tres
terminales, dos o tres computadoras y casos de uno y dos millones de enteros.
[Merge Sort con Pthreads y DSM](apps/mergesort/README.md)

Desde la raíz del proyecto en Linux x86-64, con los puertos del INI libres:

```bash
set -e
make clean
make
make examples tests aceptacion
timeout -k 2s 90s ./bin/tests/aceptacion_app_matrices
timeout -k 5s 180s ./bin/tests/aceptacion_app_mergesort
for prueba in red memoria directorio paginador paginacion_remota escritura_local \
              api coherencia escritura_remota api_remoto multipagina multipagina_remota api_errores
do
    timeout -k 1s 45s "./bin/tests/aceptacion_$prueba"
done
for repeticion in 1 2 3 4 5
do
    for prueba in api_remoto multipagina_remota api_errores
    do
        timeout -k 1s 45s "./bin/tests/aceptacion_$prueba"
    done
done
timeout -k 1s 60s ./bin/tests/aceptacion_multihogar
timeout -k 1s 60s ./bin/tests/aceptacion_multihogar_errores
for repeticion in 1 2 3 4 5
do
    timeout -k 1s 60s ./bin/tests/aceptacion_multihogar
    timeout -k 1s 60s ./bin/tests/aceptacion_multihogar_errores
done
```

La aceptación de escritura remota incluye dos escritores con hogar en el nodo
3. Las aceptaciones del API usan procesos independientes, el transporte TCP
del DSM y permisos observados en `/proc`. La prueba local inyecta además un
fallo al crear la vista interna y comprueba que el nodo recupera la capacidad.
El informe de la ejecución y las limitaciones se guarda en
[la documentación técnica](docs/README.md).
