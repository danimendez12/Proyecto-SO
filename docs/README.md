# Documentación del DSM

Este directorio reúne los documentos de arquitectura, las modificaciones y las
evidencias de prueba. La siguiente revisión corresponde a la integración
del Prototipo 2 del 17 de noviembre de 2026.

## Estado de partida

La rama `prototipo-2` estaba limpia y coincidía con su referencia local
`origin/prototipo-2` en `87cb2e2`. La base `d47389e` incorporaba el
paginador por señales y la lectura TCP del Prototipo 1. Sobre ella,
`625977b` incorporó el protocolo de coherencia, las campañas del directorio
y los latidos. `afd7297` añadió la clasificación de fallos de escritura y
el paginador MSI local. `ce42a04` incorporó el API local del Paso 11A.
El commit `87cb2e2` reunió ambos desarrollos.

Las ocho aceptaciones preexistentes salieron con código 0 en una copia de
ese commit. La aceptación de coherencia de la División 1 usaba varios
contextos de nodo en un mismo proceso, creaba el manifiesto de prueba y
escribía en Cr. La del Paso 10 utilizaba un proveedor local de exclusividad.
La interacción entre sus dos rutas se comprobó con una nueva aceptación.

## Hallazgos y correcciones

| Gravedad | Hallazgo en el código inicial | Corrección |
| --- | --- | --- |
| Crítica | Una desconexión borraba el bit de un lector y el hogar podía saltar su invalidación y conceder M. | El hogar conserva el bit hasta obtener el ACK. La pérdida del poseedor M deja la página indisponible. |
| Crítica | Algunas rutas abrían Ar antes de copiar la imagen a Cr, copiaban desde M mientras aún era escribible o ignoraban un fallo de protección. | Se congela primero al escritor y se copia la imagen antes de habilitar Ar. La concesión exige que los cambios de protección hayan tenido éxito. |
| Alta | El paginador de la CLI solo resolvía lecturas, mantenía estado separado del directorio y se cerraba tras la lectura de demostración. | La CLI conserva registrado al trabajador y usa el directorio para resolver cada fallo hasta cerrar. |
| Alta | Una respuesta podía entregarse por correlación al pendiente de otro par. El poseedor podía responder directamente al lector antes de que el hogar consolidara la bajada. | Las respuestas al trabajador proceden del hogar, que comprueba la imagen del poseedor. Se verifican par, destino, correlación, identidad, versión, máscara y carga antes de aplicar el permiso. |
| Alta | Un mismo hilo de directorio podía esperar una confirmación de otro hogar mientras tenía una revocación pendiente en su propia cola. | El hogar serializa sus campañas y la réplica dispone de otra cola para atender controles. El lector TCP sigue recibiendo. |
| Media | Con un solo latido de tolerancia se observaron desconexiones de procesos sanos. Los avisos de baja y sospecha carecían de validación suficiente. | La ventana del receptor contempla el margen entre envío y recepción. Se comprueba el nodo al que corresponde cada aviso. |

## Archivos de la integración

| Archivos | Motivo |
| --- | --- |
| `include/coherence.h`, `src/coherence.c` | Extraer el despacho y la espera de RPC de la CLI y conectarlos al paginador. |
| `include/directory.h`, `src/directory.c` | Separar las colas de hogar y réplica, coordinar versiones y permisos, conservar la imagen vigente y exigir las confirmaciones. |
| `include/pager.h`, `src/pager.c` | Reutilizar el hilo, el semáforo y las ranuras del paginador con un directorio que controla los permisos de la vista. |
| `src/network.c` | Validar avisos TCP y corregir la detección de desconexión por latidos. |
| `src/main.c` | Usar el mismo servicio en `servir` e `interactivo`. Permitir ejecutar una lectura o escritura real sobre Ar desde la consola. |
| `tests/aceptacion_escritura_remota.c`, `Makefile` | Ejecutar procesos independientes y verificar sus datos, versiones y permisos en Linux. |
| `README.md`, `docs/README.md` | Registrar los comandos de laboratorio, el alcance, las decisiones y los resultados. |

## Decisiones para el diseño del Entregable 3

- El MSI global del hogar y los permisos de su propia copia pueden diferir.
  La máscara indica si el nodo conserva una copia. Cuando otro nodo tiene M,
  el hogar registra ese poseedor y su propia Ar está en I.
- El hilo paginador entrega la identidad y clase de fallo al servicio de
  coherencia. El lector TCP instala un PAGE_DATA correlacionado bajo el
  mutex del directorio antes de publicar la finalización. El paginador
  reanuda al trabajador. Si se produjo otra revocación, el reintento de la
  instrucción vuelve a pasar por la protección de Linux.
- En una promoción remota desde S, el escritor reserva el identificador de
  la operación y pide al hogar invalidar su copia local. Tras el ACK envía
  PAGE_REQUEST. El hogar revalida las copias, revoca cualquier lectura local
  reabierta y exige ACK a cada otra réplica antes de conceder M.
- Cuando M pertenece a otro nodo, la imagen y la confirmación de la bajada
  llegan al hogar. Este valida la versión y responde al solicitante con la
  imagen vigente. Se serializan las campañas por hogar y las revocaciones
  entrantes avanzan de manera independiente.
- Las confirmaciones de control identifican campaña, GPN, par y operación.
  PAGE_DATA transporta además reserva, versión y permisos. Una respuesta
  tardía carece de autorización para volver a habilitar Ar. Ante pérdida
  ambigua de M se privilegia la coherencia y la página puede quedar
  indisponible hasta reiniciar el ensayo.

