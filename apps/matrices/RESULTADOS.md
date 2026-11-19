# Informe técnico del Paso 12A

## Alcance y procedencia

Se partió de `prototipo-2` en `beb4d50`, con el árbol limpio. La adaptación usa el código MIT de Mathias Perlet en el commit `fc08492183fde5dcacc422c29d1123b6e126b344`. El directorio `original/src` conserva `matrix.c`, `matrix.h`, `sequential.c` y `thread2.c`. También se conservaron el README y la licencia originales. El manifiesto de procedencia identifica el blob Git de cada archivo. [Origen, licencia y cambios algorítmicos](UPSTREAM.md)

| Archivos en el proyecto | Responsabilidad |
| --- | --- |
| `apps/matrices/main.c` | Roles de creador y trabajador, publicación de IDs, inspección de hogares y GPN, reparto de páginas y cierre. |
| `apps/matrices/calculo.c` y `calculo.h` | Adaptación Pthreads del producto punto en doble precisión, generación reproducible y verificación numérica. |
| `apps/matrices/control.c` y `control.h` | Barreras de alta y finalización sobre una página DSM, propagación de errores y espera de devolución de aportes. |
| `apps/matrices/entrada.c` y `entrada.h` | Lectura de TSV en el creador, referencia independiente desde archivos y exportación de entradas deterministas para el programa original. |
| `apps/matrices/original/`, `LICENSE` y `UPSTREAM.md` | Fuentes de referencia y atribución. |
| `tests/aceptacion_app_matrices.c` y `apps/matrices/prueba_red.sh` | Aceptación por procesos y escenario con dos pilas TCP, dos IPv4 e INI independientes. |
| `config/matrices-lab.ini` y `config/matrices-grande.ini` | Contribuciones para la demostración normal y la de un millón de elementos por matriz. |
| `Makefile`, `README.md` y las dos guías en `apps/matrices/` | Compilación, operación y resultados. |

El creador asigna tres reservas y una página adicional de control. La multiplicación usa A y B desde Ar. B se prepara transpuesta para recorrer consecutivamente los factores de cada producto escalar. Cada Pthread calcula un intervalo exclusivo de páginas y publica por `dsm_escribir`. Los últimos bloques pueden terminar antes del final de la página. El creador exige que los trabajadores hayan concluido los `pthread_join`, devuelto las vistas de matrices y publicado sus marcas de éxito antes de leer C. La guía describe las firmas del API usadas en cada fase y el orden de ejecución. [Diseño y reproducción](README.md)

## Mediciones en Linux

VM Debian 12 x86-64 con dos CPU virtuales, kernel 6.1.0-39-amd64, GCC 12.2.0, GNU Make 4.3 y página de 4096 bytes. Los binarios de la aplicación se compilaron con `-O2`. Para estas mediciones el nodo 1 estuvo en un espacio de nombres de red distinto del creador, enlazado por veth. Los tiempos están en **segundos** y corresponden a una ejecución por tamaño. Para apertura y cálculo se informa el máximo entre procesos trabajadores. Sus relojes miden duraciones locales y esas columnas no se suman para obtener el total.

| N | Elementos por matriz | Bytes DSM de A+B+C | Hilos efectivos por trabajador | Inicializar | Abrir | Calcular | Verificar | Total DSM | Secuencial MIT |
| ---: | ---: | ---: | :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| 4 | 16 | 384 | 1 | 0.000034 | 0.001145 | 0.001223 | 0.000049 | 0.602421 | 0.001904 |
| 32 | 1 024 | 24 576 | 1 + 1 | 0.000039 | 0.000570 | 0.001143 | 0.000102 | 0.604941 | 0.004121 |
| 128 | 16 384 | 393 216 | 2 + 2 | 0.003375 | 0.000574 | 0.023873 | 0.005934 | 0.608243 | 0.017004 |
| 256 | 65 536 | 1 572 864 | 2 + 2 | 0.002359 | 0.000577 | 0.126547 | 0.040792 | 1.305026 | 0.116186 |
| 512 | 262 144 | 6 291 456 | 2 + 2 | 0.012745 | 0.001086 | 0.621270 | 0.023748 | 1.909406 | 0.366808 |
| 1000 | 1 000 000 | 24 000 000 | 2 + 2 | 0.429204 | 0.001723 | 2.462917 | 0.481824 | 5.418316 | 2.232911 |

El total DSM incluye iniciar servicios, esperar los otros procesos, crear reservas, verificar y cerrar. El tiempo de cálculo incluye paginación y escritura en C. El tiempo MIT abarca la ejecución completa del binario original sobre los mismos valores, incluida la lectura de dos TSV y la escritura de C a un archivo. La generación de los TSV se midió aparte. En esta VM el tiempo total original fue menor en los seis tamaños. Las cifras no son una estimación de rendimiento para la red física del TEC.

### Mapa observado para N = 1000

Cada matriz solicitó 8 000 000 bytes lógicos y ocupó 1954 páginas. Las tres exceden en conjunto los 16 MiB del mayor hogar configurado.

| Reserva | ID | Hogar | Ordinales | GPN inicial | Páginas |
| --- | ---: | ---: | --- | ---: | ---: |
| A | 2 | 0 | 0–1953 | 1 | 1954 |
| B transpuesta | 3 | 0 | 0–1953 | 1955 | 1954 |
| C | 4 | 0 | 0–186 | 3909 | 187 |
| C | 4 | 1 | 187–1953 | 4096 | 1767 |

