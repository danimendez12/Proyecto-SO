> Archivo del Prototipo 2 recuperado del paquete de evidencias 12B. Los comandos y mediciones históricos corresponden a esa revisión. Para el Paso 13B consulte la [guía de laboratorio](../../docs/guia-laboratorio.md).

# Procedencia de la adaptación

**Brandon Lavello** publicó la aplicación bajo licencia MIT, con copyright de 2019. La revisión utilizada es el commit `c40b074305776d0c90592fe6335a846ac6959cae` del 21 de enero de 2025. La carpeta `original/` conserva el programa C y el README tal como se publicaron. La copia íntegra de la licencia cubre también esos archivos y debe acompañar las copias distribuidas de la aplicación. [Commit original](https://github.com/brandonlavello/Multi-Threaded-Merge-Sort-in-C/commit/c40b074305776d0c90592fe6335a846ac6959cae) · [Licencia](LICENSE)

| Archivo en esta carpeta | Git blob SHA-1 comprobado en la revisión fijada |
| --- | --- |
| `original/sorting.c` | `0cb357f19eaea8d4f30ab837f3d8a8fab1bc3247` |
| `original/README.md` | `428650b728764c0f6a29a2296a83d1bac22cac22` |
| `LICENSE` | `0dba3afe5fb8e2baf10f6f1e24d81d5a206f27f3` |

Los archivos se obtuvieron de GitHub fijando el SHA completo. Para comprobar los bytes locales desde la raíz usá `git hash-object --no-filters apps/mergesort/original/sorting.c` y repetí con los otros dos archivos. [Fuente fijada](https://github.com/brandonlavello/Multi-Threaded-Merge-Sort-in-C/blob/c40b074305776d0c90592fe6335a846ac6959cae/sorting.c)

## Correspondencia de funciones

El original tiene once enteros. Su `main` crea dos Pthreads para ordenar las mitades, espera ambos y crea un tercer Pthread para fusionarlas. La función denominada `mergeSort` combina dos mitades que ya llegan ordenadas. El ordenamiento interno original lo realiza `bubbleSort`.

| Código de Lavello | Uso en la aplicación distribuida |
| --- | --- |
| `main`, `SortingThreadParameters`, llamadas a `pthread_create` y `pthread_join` | Se conserva la organización por bloques, con Pthreads y espera antes de combinar. En `orden.c`, `struct tarea` y `ejecutar` generalizan las dos mitades a rangos de bloques. El nuevo `main.c` inicia un único nodo y elige el rol de la aplicación. |
| `sort` y `bubbleSort` | El callback `worker` conserva la responsabilidad de ordenar un rango. El nuevo `orden_secuencial` sustituye Bubble Sort por un Merge Sort iterativo O(n log n) que llama a la combinación adaptada. |
| `mergeSort` | Es la reutilización directa del algoritmo. La función homónima en `orden.c` conserva dos índices de entrada, compara con `<`, toma del lado derecho en el empate y copia las colas. Usa `int32_t`, longitudes `size_t` y entradas de solo lectura. |
| `merge`, `MergingThreadParameters` | `mergeSort_dsm` aplica la misma selección y agotamiento a dos intervalos de una reserva. Sus cursores se recargan por página. `orden_fase` distribuye parejas disjuntas en cada nivel. |
| `printArray` | Se conserva en la copia original. La demostración imprime rangos, mapas de hogares y resultados resumidos para poder trabajar con millones de enteros. |

Las funciones literales del autor están en `original/sorting.c`. El ejecutable de revisión compila las versiones adaptadas en `orden.c`. También se corrigió la firma del callback a la que requiere POSIX, `void *(*)(void *)`. La fuente original queda disponible para inspección, incluidos sus callbacks tipados y el Bubble Sort.

## Trabajo propio del Paso 12B

- **Algoritmo y DSM.** El Merge Sort iterativo, los cursores de páginas, los intervalos alineados, las dos reservas alternas y el registro de cada hilo permiten extender la organización original a varios procesos y tamaños arbitrarios dentro de la capacidad del INI.
- **Coordinación.** `main.c` y `control.c` añaden alta, publicación de IDs, confirmaciones por generación, rangos de tareas, métricas, transmisión de errores y devolución de vistas y capacidad a través del API DSM.
- **Entrada y validación.** `datos.c` añade los patrones, el lector de enteros, el comparador independiente con `qsort` y cola de prioridad, y la medición secuencial. Solo el modo secuencial de rendimiento reserva dos arreglos locales completos.
- **Reproducibilidad.** Los INI, el guion de tres procesos, la aceptación, los comandos de laboratorio y las mediciones son material del proyecto del TEC. [Guía de ejecución](README.md) · [Resultados](RESULTADOS.md)
