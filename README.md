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
| `examples/` | Programas base para los ejemplos de creación y lectura. |
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
| `make aceptacion` | Compila los cinco binarios de aceptación. |
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
```

La última prueba ejecuta dos procesos en loopback y después tres procesos
activos en orden 2, 0, 3. En ese segundo escenario el nodo 2 lee del hogar 3
mientras el nodo 1 permanece apagado. Usa aportes distintos para comprobar
el GPN calculado por suma acumulada.