El control ocupó la página local inicial. Los cuatro rangos de Pthreads abarcaron conjuntamente las 1954 páginas de C. Con N = 128 la división observada de C fue de 16 páginas en cada hogar. En 4, 32, 256 y 512 las entradas y la salida cupieron en el nodo 0 con el INI empleado. En esos casos el cálculo siguió siendo remoto.

En las tres trazas completas de N = 1000 se contaron **7327 avisos de fallo SIGSEGV de lectura**, **11 366 registros de envío PAGE_REQUEST** y **11 366 recepciones PAGE_DATA**. Los tamaños de datos declarados en esas respuestas sumaron 46 538 752 bytes. Son observaciones de la aplicación y del protocolo, que también tiene rutas locales. Medir los bytes que circularon por la interfaz TCP requeriría una captura de red independiente.

### Corrección matemática y errores

- Para 4, 32, 128 y 256 se comprobó cada elemento de C frente al cálculo secuencial independiente y se revisaron las entradas deterministas.
- Para 512 y 1000 se comprobaron cuatro proyecciones por fila, se recorrieron todos los elementos de A, B y C y se recalcularon respectivamente 1027 y 2957 productos punto de muestra. En 1000, los valores de C en `[0,0]`, `[500,0]` y `[999,999]` fueron **−76, 26 y 572**. Coincidieron con el ejecutable secuencial original. En 512 los valores correspondientes fueron **442, −204 y 1040**.
- En un ensayo adicional para 128 y 1000 se añadió al verificador del creador, solo en el binario de ensayo, una comparación completa contra el TSV producido por el ejecutable MIT. Después de la verificación habitual se leyó C página por página mediante `dsm_leer`. Coincidieron **16 384 de 16 384** y **1 000 000 de 1 000 000** elementos, respectivamente, y se comprobó el fin de archivo. Ambos creadores y sus trabajadores finalizaron con éxito. Esta comparación adicional quedó fuera de los tiempos de la tabla.
- La aceptación incluyó un producto rectangular 33×37 por 37×41, escrituras remotas en páginas de distintos hogares, una matriz TSV fraccionaria con resultados conocidos, dimensiones incompatibles, desbordamiento, hilos inválidos y falta real de capacidad. La creación parcial se canceló y el proceso comunicó el error.
- En un ensayo complementario con el API público se inyectaron NaN y desviaciones de +1 y −1 antes de verificar. Las tres alteraciones dieron `DSM_ERR_ESTADO` en 4×4 y 512×512. En el caso grande se localizó la discrepancia en `C[213,317]`, cuyo valor correcto era −311. Después de restaurar el valor la verificación pasó. Al liberar las reservas, el ensayo pudo volver a reservar los 8 MiB de capacidad y escribir y releer su último byte.

### Compilación y aceptaciones

Se ejecutaron en orden `make clean`, `make`, `make examples tests aceptacion` y `make apps`. Los cuatro terminaron con código 0. Los tres programas existentes `test_protocol`, `test_memory` y `test_signals` también terminaron con 0.

| Aceptación | Código | Aceptación | Código |
| --- | ---: | --- | ---: |
| `aceptacion_red` | 0 | `aceptacion_memoria` | 0 |
| `aceptacion_directorio` | 0 | `aceptacion_paginador` | 0 |
| `aceptacion_paginacion_remota` | 0 | `aceptacion_escritura_local` | 0 |
| `aceptacion_api` | 0 | `aceptacion_coherencia` | 0 |
| `aceptacion_escritura_remota` | 0 | `aceptacion_api_remoto` | 0 |
| `aceptacion_multipagina` | 0 | `aceptacion_multipagina_remota` | 0 |
| `aceptacion_api_errores` | 0 | `aceptacion_multihogar` | 0 |
| `aceptacion_multihogar_errores` | 0 | `aceptacion_app_matrices` | 0 |

Hubo **cinco repeticiones adicionales de cada una** de `aceptacion_app_matrices`, `aceptacion_api_remoto` y `aceptacion_multihogar`. Las quince repeticiones terminaron con 0. La aceptación de matrices comprueba además que el creador espera al trabajador que falta y que un trabajador que anuncia un error impide la verificación y provoca el cierre con error.

## Operación en el TEC y límites

La ejecución con esta aplicación se probó en Linux con procesos independientes y TCP, incluido el enlace entre espacios de nombres. La ejecución de matrices en dos y tres computadoras físicas Linux Mint 22.3 con GCC 13.3 queda pendiente. Las pruebas físicas anteriores corresponden al DSM, según el equipo. La guía incluye la configuración por terminal y el orden de cierre. [Demostración en dos o tres computadoras](README.md#uso-en-dos-o-tres-computadoras-del-tec)

Los contadores globales de fallos y bytes de red no están expuestos en `dsm.h`. Se conservaron las trazas del protocolo y se midieron los tiempos de las fases. Un fallo definitivo del hogar durante un acceso directo a Ar puede terminar al trabajador. Las barreras dependen de las confirmaciones de todos los participantes y tienen plazo. El modo generado grande usa comprobación algebraica y muestreo reproducibles. El modo TSV compara todas las salidas y necesita en el creador memoria convencional adicional para las entradas de referencia. Antes de entregar corresponde repetir y registrar las mediciones en las computadoras del laboratorio, incluidas sus características de red y CPU.

El trabajo quedó en `prototipo-2` sobre `beb4d50`, con las modificaciones en el árbol de trabajo para revisión. Los cambios se limitan al Makefile, la documentación, la aplicación, sus configuraciones y su aceptación. El núcleo y el API del DSM conservan la revisión de partida.
