# Multiplicación de matrices con Pthreads y DSM

```bash
make apps aceptacion
./bin/apps/matrices config/pool-lab.ini 32 32 32
./bin/tests/aceptacion_app_matrices
```

Los argumentos son filas de A, dimensión interior y columnas de B. Cada proceso reclama su propio puesto. Las invocaciones con los mismos parámetros pueden acordar una ejecución común. El miembro con menor ID del grupo confirmado crea control, A, B y C. Los trabajadores validan sus IDs y tamaños, abren las reservas y reparten páginas disjuntas de C por ordinal de trabajo. El creador también calcula.

El núcleo conserva el producto escalar de `thread2.c` y el reparto estático entre Pthreads con `pthread_join`. A y C se almacenan por filas y B se almacena transpuesta. Los hilos registrados obtienen entradas y publican resultados mediante el API DSM. La suma recorre la dimensión interior en su orden original. Cada matriz tiene una reserva independiente que puede abarcar varios hogares.

Las entradas generadas utilizan enteros acotados representados en `double`. Con las tres dimensiones ≤256, el verificador comprueba todas las entradas y todos los valores de C con una referencia secuencial. Para tamaños mayores usa cuatro proyecciones deterministas y muestras de productos escalares. La línea `LIBERACION OK` informa el cierre después de verificar. Tres matrices de 1000 × 1000 ocupan 24 000 000 bytes lógicos; con páginas de 4096, se reservan 5863 páginas incluyendo control.

La utilidad de exportación y el ejecutable secuencial original permiten comparar entradas reproducibles. Desde una terminal Linux:

```bash
d=$(mktemp -d)
./bin/apps/matrices exportar A 4 3 "$d/A.tsv"
./bin/apps/matrices exportar B 3 5 "$d/B.tsv"
./bin/apps/matrices_secuencial "$d/A.tsv" "$d/B.tsv"
```

La interfaz distribuida actual recibe dimensiones y genera los datos. Los comandos por roles y los INI antiguos que aparecen en las mediciones históricas corresponden al Prototipo 2.

[Guía del laboratorio](../../docs/guia-laboratorio.md) · [Procedencia y licencia](UPSTREAM.md) · [Mediciones históricas](RESULTADOS.md)
