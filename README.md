# Proyecto-SO

Este proyecto corresponde al Entregable 2 del curso de Sistemas Operativos.
El objetivo es implementar en C para Linux un sistema de Memoria Compartida
Distribuida (DSM) que permita a procesos en distintos nodos acceder a una región
de memoria compartida.

El Prototipo 1 permite iniciar varios procesos DSM y leer una página remota
bajo demanda por TCP.

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

## Módulos previstos

| Módulo | Responsabilidad |
| --- | --- |
| `dsm` | Coordinar el ciclo de vida del nodo y sus módulos. |
| `config` | Cargar y validar los parámetros del laboratorio. |
| `protocol` | Definir el formato y la codificación de los mensajes entre nodos. |
| `network` | Gestionar la comunicación TCP entre nodos. |
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
| `make aceptacion` | Compila los siete binarios de aceptación. |
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

## Alcance del paso 6

La demostración crea reservas unitarias en el hogar del creador. La apertura
consulta al creador y la lectura se enruta por el hogar del GPN del manifiesto.
La transferencia implementada parte de I y requiere una imagen completa.
La primera concesión de lectura degrada M a S e incrementa la versión. Las
escrituras distribuidas, las invalidaciones y la asignación en otros hogares
quedan para pasos posteriores. El alta conserva un latido inicial y una
ficha WELCOME por conexión. La paginación en este paso se ejercita desde la
CLI y la API pública del Entregable 1 queda para su integración posterior.

## Paso 10 del Prototipo 2

En Linux x86-64, el manejador usa el bit 1 de `REG_ERR` de `ucontext_t`
para distinguir un fallo de escritura de uno de lectura. El acceso se
publica en la ranura del trabajador. El paginador aplica los permisos de
`Ar` por página según MSI. I corresponde a `PROT_NONE`, S a `PROT_READ`
y M a `PROT_READ | PROT_WRITE`.

`dsm_pager_iniciar_msi` recibe un adaptador interno para coordinar con la
División 1. Por cada fallo el adaptador recibe ID de reserva, GPN, nodo
solicitante, tipo de acceso, estado y versión locales, y un plazo absoluto
de `CLOCK_MONOTONIC`. Debe resolver el hogar, correlacionar la respuesta y
devolver antes del plazo con la versión autorizada, la imagen vigente si
corresponde y la confirmación de exclusividad. Si la página parte de I se
requiere una imagen completa. Desde S puede omitirse si el hogar confirma
que la copia local conserva los datos vigentes. Si llega una imagen desde S,
debe coincidir con Cr para preservar la vista de los lectores activos.
El paginador exige una
versión nueva y exclusividad confirmada para conceder M. La consulta al
adaptador se realiza sin el mutex del paginador. Ante error o vencimiento,
el trabajador termina por SIGSEGV según la política existente.

La integración de producción requiere acordar el adaptador que completa la
solicitud después de las confirmaciones de invalidación. También debe
coordinar la instalación de la concesión con las revocaciones posteriores,
incluida la actualización de metadatos y permisos locales, y liberar o
revocar una concesión si su instalación falla. Los nombres internos del
paginador no forman parte del protocolo ni del API público.
La CLI continúa usando la entrada de lectura del Prototipo 1. El proveedor
de prueba de escritura vive en la aceptación local.

La aceptación remota de escritura, una vez integrada la División 1, deberá
comprobar S en ambos nodos, invalidación de `Ar` del nodo 0 antes de conceder
M al nodo 1, escritura directa de `Adios TEC`, lectura posterior del nodo 0
con los datos actualizados, versiones coherentes y un único poseedor M.

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
`DSM_ERR_NODO` en esta entrada local. Para integrarlas se necesita extraer
de la CLI el despacho y la correlación de RPC, con un único lector por
conexión, y exponer la apertura `REGION_LOOKUP/INFO` y el cierre
`REGION_CLOSE`. Las escrituras distribuidas requieren el adaptador MSI
real, sus plazos y la coordinación de revocaciones y versiones descrita
en el Paso 10.

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
```

La aceptación de paginación remota ejecuta dos procesos en loopback y después tres procesos
activos en orden 2, 0, 3. En ese segundo escenario el nodo 2 lee del hogar 3
mientras el nodo 1 permanece apagado. Usa aportes distintos para comprobar
el GPN calculado por suma acumulada.
