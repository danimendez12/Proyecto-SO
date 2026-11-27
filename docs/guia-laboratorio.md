# Laboratorio de la entrega final

## Preparar los equipos

Compilá la misma revisión en cada Linux x86-64. Se requieren GCC, GNU Make, glibc con Pthreads, sockets IPv4, semáforos POSIX y soporte de `memfd_create`, `mmap` y `mprotect`. En Debian o Ubuntu el paquete de compilación es `build-essential`. Comprobá el tamaño de página y construí todos los objetivos desde la raíz.

```bash
getconf PAGESIZE
make clean && make && make apps examples tests aceptacion
```

Prepará **una sola copia idéntica** de `config/pool-lab.ini` para todos los equipos. Las 20 IPv4 incluidas son ejemplos. Sustituilas por las direcciones autorizadas que proporcione el laboratorio antes de iniciar los procesos. Esta plantilla usa 8 MiB por puesto. Sus bases GPN difieren de la plantilla histórica de 1 MiB. Aplicá el cambio antes de iniciar un clúster nuevo. Mantené orden, capacidad y puertos iguales durante toda la sesión. Comprobá con `sha256sum config/pool-lab.ini` en cada computadora. Las pruebas internas usan otros INI.

La plantilla tiene 60 puestos, tres por computadora. Para la IPv4 de índice `e`, empezando en cero, los IDs son `3e`, `3e+1` y `3e+2` y los puertos son 27200, 27201 y 27202. El último equipo tiene 57–59. El rango autorizado sigue siendo 27200–27299. Una aplicación independiente requiere un puesto libre en el equipo donde se inicia. Para diagnosticar la ocupación usá `nodos` desde otra consola. La topología se mantiene fija mientras haya un clúster en ejecución.

## Ensayo en dos o tres computadoras

Cada fila corresponde a una terminal. Usá la misma ruta desde la raíz del proyecto. El comando sin cantidad conserva la consola interactiva y facilita cerrar individualmente los donantes. Con el argumento opcional de cantidad, los servicios secundarios quedan en segundo plano y se detienen con SIGINT. Revisá el resultado de alta de cada proceso, su ID, PID y puerto. Para una demostración con una terminal por puesto, usá el comando sin cantidad.

| Momento | Dos equipos | Tres equipos | Acción |
|---|---|---|---|
| 1 | A | A | `./bin/dsm_unir config/pool-lab.ini`. Esperá el mensaje con el ID y el puerto. |
| 2 | B | B | `./bin/dsm_unir config/pool-lab.ini`. Escribí `estado` y esperá que figuren ambos puestos. |
| 3 | Opcional, después del cálculo | C | Incorporá otro donante con el mismo comando. Consultá `estado` desde A. |
| 4 | Nueva terminal A | Nueva terminal A | `./bin/apps/matrices config/pool-lab.ini 1000 1000 1000` |
| 5 | Consola del donante B | Consola del donante B | Mientras existan reservas, usá `memoria`, `paginas global 0 8` y `estadisticas`. Compará luego la capacidad libre y los contadores. |
| 6 | Nueva terminal B | Nueva terminal B | `./bin/apps/mergesort config/pool-lab.ini 2000000 aleatorio 2026` |
| 7 | Nuevas terminales A y B | Nuevas terminales A, B y opcionalmente C | Ejecutá el mismo comando de cálculo en cada una durante la ventana de agrupación. Compará el token, los IDs del grupo, los ordinales, el resultado y los códigos de salida. |
| 8 | B | C | Cuando las reservas se hayan liberado, escribí `salir` en el donante. Consultá `estado` en A, volvé a ejecutar `dsm_unir` en el equipo retirado y ejecutá un cálculo nuevo. |

Para un ensayo corto se pueden usar `32 32 32` en matrices y `40000 aleatorio 2026` en Merge Sort. Con 8 MiB locales, los casos cortos pueden caber íntegramente en el creador. Los tramos impresos por las aplicaciones permiten comprobar dónde se alojaron realmente. Con páginas de 4096, matrices 1000 requiere 5863 páginas incluyendo control y Merge Sort de dos millones requiere 3909. Una consola nueva de `dsm_unir` aporta las 2048 páginas disponibles. El servicio histórico `dsm_node` sí ocupa una página de demostración. Para matrices grandes iniciá al menos tres puestos activos contando la aplicación.

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
- `estado` presenta un resumen. `nodos` muestra los puestos y su respuesta. `paginas global DESDE LIMITE` enumera filas autoritativas y `copias local` las copias del emisor. Solo las fichas confirmadas integran la capacidad activa y la ocupación medida.

- La comunicación de una página modificada depende de su hogar y del poseedor. Una salida abrupta durante el trabajo puede impedir completar o liberar esa reserva con seguridad. Para la demostración de reincorporación terminá primero las operaciones pendientes.

[Manual de comandos](manual-consola.md) · [Guion por terminal en el TEC](pruebas-fisicas-14d.md)

## Reproducir las aceptaciones

```bash
make apps examples tests aceptacion
for p in bin/tests/test_* bin/tests/aceptacion_*; do
    if [ "${p##*/}" = aceptacion_demanda ] && [ "${DSM_SUDO_DEMANDA:-0}" = 1 ]; then
        sudo timeout -k 5s 210s "$p" || exit $?
    else
        timeout -k 5s 210s "$p" || exit $?
    fi
done
```

La aceptación de consola necesita Python 3. Si Ubuntu restringe `unshare -n`, usá `export DSM_SUDO_DEMANDA=1` antes del bucle para ejecutar únicamente `aceptacion_demanda` con los privilegios autorizados por el laboratorio y conservá su código de salida. No es necesario cambiar las políticas del equipo. Ejecutá la batería en serie porque varias aceptaciones usan los mismos puertos de loopback. Para un único equipo podés emplear `config/pool.ini` para ensayos pequeños. El INI oficial con las IPv4 físicas requiere conectividad TCP directa entre los equipos autorizados.