## Evidencia de ejecución

Validación en Debian 12 x86-64, kernel 6.1.0-39, GCC 12.2 y GNU Make 4.3.
`make clean`, `make` y `make examples tests aceptacion` terminaron con código
0. GCC compiló con C11, `-Wall -Wextra -Wpedantic -pthread` y sin advertencias.

| Binario en `bin/tests/` | Código final | Qué se comprobó |
| --- | ---: | --- |
| `aceptacion_red` | 0 | Alta TCP y diálogo entre pares. |
| `aceptacion_memoria` | 0 | Vistas del mismo respaldo y protección por página. |
| `aceptacion_directorio` | 0 | Reserva, manifiesto y apertura. |
| `aceptacion_paginador` | 0 | SIGSEGV real, reanudación y aislamiento entre páginas. |
| `aceptacion_paginacion_remota` | 0 | Lectura TCP del Prototipo 1 con dos y tres procesos. |
| `aceptacion_escritura_local` | 0 | Fallo de escritura, exclusividad, rechazo y vencimiento de concesiones locales. |
| `aceptacion_api` | 0 | Ar local, límites, registro, restauración de señales, cierre y reinicio. |
| `aceptacion_coherencia` | 0 | Campañas de la División 1, retraso del ACK, carrera de solicitudes y reincorporación. |
| `aceptacion_escritura_remota` | 0 | Escritura y relectura por Ar entre procesos, comprobación de permisos y dos escritores concurrentes. |

El ejemplo `demo_creator` terminó con código 0. La nueva aceptación pasó
25 repeticiones adicionales. Al finalizar se comprobaron la terminación de
los procesos y la liberación de los puertos.

| Momento de la aceptación de dos procesos | Ar del hogar 0 | Ar del nodo 1 | Evidencia |
| --- | --- | --- | --- |
| Primera lectura | S v1, `r--s` | S v1, `r--s` | El nodo 1 obtiene `Hola TEC`. |
| Escritura confirmada | I local, `---s`, directorio v2 | M v2, `rw-s` | Fallo de escritura, INVALIDATE y ACK. La máscara de copias es 2. |
| Relectura del hogar | S v3, `r--s` | S v3, `r--s` | El antiguo poseedor entrega la imagen. El nodo 0 obtiene `Adios TEC` y vuelve a leerlo sin otro PAGE_REQUEST. |

Cada permiso se leyó de `/proc/<pid>/maps` en la dirección Ar indicada por el
proceso correspondiente. En el escenario de cuatro procesos, los escritores
1 y 2 modificaron posiciones distintas con el hogar en 3. Tras las escrituras
hubo una sola Ar escribible. El lector y el hogar recuperaron ambas marcas.

Una prueba adicional de tres procesos hizo escribir al nodo 1 directamente
desde I. Obtuvo M v1 con carga completa. El hogar releyó `Adios TEC` y pasó
de S v2 a M v3 al escribir. El nodo 2 pasó de I a M v4 y el hogar recuperó
el texto y las marcas en S v5. Una réplica en I puede conservar el número de
su última versión local hasta la siguiente instalación.

Al detener un tercer lector S mediante `SIGSTOP`, el escritor terminó por
`SIGSEGV` al intentar el acceso y el hogar mantuvo su Ar sin escritura. El
lector detenido aún tenía `r--s`. En otro ensayo, el hogar perdió al
poseedor M y su intento de relectura terminó por `SIGSEGV`. Con latidos de
100 ms y un fallo de tolerancia también se verificó la detección de un par
detenido en ambos sentidos.

La demostración manual se repitió con el binario final en **dos espacios de
nombres de red** unidos por un enlace veth, con 192.0.2.1 y 192.0.2.2 en
el INI y los puertos 27200 y 27201. Se observó una conexión TCP establecida,
la secuencia S/S v1, I/M v2, S/S v3 y el cierre con código 0 de cada
proceso. Ambos espacios de nombres pertenecían a la misma máquina virtual.
La ejecución de esta integración entre dos computadoras físicas Linux Mint
queda para el laboratorio.

## Alcance pendiente

El API público conserva el contrato local probado, una reserva de una
página y una vista para sus trabajadores. Para abrir reservas remotas
desde ese API se requiere registrar varias ventanas y coordinar su
desvinculación antes de liberarlas. Las consultas públicas de estado,
la asignación multihogar y las aplicaciones del Paso 12 quedan pendientes.
Los ID, IPv4 y puertos de escucha se asignan mediante el INI. El alta
y el reintento se realizan entre esos pares configurados.

El código comprueba identidad, plazo y versión al recibir las concesiones.
La suite ejercita el rechazo y el vencimiento en el adaptador local. Quedan
sin inyección específica en TCP la duplicación deliberada de una respuesta
antigua y un fallo del sistema en `mprotect` durante la instalación de M.
Los tres programas `test_*` y `demo_reader` siguen siendo plantillas.

Los pasos para compilar y operar los dos nodos en el laboratorio se
encuentran en la [demostración de escritura coherente](../README.md#demostración-de-escritura-coherente).

## Estado de Git para la revisión

La entrega quedó en el árbol de trabajo de `prototipo-2`. HEAD siguió en
`87cb2e2`. Hay nueve archivos versionados modificados y tres nuevos
(`coherence.h`, `coherence.c` y `aceptacion_escritura_remota.c`). El índice
está vacío y `git diff --check` terminó con código 0.
