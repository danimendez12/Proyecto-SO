> Archivo del Prototipo 2 recuperado del paquete de evidencias 12B. Los comandos y mediciones históricos corresponden a esa revisión. Para el Paso 13B consulte la [guía de laboratorio](../../docs/guia-laboratorio.md).

# Mediciones del Paso 12B

Las mediciones de esta sección se ejecutaron en VirtualBox, en un invitado **Debian 12.12 x86-64**, kernel 6.1.0-39-amd64, 2 vCPU, 960 MiB de RAM visibles, páginas de 4096 bytes, GCC 12.2.0 y GNU Make 4.3. La aplicación se compiló con C11, `-pthread -O2 -Wall -Wextra -Wpedantic`. Se usó la rama `prototipo-2` sobre `25ff7df442a27f60bde7c23b2fbbb5b3f4af72e6`. Las pruebas físicas en Linux Mint 22.3 en el TEC quedan pendientes.

## Escala y comparación secuencial

Se usaron procesos independientes conectados por TCP sobre loopback en una pila de red Linux aislada. La ejecución de 16 elementos tuvo un creador y un trabajador con un hilo. Las demás tuvieron tres procesos y dos Pthreads por trabajador cuando el reparto proporcionó suficientes tareas. La semilla fue 2026. Todas las comparaciones fueron exhaustivas y todos los procesos terminaron con código 0.

| N | Patrón | Etapas de fusión | Inicialización DSM (s) | Bloques (s) | Fusión (s) | Verificación (s) | Total DSM (s) | Merge Sort secuencial (s) | qsort (s) |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 16 | Iguales | 0 | 0.000266 | 0.021643 | 0.000000 | 0.000222 | 0.608522 | 0.000001 | 0.000002 |
| 1 000 | Negativos | 0 | 0.000180 | 0.021391 | 0.000000 | 0.000314 | 0.606846 | 0.000116 | 0.000123 |
| 10 000 | Repetidos | 2 | 0.002351 | 0.042063 | 0.043526 | 0.002255 | 0.704337 | 0.000332 | 0.000734 |
| 10 001 | Ordenados | 2 | 0.003651 | 0.041476 | 0.050379 | 0.005487 | 0.714454 | 0.000172 | 0.000301 |
| 100 000 | Inversos | 3 | 0.002903 | 0.082492 | 0.147671 | 0.007214 | 1.417723 | 0.003755 | 0.004734 |
| 1 000 000 | Aleatorios | 6 | 0.346065 | 0.818991 | 0.847625 | 0.609030 | 4.128165 | 0.104451 | 0.199133 |
| 2 000 000 | Aleatorios | 7 | 0.569010 | 2.731371 | 2.764190 | 1.264304 | 9.783515 | 0.224581 | 0.420312 |

Los tiempos son de `CLOCK_MONOTONIC`. El total DSM abarca generar la entrada, esperar trabajadores, reservar, ordenar, comprobar y cerrar. La columna de bloques incluye apertura y coordinación desde el creador hasta la última confirmación. En los trabajadores 1 y 2, abrir A y B tomó 0.004055 y 0.000763 s para un millón, y 0.010139 y 0.003653 s para dos millones. La generación de la entrada privada tomó 0.007276 s y 0.024427 s, respectivamente. Los tiempos de las dos referencias corresponden exclusivamente a sus algoritmos sobre datos locales. Con un millón de elementos, aun contando solo bloques y fusión, el cálculo DSM tomó 1.667 s frente a 0.104 s del Merge Sort secuencial.

```text
VERIFICACION OK elementos=1000000 comparados=1000000 ordenado=si coincide=si segundos=0.609030
VERIFICACION OK elementos=2000000 comparados=2000000 ordenado=si coincide=si segundos=1.264304
```

## Hogares, rangos y capacidad

Los IDs observados fueron 1 para control y 2 y 3 para A y B. El control ocupó 12 288 bytes en los ensayos con dos trabajadores. Los tamaños lógicos de A y B para un millón fueron 4 000 000 bytes **cada uno**. Cada reserva ocupó 977 páginas. Para dos millones fueron 8 000 000 bytes y 1954 páginas por reserva. Los totales de la tabla provienen del recorrido con `dsm_ubicacion_pagina` e incluyen el redondeo y el control.

| Escenario | Hogar 0 (bytes) | Hogar 1 (bytes) | Hogar 2 (bytes) | Total DSM (bytes) |
| --- | ---: | ---: | ---: | ---: |
| 10 000 / 10 001, INI pequeño | 28 672 | 65 536 | 0 | 94 208 |
| 1 000 000, INI grande | 1 048 576 | 6 967 296 | 0 | 8 015 872 |
| 2 000 000, INI grande | 1 048 576 | 14 970 880 | 0 | 16 019 456 |
| 1 000 000, INI ajustado a tres hogares | 1 048 576 | 4 194 304 | 2 772 992 | 8 015 872 |

En el INI grande, A empezó en GPN 3 del hogar 0 y su página lógica 253 pasó al GPN 256 del hogar 1. Para un millón, B empezó en GPN 980 del hogar 1. Los rangos iniciales fueron `[0,507904)` y `[507904,1000000)`, ambos con dos hilos. En dos millones fueron `[0,1015808)` y `[1015808,2000000)`. En la última generación trabajó un hilo del nodo 1 y ambos nodos confirmaron la barrera.

También se ejecutó un millón con capacidades de 1 MiB, 4 MiB y 5 MiB. A cruzó los hogares 0 y 1 y la página lógica 300 de **B** pasó del hogar 1 al GPN 1280 del hogar 2. Se compararon 1 000 000/1 000 000 enteros; total 3.172956 s, seis niveles y códigos 0/0/0. Esta ejecución comprueba el cruce de hogar en ambos lados de la fusión.

## Repetibilidad y errores

Se ejecutaron cinco ensayos consecutivos con **1 000 000** de valores aleatorios, dos hogares, dos trabajadores y la misma semilla. Los tiempos totales fueron 3.822072, 3.919869, 3.819571, 3.741319 y 4.037782 s. Cada ensayo comparó el millón de elementos, confirmó el orden y la coincidencia y terminó con los tres códigos en 0. Una segunda serie de cinco ejecuciones con 10 001 valores repetidos produjo cinco comparaciones completas y cinco cierres de los tres procesos.

Un creador iniciado sin el trabajador requerido y con `MERGESORT_PLAZO_SEG=1` salió con código 1, `DSM=-4` y `LIBERACION OK` después de 1.004863 s. Se rechazaron también un entero fuera de rango, un token no numérico y una entrada truncada. El modo secuencial leyó correctamente un TSV con extremos `INT32_MIN`, `INT32_MAX` y ceros repetidos.

[Comandos para reproducir los ensayos y llevarlos al laboratorio](README.md)

