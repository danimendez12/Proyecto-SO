# Merge Sort con Pthreads y DSM

```bash
make apps aceptacion
./bin/apps/mergesort config/pool-lab.ini 40000 aleatorio 2026
./bin/tests/aceptacion_app_mergesort
./bin/apps/mergesort secuencial 40000 aleatorio 2026
```

La interfaz distribuida recibe cantidad de enteros, patrón y semilla. Admite `aleatorio`, `inverso`, `ordenado`, `negativos`, `repetidos` e `iguales`. El modo secuencial también admite `archivo` y la ruta de entrada. Para repartir la aplicación, iniciá el mismo comando en otras terminales durante la formación del grupo. El programa anuncia los IDs DSM, el creador temporal y el ordinal de cada trabajador.

El creador obtiene control y dos reservas de `int32_t`, prepara la entrada y comunica los tres IDs. Los trabajadores validan los tamaños y confirman cada generación mediante el DSM. Los Pthreads ordenan bloques y después fusionan parejas disjuntas. El ancho se duplica al completar cada nivel y la fuente alterna entre A y B. Una cola impar se copia. En el último nivel queda una sola tarea de fusión. Los datos y las marcas de fase viajan mediante reservas del DSM.

El algoritmo adapta el reparto con Pthreads y la combinación de dos rangos de Brandon Lavello. El ordenamiento interno usa Merge Sort iterativo. El verificador guarda los datos originales en el creador, ordena tramos locales con `qsort` y obtiene la secuencia de referencia mediante una cola de prioridad. Compara **cada entero**, incluido el número de apariciones de cada valor. La salida informa cantidad comparada, orden y coincidencia. La aceptación incluye un resultado ordenado con un duplicado alterado que el verificador debe rechazar.

Con páginas de 4096, A+B+control necesitan 1955 páginas para un millón de enteros y 3909 para dos millones. La devolución de las vistas y de cada aporte precede a `LIBERACION OK`. El antiguo `control.c` permanece para sus pruebas históricas. La ruta de la aplicación actual pasa por `demanda.c`.

[Guía del laboratorio](../../docs/guia-laboratorio.md) · [Procedencia](UPSTREAM.md) · [Licencia MIT](LICENSE) · [Mediciones históricas](RESULTADOS.md)
