# Laboratorio de la entrega final

## Preparar los equipos

Compilá la misma revisión en cada Linux x86-64. Se requieren GCC, GNU Make, glibc con Pthreads, sockets IPv4, semáforos POSIX y soporte de `memfd_create`, `mmap` y `mprotect`. En Debian o Ubuntu el paquete de compilación es `build-essential`. Comprobá el tamaño de página y construí todos los objetivos desde la raíz.

```bash
getconf PAGESIZE
make clean && make && make apps examples tests aceptacion
```

Prepará **una sola copia idéntica** de `config/pool-lab.ini` para todos los equipos. Las 20 IPv4 incluidas son ejemplos. Sustituilas por las direcciones autorizadas que proporcione el laboratorio antes de iniciar los procesos. Esta plantilla usa 8 MiB por puesto. Sus bases GPN difieren de la plantilla histórica de 1 MiB. Aplicá el cambio antes de iniciar un clúster nuevo. Mantené orden, capacidad y puertos iguales durante toda la sesión. Comprobá con `sha256sum config/pool-lab.ini` en cada computadora. Las pruebas internas usan otros INI.

Con la plantilla, hay 40 puestos potenciales, dos por computadora. La primera IP tiene IDs 0 y 1, la segunda 2 y 3, y así hasta 38 y 39. En cada IP se usan 27200 y 27201. El rango permitido para configurar puestos es 27200–27299. El DSM sondea los endpoints que resultan del INI. Puede iniciarse en cualquier orden. Una computadora con ambas plazas ocupadas necesita liberar una antes de iniciar una aplicación allí. Para una distribución distinta al preparar la sesión, el formato explícito `equipo = IP CANTIDAD PUERTO_INICIAL` ya existe. Usalo en **todas** las filas, con suma de cupos ≤40. Cambiar el orden o los cupos exige que todos los procesos estén detenidos, porque cambian las asociaciones de ID y los GPN.

## Ensayo en dos o tres computadoras

Cada fila corresponde a una terminal. Usá la misma ruta desde la raíz del proyecto. El comando sin cantidad conserva la consola interactiva y facilita cerrar individualmente los donantes. Con el argumento opcional de cantidad, los servicios secundarios quedan en segundo plano y se detienen con SIGINT. El código del proceso interactivo por sí solo no confirma el alta de todos: revisá las fichas de `estado`.

| Momento | Dos equipos | Tres equipos | Acción |
|---|---|---|---|
| 1 | A | A | `./bin/dsm_unir config/pool-lab.ini`. Esperá el mensaje con el ID y el puerto. |
| 2 | B | B | `./bin/dsm_unir config/pool-lab.ini`. Escribí `estado` y esperá que figuren ambos puestos. |
| 3 | Opcional, después del cálculo | C | Incorporá otro donante con el mismo comando. Consultá `estado` desde A. |
| 4 | Nueva terminal A | Nueva terminal A | `./bin/apps/matrices config/pool-lab.ini 1000 1000 1000` |
| 5 | Consola del donante B | Consola del donante B | Mientras existan reservas, escribí `estado`. Compará GPN, hogar, reserva, versión, contadores y páginas libres con otra consulta al terminar. |
| 6 | Nueva terminal B | Nueva terminal B | `./bin/apps/mergesort config/pool-lab.ini 2000000 aleatorio 2026` |
| 7 | Nuevas terminales A y B | Nuevas terminales A, B y opcionalmente C | Ejecutá el mismo comando de cálculo en cada una durante la ventana de agrupación. Compará el token, los IDs del grupo, los ordinales, el resultado y los códigos de salida. |
| 8 | B | C | Cuando las reservas se hayan liberado, escribí `salir` en el donante. Consultá `estado` en A, volvé a ejecutar `dsm_unir` en el equipo retirado y ejecutá un cálculo nuevo. |

Para un ensayo corto se pueden usar `32 32 32` en matrices y `40000 aleatorio 2026` en Merge Sort. Con 8 MiB locales, los casos cortos pueden caber íntegramente en el creador. Los tramos impresos por las aplicaciones permiten comprobar dónde se alojaron realmente. Con páginas de 4096, matrices 1000 requiere 5863 páginas incluyendo control y Merge Sort de dos millones requiere 3909. Cada donante creado con `dsm_unir` ocupa además una página propia de demostración. Para matrices grandes iniciá al menos tres puestos activos contando la aplicación.

Las aplicaciones que coinciden en parámetros y nombre de grupo intentan reunirse. Tras un sondeo inicial de hasta 2000 ms, la ventana de estabilidad predeterminada es de 3000 ms. `DSM_DEMANDA_PLAZO_SEG` limita la coordinación (1800 s por defecto). Para una presentación con arranque manual en varios equipos podés poner, en **cada** terminal de cálculo:

```bash
export DSM_DEMANDA_ESPERA_MS=10000
export DSM_DEMANDA_GRUPO=mesa-A
./bin/apps/mergesort config/pool-lab.ini 40000 aleatorio 2026
echo "$?"
```

Para correr dos trabajos independientes con los mismos parámetros al mismo tiempo, elegí nombres de grupo diferentes. Una llegada posterior al cierre del grupo recibe un diagnóstico si observa esa ejecución. Tras su cierre, el mismo comando puede iniciar una ejecución nueva. Ante una partición de red, grupos sin contacto pueden iniciar ejecuciones distintas. El algoritmo no proporciona consenso del clúster.

## Interpretar la demostración

- `PUESTO` identifica al proceso DSM. `DEMANDA` identifica al creador temporal y enumera los integrantes. Un donante sin aplicación sigue sirviendo capacidad y coherencia.
- `TRAMO` muestra intervalos contiguos de páginas del mismo hogar. `VERIFICACION` se refiere a la comprobación numérica. `LIBERACION OK` exige que la limpieza y el cierre hayan tenido éxito. Comprobá además el código de cada trabajador y las páginas libres de cada donante antes y después.
- `estado` reúne una ficha por puesto que responde y una fila por página en su hogar. Un puesto ausente se marca sin respuesta y aporta cero bytes disponibles. Es normal obtener un resultado parcial con pocos equipos encendidos. La memoria nominal se expresa en bytes y la libre de la ficha en páginas. La reserva de demostración de cada donante persiste hasta que sale.
- La comunicación de una página modificada depende de su hogar y del poseedor. Una salida abrupta durante el trabajo puede impedir completar o liberar esa reserva con seguridad. Para la demostración de reincorporación terminá primero las operaciones pendientes.

## Reproducir las aceptaciones

```bash
make apps examples tests aceptacion
for p in bin/tests/test_* bin/tests/aceptacion_*; do
    timeout -k 3s 180s "$p" || { echo "Falló $p ($?)"; exit 1; }
done
```

Ejecutá la batería en serie porque varias aceptaciones usan los mismos puertos de loopback. Para un único equipo podés emplear `config/pool.ini` para ensayos pequeños. El INI oficial con las IPv4 físicas requiere conectividad TCP directa entre los equipos autorizados.
