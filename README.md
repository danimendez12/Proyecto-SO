# Proyecto-SO

Este proyecto corresponde al Entregable 2 del curso de Sistemas Operativos.
El objetivo es implementar en C para Linux un sistema de Memoria Compartida
Distribuida (DSM) que permita a procesos en distintos nodos acceder a una región
de memoria compartida.

El proyecto está en la etapa de estructura inicial para el Prototipo 1. Los
módulos están pendientes de implementación y los programas de ejemplo y de
prueba sirven para verificar la compilación.

## Organización

| Directorio | Contenido |
| --- | --- |
| `include/` | Cabeceras de los módulos. |
| `src/` | Módulos en C y punto de entrada del nodo (`main.c`). |
| `examples/` | Programas base para los ejemplos de creación y lectura. |
| `tests/` | Programas base para las futuras pruebas de protocolo, memoria y señales. |
| `config/` | Plantilla de configuración del laboratorio. |
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
| `make clean` | Elimina el directorio de salida `bin/`. |
