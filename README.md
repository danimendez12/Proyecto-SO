# Proyecto-SO

Este proyecto corresponde al Entregable 2 del curso de Sistemas Operativos.
El objetivo es implementar en C para Linux un sistema de Memoria Compartida
Distribuida (DSM) que permita a procesos en distintos nodos acceder a una región
de memoria compartida.

El Prototipo 1 permite iniciar nodos TCP y consultar los metadatos de una reserva
remota de una página. La paginación bajo demanda queda para el paso 5.

## Organización

| Directorio | Contenido |
| --- | --- |
| `include/` | Cabeceras de los módulos. |
| `src/` | Módulos en C y punto de entrada del nodo (`main.c`). |
| `examples/` | Programas base para los ejemplos de creación y lectura. |
| `tests/` | Pruebas de aceptación de red, memoria y directorio y programas base para pruebas unitarias. |
| `config/` | Configuración del laboratorio con dos nodos en loopback. |
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
| `make aceptacion` | Compila los tres binarios de aceptación. |
| `make clean` | Elimina el directorio de salida `bin/`. |

## Prueba local con dos nodos

Compilá con `make` y ejecutá un comando en cada terminal, desde la raíz del
proyecto.

```bash
./bin/dsm_node config/lab.ini 0 servir
./bin/dsm_node config/lab.ini 1 servir 1
```

Cada nodo crea una reserva local de una página al arrancar. El último `1` del
segundo comando indica el ID de la reserva del nodo 0. Después del intercambio
`HELLO`/`WELCOME`, el nodo 1 consulta ese ID y muestra
`nodo 1 reserva_remota 1 gpn 0 hogar 0`. Ambos procesos siguen escuchando. Para
cerrar cada proceso, presioná Ctrl+C en su terminal.

La forma `./bin/dsm_node config/lab.ini 0` muestra la configuración y termina.
Para usar dos equipos, configurá las IPv4 de ambos en una copia del mismo
archivo y ejecutá un nodo por equipo.
