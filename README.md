# Proyecto-SO

Proyecto de los entregables 2 y 3 del curso IC-6600 Principios de Sistemas Operativos.
El objetivo es implementar en C para Linux un sistema de Memoria Compartida
Distribuida (DSM) que permita a procesos en distintos nodos acceder a una región
de memoria compartida.

El Prototipo 2 permite compartir reservas de una o varias páginas desde
aplicaciones C independientes en Linux x86-64. Un fallo sobre la vista de aplicación activa al paginador
y el hogar coordina por TCP los permisos MSI de esa página.

## Organización

| Directorio | Contenido |
| --- | --- |
| `include/` | Cabeceras de los módulos. |
| `src/` | Módulos en C y punto de entrada del nodo (`main.c`). |
| `examples/` | Aplicaciones creadora y lectora que usan el API público. |
| `tests/` | Pruebas de aceptación de red, memoria, directorio y paginación. |
| `config/` | Configuración de dos nodos en loopback y plantilla para ocho nodos. |
| `docs/` | Documentación técnica y evidencias del laboratorio. |
| `bin/` | Ejecutables generados durante la compilación. |

## Módulos

| Módulo | Responsabilidad |
| --- | --- |
| `dsm` | Coordinar el ciclo de vida del nodo y sus módulos. |
| `config` | Cargar y validar los parámetros del laboratorio. |
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
| `make` | Genera `bin/dsm_node`. |
| `make examples` | Genera los ejemplos en `bin/examples/`. |
| `make tests` | Genera los programas de prueba en `bin/tests/`. |
| `make aceptacion` | Compila los trece binarios de aceptación. |
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

`config/lab.ini` es el caso local de dos procesos. Para el laboratorio, copiá
`config/lab-ocho.ini`, sustituí los marcadores `<IPv4_A>` a `<IPv4_E>`
por las direcciones reales y descomentá las ocho entradas. Usá el mismo
archivo en todos los equipos y elegí el ID al arrancar. El ejemplo asigna los
nodos 0–2 a A, 3–4 a B, 5 a C, 6 a D y 7 a E, con puertos 27200–27207.
Comprobá que los puertos estén libres y que el firewall permita TCP entre
los equipos. El formato de cada entrada es:

~~~text
nodo = id nombre IPv4 puerto [memoria_bytes]
~~~

Si omitís la contribución, se usa `memoria_inicial`. Las bases lógicas se
calculan sumando los aportes de los IDs anteriores. La capacidad de
configuración es de hasta 16 nodos, limitada en cada archivo por `max_nodos`.
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
| Crear | `dsm_proc_abrir` obtiene el ID de proceso. `dsm_reservar` devuelve un ID global y una Ar contigua. El creador aporta las páginas, inicialmente en M. |
| Abrir | `dsm_abrir_reserva` consulta al creador por TCP, recibe el manifiesto y crea vistas locales. Las páginas remotas comienzan en I. Reabrir el mismo ID devuelve la misma Ar y el tamaño lógico. Basta una desvinculación por nodo. |
| Acceder | Registrá el hilo antes de usar directamente Ar o el puntero de `dsm_traducir`. Cada fallo resuelve el GPN de esa página. La aplicación sincroniza los accesos directos concurrentes a los mismos datos. |
| Copiar | `dsm_leer` y `dsm_escribir` validan el rango lógico y adquieren los permisos MSI por página. Admiten cruces y solapamiento. Un error de comunicación se devuelve al llamador. Una copia de varias páginas puede haberse completado parcialmente si falla una página posterior. |
| Desvincular | El cliente llama a `dsm_desvincular_reserva`. El hogar revoca las copias de ese nodo y recupera el contenido de cada página en M antes de confirmar el cierre. Si se pierde la confirmación, se puede reintentar el cierre con el mismo ID. |
| Liberar | Solo el creador puede llamar a `dsm_liberar`. Una apertura remota vigente devuelve `DSM_ERR_OCUPADA`. Las demás reservas conservan sus vistas. |
| Cerrar | Desregistrá los trabajadores antes de desmontar una reserva o cerrar. `dsm_cerrar` desvincula las aperturas remotas, comprueba las propias, detiene los hilos, restaura las señales y libera recursos. Un cierre fallido puede haber desvinculado otras reservas. Tras un cierre exitoso, el contexto y sus punteros dejan de ser válidos. |

El tamaño lógico es el solicitado, aunque el respaldo use páginas completas.
Los punteros directos deben mantenerse dentro del rango lógico. Linux solo
puede proteger páginas completas, incluido el relleno de la última. Si un
fallo directo sobre Ar resulta irresoluble, el proceso recibe SIGSEGV. Para
obtener un código de error de la operación, usá las funciones de copia.

| Código del API | Situación |
| --- | --- |
| `DSM_ERR_INVAL` | Tamaño cero al reservar, ID inexistente o rango lógico inválido. |
| `DSM_ERR_NOMEM` | Faltan capacidad local, tramos representables o recursos para las vistas. |
| `DSM_ERR_NODO` | El par está desconectado o la página quedó aislada al perder el hogar. |
| `DSM_ERR_TIEMPO` | Una solicitud enviada quedó sin respuesta dentro del plazo. |
| `DSM_ERR_OCUPADA` | Hay trabajadores o aperturas que impiden retirar la reserva. |

Después de perder el hogar, una vista antigua permanece protegida aunque se
restablezca TCP. El ID de nodo carece de una época persistente que identifique
la misma instancia del creador. Una página cuyo último poseedor M se perdió
puede quedar indisponible.

El manifiesto admite hasta 16 tramos. Cada reserva actual cabe íntegramente
en el aporte de su creador. Dos reservas distintas sí pueden tener hogares
distintos. La distribución de una sola reserva entre hogares queda pendiente.

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

## Pruebas de aceptación

Desde la raíz del proyecto en Linux x86-64, con los puertos del INI libres:

```bash
set -e
make clean
make
make examples tests aceptacion
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
```

La aceptación de escritura remota incluye dos escritores con hogar en el nodo
3. Las aceptaciones del API usan procesos independientes, el transporte TCP
del DSM y permisos observados en `/proc`. La prueba local inyecta además un
fallo al crear la vista interna y comprueba que el nodo recupera la capacidad.
El informe de la ejecución y las limitaciones se guarda en
[la documentación técnica](docs/README.md).
