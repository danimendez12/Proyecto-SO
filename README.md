# Proyecto-SO

Proyecto de los entregables 2 y 3 del curso IC-6600 Principios de Sistemas Operativos.
El objetivo es implementar en C para Linux un sistema de Memoria Compartida
Distribuida (DSM) que permita a procesos en distintos nodos acceder a una región
de memoria compartida.

El Prototipo 2 permite leer y escribir una página compartida desde procesos Linux
x86-64 independientes. Un fallo sobre la vista de aplicación activa al paginador
y el hogar coordina por TCP los permisos MSI de esa página.

## Organización

| Directorio | Contenido |
| --- | --- |
| `include/` | Cabeceras de los módulos. |
| `src/` | Módulos en C y punto de entrada del nodo (`main.c`). |
| `examples/` | Ejemplo del API local y programa base para el lector. |
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
| `make aceptacion` | Compila los nueve binarios de aceptación. |
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

## API local del Prototipo 2

`make examples` compila el ejemplo de aplicación. Ejecutalo desde la raíz
del proyecto. El argumento del INI es opcional.

```bash
./bin/examples/demo_creator config/lab.ini
```

La aplicación llama a `dsm_iniciar` con el INI y el ID de nodo, y abre su
identidad con `dsm_proc_abrir`. La inicialización prepara una página local
en M e inicia el paginador y sus señales en el mismo proceso. Hay un
contexto activo por proceso. `dsm_reservar` publica esa página, redondea
el tamaño solicitado a una página y devuelve su ID y la dirección de
`Ar`. Las escrituras sobre esa vista se realizan directamente.

Registrá cada trabajador desde su propio hilo con `dsm_hilo_registrar`
antes de acceder a `Ar` y llamá a `dsm_hilo_desregistrar` cuando termine.
El desregistro amplía el API del Entregable 1 para devolver la ranura y
restaurar la máscara del hilo. `dsm_liberar` y `dsm_cerrar` devuelven
`DSM_ERR_OCUPADA` si todavía hay hilos registrados. Un cierre exitoso
invalida el puntero al contexto y los punteros prestados. Inicializá otro
contexto para volver a reservar después del cierre.

`dsm_abrir_reserva` devuelve la misma vista para una reserva propia ya
publicada. `dsm_traducir` devuelve la dirección local de un desplazamiento.
`dsm_leer` y `dsm_escribir` copian a través de `Ar` con validación de
límites. La aplicación sincroniza sus accesos concurrentes al contenido.
La liberación local desmonta la única reserva del contexto. Las consultas
públicas de nodos y páginas y la impresión de estado quedan pendientes.

La apertura remota y `dsm_desvincular_reserva` de una vista remota devuelven
`DSM_ERR_NODO` en esta entrada local. El servicio interno ya expone el intercambio
`REGION_LOOKUP/INFO`. El API público prepara y registra trabajadores sobre
su única vista local antes de publicar la reserva. Permitir abrir y cerrar
también una vista remota requiere gestionar la vida de las demás ventanas,
sus trabajadores y la desvinculación de las copias en el hogar.

## Pruebas de aceptación

```bash
make clean
make
make examples tests aceptacion
timeout -k 1s 25s ./bin/tests/aceptacion_red
timeout -k 1s 10s ./bin/tests/aceptacion_memoria
timeout -k 1s 25s ./bin/tests/aceptacion_directorio
timeout -k 1s 15s ./bin/tests/aceptacion_paginador
timeout -k 1s 30s ./bin/tests/aceptacion_paginacion_remota
timeout -k 1s 20s ./bin/tests/aceptacion_escritura_local
timeout -k 1s 20s ./bin/tests/aceptacion_api
timeout -k 1s 30s ./bin/tests/aceptacion_coherencia
timeout -k 1s 45s ./bin/tests/aceptacion_escritura_remota
```

La aceptación de paginación remota ejecuta dos procesos en loopback y después tres procesos
activos en orden 2, 0, 3. En ese segundo escenario el nodo 2 lee del hogar 3
mientras el nodo 1 permanece apagado. Usa aportes distintos para comprobar
el GPN calculado por suma acumulada.

La aceptación de escritura remota lanza procesos independientes, verifica los
permisos de cada Ar en `/proc/<pid>/maps` y recupera las escrituras mediante
lecturas reales. El segundo escenario enfrenta dos escritores sobre bytes
distintos de la misma página con el hogar en el nodo 3. `demo_reader` y los
programas `test_*` son plantillas compilables.
