> Archivo del Prototipo 2 recuperado del paquete de evidencias 12B. Los comandos y mediciones históricos corresponden a esa revisión. Para el Paso 13B consulte la [guía de laboratorio](../../docs/guia-laboratorio.md).

# Procedencia de la adaptacion

El algoritmo procede de [matrix_multiplication](https://github.com/mperlet/matrix_multiplication), de **Mathias Perlet**, bajo licencia MIT. Se fijo el commit [`fc08492183fde5dcacc422c29d1123b6e126b344`](https://github.com/mperlet/matrix_multiplication/commit/fc08492183fde5dcacc422c29d1123b6e126b344), del 27 de junio de 2022 (`Create LICENSE`). La consulta y descarga se realizaron el 19 de noviembre de 2026 mediante la API de GitHub, usando el SHA completo como referencia.

`LICENSE` conserva exactamente el aviso y los terminos MIT del autor. La licencia cubre tambien las fuentes incluidas en `original/`. Los archivos originales se conservaron byte por byte, sin reformatear ni corregirlos. No se incorporaron binarios, `.git`, OpenMP ni MPI.

| Archivo en origen | Copia local | Git blob SHA-1 verificado |
|---|---|---|
| `src/matrix.c` | `original/src/matrix.c` | `9e2ab3f4053f420067eadb77a67364515eb3639c` |
| `src/matrix.h` | `original/src/matrix.h` | `9de4e617a77c27618f751bb633e1faa3a21fd51b` |
| `src/sequential.c` | `original/src/sequential.c` | `08d498a9a5c8d48e56474749a8fc59ed1b51750b` |
| `src/thread2.c` | `original/src/thread2.c` | `46598150d23da47af4c276cf755641e1c5e6fa50` |
| `README.md` | `original/README.md` | `c7253e9397680e066cddd4f2410615419d670a8c` |
| `LICENSE` | `LICENSE` | `157d7b11817d371f805da049b743a06cfde56a5e` |

Los valores se contrastaron con `git hash-object ARCHIVO`, que no escribe objetos sin la opcion `-w`. Para recuperar una copia se puede utilizar `https://raw.githubusercontent.com/mperlet/matrix_multiplication/fc08492183fde5dcacc422c29d1123b6e126b344/RUTA`.

## Logica que se conserva

`thread2.c` crea un conjunto acotado de Pthreads, reparte un intervalo estatico de filas, entrega el resto al ultimo hilo y espera todos los hilos mediante `pthread_join`. Cada trabajador calcula los elementos independientes con el producto escalar `sum_k A[i][k] * B[k][j]`, usando `double`. `calculo.c` conserva ese producto, el orden creciente de `k`, el reparto estatico, el ultimo rango con resto y la union de todos los hilos. No crea un hilo por elemento.

La unidad de reparto cambia de filas a paginas de C. Un intervalo de paginas puede cortar una fila; el indice lineal se convierte en `(fila, columna)`. Cada pagina pertenece a un solo Pthread de un solo proceso durante la fase de calculo. El coordinador de la aplicacion asigna rangos disjuntos entre procesos.

El original representa cada matriz como `double **` y asigna cada fila con `calloc`. La adaptacion utiliza tres reservas DSM contiguas: A y C en orden por filas, B transpuesta como `B_t[j * interior + k]`. La transposicion conserva el valor de B[k,j] y permite recorrer ambos operandos del producto escalar de forma contigua. No se almacenan punteros virtuales dentro de las reservas. Cada trabajador obtiene sus direcciones locales con `dsm_traducir`, se registra antes de leer Ar y se desregistra al terminar o al detectar un error controlado.

El acumulador empieza en cero para cada elemento, equivalente a la acumulacion sobre C inicialmente cero del original. Una pagina auxiliar por hilo contiene los resultados y se publica por `dsm_escribir`; A y B se leen directamente desde Ar y permanecen estables durante el calculo. Las matrices principales no se reemplazan por copias privadas ni por un canal de transferencia adicional. C comienza con `NAN`, de modo que una pagina no calculada no se confunde con un resultado cero valido.

Tambien se agregan comprobaciones de dimensiones, multiplicaciones de tamanos, precision entera, errores de asignacion y creacion de hilos, y liberacion de los argumentos auxiliares. Estos controles no estaban completos en el programa original.

## Generacion y verificacion

El modo determinista usa semillas A=2026 y B=2027, independientes de la computadora y del numero de hilos. Para `(fila,columna)` se combina la semilla mediante XOR con `fila * 0x9e3779b97f4a7c15` y `columna * 0xbf58476d1ce4e5b9`, con aritmetica sin signo de 64 bits. Se aplica el finalizador SplitMix64 y se obtiene `(hash % 17) - 8`. `matrices_dato_a` y `matrices_dato_b` exponen exactamente esa generacion para reproducir las entradas de la referencia original.

Se conserva `double`, pero los datos deterministas son enteros pequenos y sus productos, sumas y proyecciones se limitan a 2^53 para permitir comparacion exacta. No se compila con `-ffast-math`.

Para dimensiones de hasta 256 en los tres ejes, el verificador lee las matrices DSM, comprueba todas las entradas A/B y calcula una referencia secuencial completa con B en su representacion convencional y el triple bucle original i,j,k. Compara todos los elementos de C e imprime la primera diferencia con coordenadas y valores. Las copias convencionales se usan solo en esta verificacion de referencia.

Para dimensiones mayores se leen y validan todas las entradas A/B y todos los elementos de C. Se rechazan valores ausentes, no finitos, no enteros o fuera del limite matematico. Se comprueban cuatro identidades por fila `A*(B*r) == C*r`: un vector de unos y tres vectores de signos reproducibles. Ademas se recalcula el producto escalar independiente de una muestra por fila, una muestra por pagina de C y los elementos primero, central y ultimo, cuyos valores se imprimen. Una discrepancia de proyeccion provoca la busqueda del primer elemento incorrecto de la fila.

Estas verificaciones grandes combinan proyecciones y productos escalares distribuidos; no equivalen a recalcular los miles de millones de operaciones de una referencia completa. Los vectores tienen semillas publicas y sirven para deteccion de errores de calculo o transporte, sin una garantia criptografica frente a modificaciones adversarias deliberadas.

El modo de archivos TSV y su comparacion secuencial se implementan por separado en `entrada.c`: no usan las formulas deterministas como sustituto de los datos cargados.

## Referencia secuencial original ejecutable

Desde la raiz del proyecto:

```bash
make apps
./bin/apps/matrices_secuencial A.tsv B.tsv > C_original.tsv
```

La referencia original acepta exactamente dos rutas, sin cabecera de dimensiones: cada fila termina con salto de linea y las columnas se separan por tabulaciones. No necesita DSM, Pthreads, MPI ni OpenMP. Para compilarla directamente:

```bash
gcc -O2 -std=c11 -o /tmp/matrices_secuencial \
    apps/matrices/original/src/matrix.c \
    apps/matrices/original/src/sequential.c
```

El parser original presupone archivos bien formados y asignaciones exitosas; se conserva como referencia historica para entradas generadas y controladas. El modo de archivos de la aplicacion DSM realiza validaciones adicionales. La salida original imprime seis decimales, por lo que la comparacion de sus archivos para entradas fraccionarias debe considerar ese redondeo. Con las entradas enteras deterministas los resultados son exactos.

Los tiempos del nucleo Pthreads y de la referencia secuencial interna se miden con `CLOCK_MONOTONIC`; el ejecutable original no se modifica y su tiempo completo debe medirse externamente. Incluye lectura y escritura TSV, por lo que se distingue del tiempo exclusivo de multiplicacion. Los resultados observados se documentan en el README de la aplicacion.
