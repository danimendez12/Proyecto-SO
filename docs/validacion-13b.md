# Paso 13B · Validación de la entrega final

## 1. Revisión de partida

La rama de trabajo es `entrega-final-3`. El árbol estaba limpio al inicio. HEAD local y la referencia publicada consultada con `git ls-remote` coincidían en `f6a9cf210b1338728111bc721eeadd940a3f15da`. La rama ya estaba al día. El árbol de trabajo se probó aparte de una exportación inmutable del commit recibido.

## 2. Cambios incorporados desde el Paso 13A

Entre `5f04d859daf29fcbee4a9642c848176534bc18ef` y HEAD hay un commit, «Limpiado de sistema y feature final». Añadió ambos módulos `demanda`, cambió las interfaces de las aplicaciones, retiró el controlador antiguo de matrices y los dos scripts de red, modificó las dos aceptaciones y la guía. El pool, los avisos iniciales y las consultas ya existían al auditar 13A. Frente a `prototipo-2` (`f6cb7909cd67495110a7239718e149b1c6bbbce0`) hay 43 archivos cambiados en el historial, incluidos los cuatro INI eliminados de las aplicaciones.

La antigua aceptación de matrices comprobaba mapa de C multihogar, intervalos de trabajo, confirmaciones de devolución, un TSV rectangular con resultados conocidos, el ejecutable secuencial original y el fallo de un trabajador. La aceptación publicada conservaba ejecuciones reales de uno y dos procesos, falta de capacidad y dimensiones inválidas, pero solo exigía una marca de verificación por ejecución positiva. En Merge Sort se conservaron el detector del duplicado alterado y las pruebas del controlador histórico. La CLI nueva sí se ejecutaba en cuatro casos (16, 1001, 1001 y 65539 elementos). Se perdieron varios patrones, la entrada distribuida desde archivo y comprobaciones explícitas de reparto. El Paso 13B verifica la ruta `demanda` y los rangos/hogares de la interfaz vigente. El modo distribuido de archivos estaba retirado en el commit recibido.

## 3. Línea base

Debian GNU/Linux 12 x86-64, kernel 6.1.0-39-amd64, GCC 12.2.0, dos CPU virtuales, 1 GiB asignado, páginas de 4096. Se ejecutaron en serie los binarios tras `make clean`, `make`, `make apps examples tests aceptacion`. En la base, las compilaciones tardaron 0.008, 6.347 y 11.777 s respectivamente con GNU Make `-j2` y cero advertencias del compilador. Los tres `test_*` son programas mínimos que retornan cero; las comprobaciones funcionales están en las aceptaciones. La tabla de la sección 10 distingue las dos series.

## 4. Defectos observados

- La aceptación existente del API murió con código 139. Una recompilación de la base con UBSan señaló el destino nulo de `memcpy` en `src/dsm.c:768`. Una operación inválida de tamaño enorme también podía devolver NOMEM antes de validar el argumento.
- UBSan detuvo la base con desplazamiento de 32 sobre `unsigned int` en `src/directory.c:2859`. El creador también usaba máscaras de 32 bits para hogares y cierre cuando la configuración admitía 40 puestos.
- Las copias finales hacia una Ar podían saltarse la adquisición de permisos y el control de vida. La restauración antigua tomaba otra fotografía del rango en una operación separada y volvía a escribir tras el fallo sin comprobar cambios intermedios.
- Los avisos compartían tabla estática, sin identidad de arranque del autor; se reproducían publicaciones antiguas y podían convivir datos y confirmaciones de ejecuciones distintas. Se truncaban claves y había caminos con longitud positiva y puntero nulo.
- Ambos módulos de demanda convertían la diferencia negativa de nanosegundos a un entero sin signo, independientemente de los segundos transcurridos. Cada proceso calculaba por su cuenta el grupo. En un ensayo intermedio, dos procesos con una ventana de 500 ms y un intervalo de reintento mayor se declararon creadores individuales antes del alta TCP mutua.
- Las consultas de páginas mezclaban copias y hogares, omitían silenciosamente puestos ausentes y perdían la versión. El conteo y la segunda copia de la tabla del directorio podían corresponder a tamaños distintos. Las estadísticas incluían algunas concesiones locales o vacías y omitían devoluciones desde un poseedor M.
- La impresión conservaba el bloqueo de FILE durante las RPC y el receptor imprimía PAGE_DATA bajo su mutex. Los límites de respuesta o de cola podían dejar una consulta esperando sin un error correlacionado. PROTO_ERROR solo se imprimía.

## 5. Correcciones

Se mantienen el motor MSI, los GPN de cada configuración y los núcleos matemáticos. Las máscaras DSM pasan a 64 bits. Las copias fijan la vida de todas las vistas y validan el rango antes de reservar auxiliares. Se conserva una restauración condicional de escrituras por trozo. Se toma el respaldo junto con la escritura y solo se repone si aún coincide el M local, su versión y el contenido escrito. Una invalidación o datos posteriores prevalecen; se devuelve el error original sin esperar otra RPC de restauración. El pool distingue el endpoint completo con un socket UNIX abstracto. En las aplicaciones un sondeo con presupuesto común de dos segundos adelanta los reintentos de los enlaces antes de estabilizar candidatos. Los avisos se vinculan al contexto, al autor, a su sesión y a una secuencia. Los participantes acuerdan una propuesta identificada y las fases usan marcas de generación y ordinal en DSM. Los errores de limpieza se propagan. Las consultas diferencian tabla de hogar y copias locales. El directorio prepara una respuesta de tamaño acotado bajo un solo bloqueo. La impresión adquiere el bloqueo del FILE después de las RPC. Un error de protocolo despierta la solicitud pendiente correspondiente.

## 6. Archivos y responsabilidad del cambio

| Archivos | Motivo |
|---|---|
| `src/dsm.c`, `include/dsm.h`, `src/directory.c`, `include/directory.h` | Máscaras, copias, duración del contexto y consultas de nodos/páginas. |
| `src/aviso.c`, `include/aviso.h`, `src/network.c`, `include/network.h`, `src/pool.c` | Sesión de red, entrega y retirada de avisos y exclusión en el pool. |
| `src/protocol.c`, `include/protocol.h`, `src/coherence.c`, `include/coherence.h`, `src/main.c` | Codificación de sesión, versión, correlación, contadores y salida de estado. |
| `apps/demanda_grupo.c/.h`, `apps/matrices/demanda.c`, `apps/mergesort/demanda.c` | Formación común del grupo, tiempos, validación de recursos, barreras y limpieza. |
| `Makefile`, nuevas aceptaciones en `tests/` | Compilar y ejecutar las regresiones con sus dependencias. Los envoltorios de fallo pertenecen al enlace de las pruebas. |
| `src/config.c`, `config/pool.ini`, `config/pool-lab.ini` | Corregir la descripción del reparto y dimensionar la plantilla oficial. |
| README principal, README de cada aplicación, `docs/`, `UPSTREAM.md` y `RESULTADOS.md` | Comandos vigentes, procedencia, resultados históricos separados y evidencia de esta revisión. |

Puntos de inspección en la copia de trabajo probada: [preparación del manifiesto](../src/dsm.c#L242), [copias y validación del API](../src/dsm.c#L780), [restauración condicional](../src/directory.c#L3300), [reclamación por endpoint](../src/pool.c#L79), [recepción de avisos](../src/aviso.c#L317), [acuerdo de participantes](../apps/demanda_grupo.c#L135), [tabla autoritativa STATS](../src/directory.c#L3166), [impresión sin FILE bloqueado durante RPC](../src/dsm.c#L1054), [manejador](../src/signals.c#L41) e [hilo paginador](../src/pager.c#L187).

Los cuatro documentos de procedencia/resultados estaban ausentes del árbol recibido. `git check-ignore` tampoco los identificó como ignorados. Se recuperaron del paquete histórico `12b-definitivo-mwobdgio.tar` con una nota de contexto; sus tiempos antiguos no se contabilizan aquí.

## 7. Pool real

Los puestos se expanden de manera determinista al leer el archivo. El primer proceso en cada IP reclama su primer endpoint elegible. Iniciar otro donante o una aplicación consume otro puesto. Un donante de `dsm_unir` mantiene además su reserva de demostración de una página. La disponibilidad de los servicios se observa por alta y heartbeat; los nodos sin proceso no aportan memoria.

## 8. Límites y reparto

La plantilla de 20 equipos y 40 puestos asigna dos por equipo, normalmente 27200 y 27201 repetidos en cada IP. Con un solo equipo se pueden configurar tres o más puestos. La sintaxis explícita permite cupos desiguales previa configuración. **Propuesta de mínimo impacto para conservar GPN:** antes de arrancar, usar las 40 filas `nodo = ID NOMBRE IP PUERTO BYTES` ya admitidas, manteniendo cada ID y su capacidad. Por ejemplo, de los IDs 38 y 39 de la última computadora, dejar 38 allí y asignar la dirección de la primera y el puerto 27202 al ID 39. La primera computadora tendría tres puestos, la última uno, seguirían figurando las 20 IPv4 y las bases por ID serían idénticas. La plantilla oficial conserva el reparto uniforme hasta que el grupo elija el reparto del laboratorio. Un cambio dinámico de domicilio exigiría preservar la identidad lógica de los puestos y acordar quién los reclama ante particiones. Queda como propuesta. La ampliación a 8 MiB por puesto oficial solo debe aplicarse a una sesión nueva en todos los equipos. Las bases de capacidad de la antigua plantilla de 1 MiB difieren.

## 9. Aplicaciones on demand

La lista aceptada de IDs se traduce en ordinales contiguos de trabajo. Dos invocaciones con los mismos argumentos pueden unirse; para ejecuciones simultáneas independientes se puede fijar `DSM_DEMANDA_GRUPO` con nombres distintos. Las reservas, parámetros, tamaños, token y generaciones se verifican antes de continuar. Un donante no necesita pertenecer al grupo de cálculo. La aplicación no da por terminado el trabajo de otro Pthread ni de otro proceso por el mero transcurso de la ventana. [Mecanismos y verificación numérica](arquitectura-final.md)

## 10. Resultados individuales de la batería

| Programa | Base, salida | Base, s | Entrega, salida | Entrega, s |
|---|---:|---:|---:|---:|
| `clean` | 0 | 0.008 | 0 | 0.006 |
| `make` | 0 | 6.347 | 0 | 16.163 |
| `compilar` | 0 | 11.777 | 0 | 33.760 |
| `test_memory` | 0 | 0.005 | 0 | 0.004 |
| `test_protocol` | 0 | 0.003 | 0 | 0.004 |
| `test_signals` | 0 | 0.003 | 0 | 0.005 |
| `aceptacion_api` | 139 | 2.004 | 0 | 2.116 |
| `aceptacion_api_errores` | 0 | 8.111 | 0 | 8.116 |
| `aceptacion_api_remoto` | 0 | 1.213 | 0 | 0.214 |
| `aceptacion_app_matrices` | 0 | 3.656 | 0 | 9.349 |
| `aceptacion_app_mergesort` | 0 | 4.776 | 0 | 12.780 |
| `aceptacion_avisos` | — | — | 0 | 1.710 |
| `aceptacion_coherencia` | 0 | 1.383 | 0 | 1.383 |
| `aceptacion_consultas` | — | — | 0 | 3.213 |
| `aceptacion_copias` | — | — | 0 | 0.105 |
| `aceptacion_demanda` | — | — | 0 | 45.545 |
| `aceptacion_directorio` | 0 | 0.004 | 0 | 0.019 |
| `aceptacion_escritura_local` | 0 | 0.031 | 0 | 0.048 |
| `aceptacion_escritura_remota` | 0 | 0.615 | 0 | 0.533 |
| `aceptacion_mascaras` | — | — | 0 | 17.132 |
| `aceptacion_memoria` | 0 | 0.004 | 0 | 0.005 |
| `aceptacion_multihogar` | 0 | 0.413 | 0 | 0.414 |
| `aceptacion_multihogar_errores` | 0 | 4.230 | 0 | 4.322 |
| `aceptacion_multipagina` | 0 | 0.409 | 0 | 0.414 |
| `aceptacion_multipagina_remota` | 0 | 1.219 | 0 | 1.118 |
| `aceptacion_paginacion_remota` | 0 | 1.343 | 0 | 1.368 |
| `aceptacion_paginador` | 0 | 0.006 | 0 | 0.005 |
| `aceptacion_pool` | — | — | 0 | 2.321 |
| `aceptacion_red` | 0 | 0.125 | 0 | 0.233 |

Las 17 aceptaciones preexistentes y las seis regresiones terminaron con código 0 en la entrega. GCC no emitió advertencias. “compilar” corresponde al objetivo combinado de aplicaciones, ejemplos, pruebas y aceptaciones.

## 11. Nuevas regresiones y repeticiones

| Prueba | Ronda 1, s | Ronda 2, s | Ronda 3, s | Ronda 4, s | Ronda 5, s |
|---|---:|---:|---:|---:|---:|
| `aceptacion_consultas` | 3.315 | 3.718 | 3.222 | 3.314 | 3.323 |
| `aceptacion_demanda` | 45.606 | 45.548 | 45.588 | 45.435 | 45.523 |
| `aceptacion_pool` | 2.319 | 2.325 | 2.319 | 2.318 | 2.323 |
| `aceptacion_avisos` | 1.715 | 1.812 | 1.710 | 1.814 | 1.915 |
| `aceptacion_multihogar_errores` | 4.330 | 4.324 | 4.238 | 4.328 | 4.229 |
| UBSan `copias` | 0.104 | 0.106 | 0.107 | 0.105 | 0.106 |
| UBSan `mascaras` | 17.008 | 17.104 | 17.132 | 17.102 | 17.108 |

Todas las celdas corresponden a salidas 0. Las dos últimas filas usaron `-fsanitize=undefined -fno-sanitize-recover=undefined`. La regresión de máscaras configuró 40 puestos y levantó cuatro procesos con los IDs 0, 31, 32 y 39. Forzó CAP_CANCEL, confirmó cuatro páginas, cerró copias, reinició 39 y reutilizó las cuatro páginas. La prueba de copias ejercitó NULL, cero bytes, desbordamientos, vistas protegidas, solapamiento en ambas direcciones, registro temporal, ciclo de vida y un fallo posterior con y sin restauración posible.

La prueba de demanda usó dos donantes persistentes, creador 1 e IDs no consecutivos. Probó el reloj al cruzar un segundo, grupos independientes con los mismos argumentos, tres procesos de matrices y Merge Sort, repetición de comando con tokens distintos, llegada tardía, plazo, capacidad insuficiente y la muerte de un trabajador. El caso de SIGKILL exige salida fallida del creador. La ausencia de recuperación de una M perdida limita lo que se puede exigir a ese caso.

La prueba de avisos cubrió claves/cargas inválidas, publicaciones concurrentes, versiones atrasadas, retirada y más de 128 ciclos, tabla llena con enlace STATS sano y cambio de sesión/época. La prueba de pool cubrió reclamaciones simultáneas y puerto ocupado, igual puerto en dos IPv4, `exec`, reutilización, baja por heartbeat con SIGSTOP y retorno del nodo. Los espacios de usuario/red se comprobaron dentro de Linux.

## 12. Matrices

Con un proceso calculador y dos donantes se ejecutaron 7×31×13 y las dimensiones cuadradas 32, 128, 256, 512 y 1000. Tiempos de pared de la primera serie, en segundos, en ese orden: 5.147, 5.129, 5.137, 5.225, 6.14, 9.663. Para 1000, las cinco ejecuciones independientes tardaron **9.663, 13.217, 8.565, 8.174, 9.061 s**, todas con salida 0. A, B y C ocuparon 24 000 000 bytes lógicos, 1954 páginas por matriz y una de control. El verificador habitual comprobó cuatro proyecciones y 2957 muestras de una C de un millón de elementos.

En un ensayo **adicional**, se exportaron A y B con la utilidad del proyecto, se ejecutó el secuencial original MIT sobre los TSV, y una envoltura de enlace de prueba extrajo C con `dsm_leer` después de ejecutar el verificador habitual. El comparador independiente leyó ambos resultados y exigió igualdad exacta en **1 000 000 de 1 000 000** valores. Los datos generados son enteros exactamente representables como dobles. C[0], C[500000] y C[999999] fueron −76, 26 y 572. SHA-256 de C binaria `5dd947ca69c0983390428cfb6ec77bfcba408eec32612c9cddb66c5200ceae4e`. Tiempo de pared del original 2.388 s y del ensayo DSM con verificación y volcado 5.703 s. Estas duraciones incluyen operaciones de entrada/salida diferentes.

## 13. Merge Sort

Se compararon todos los enteros de cada resultado con una referencia construida con `qsort` y una cola de prioridad. Casos de la primera serie, N / patrón / segundos:

| N | Patrón | Tiempo, s |
|---:|---|---:|
| 17 | iguales | 5.124 |
| 257 | ordenado | 5.127 |
| 1001 | inverso | 5.132 |
| 40000 | negativos | 5.133 |
| 100000 | repetidos | 5.177 |
| 1000000 | aleatorio | 6.942 |
| 2000000 | aleatorio | 8.596 |

Un millón: **6.942, 6.073, 6.017, 6.168, 6.086 s**; dos millones: **8.596, 7.495, 7.755, 7.912, 7.303 s**. Todas las salidas fueron 0, con `comparados=N ordenado=si coincide=si` y liberación correcta. Los datos A+B ocuparon 8 000 000 y 16 000 000 bytes lógicos, respectivamente.

## 14. Evidencia de multihogar

En el ensayo grande de loopback, el creador 2 aportó 2 MiB y los donantes 0 y 1, 12 MiB por puesto. Puertos 27212, 27210 y 27211. En matrices 1000, A ocupó 511 páginas en 2 y 1443 en 0; B ocupó 1628 en 0 y 326 en 1; C ocupó 1954 en 1. En la muestra de Merge Sort de dos millones, A también se dividió entre 2 y 0, y B entre 0 y 1. La asignación y los GPN iniciales constan en las trazas `TRAMO`.

Se ejecutó además la plantilla oficial de 8 MiB con las 20 IP sustituidas por direcciones de documentación en dos espacios de red veth. Donantes 0 y 2 escucharon en **27200 en IP distintas**; la aplicación 1 usó 27201 en la primera IP. Matrices 1000 obtuvo salida 0 en 14.027 s y Merge Sort 2 M en 9.141 s. En un ensayo veth separado con 128 KiB por puesto, los procesos calculadores 1 y 3 usaron **27201 en IP distintas**, junto a los donantes 0 y 2. Trabajaron juntos tanto en matrices 128 como en Merge Sort 40000. Permanecieron 18 equipos potenciales sin un solo servicio activo.

## 15. Devolución y reutilización

En la plantilla oficial, los donantes comenzaron y terminaron cada uno con **2047 páginas libres de 2048**. En la prueba grande de loopback recuperaron **3071 de 3072** cada uno. La página restante pertenecía al propio servicio de demostración. En el guion manual con tres donantes de 32 páginas se compararon sus fichas y reservas antes y después, se desconectó/reincorporó el tercero y se ejecutó trabajo nuevo. Las cinco rondas completas del guion, de la apertura a la recogida del primer donante, duraron **21.865, 21.87, 21.862, 21.974, 21.578 s**. La sonda adicional bajo demanda volvió a reservar las 16 páginas propias de cada uno de sus dos donantes después de cada caso ordinario.

## 16. Consultas globales y concurrencia

La aceptación consulta desde dos contextos, crea tres páginas sobre dos hogares, retira otra reserva mientras hay consultas y hace alternar lecturas/escrituras remotas. Distingue réplicas locales de filas canónicas, exige una sola M por máscara y verifica GPN, orden, versión, capacidad y contadores de datos simétricos. Un tercer puesto configurado conserva capacidad nominal, con cero libre. Un timeout simulado sobre un enlace sano produce SOSPECHA; una petición desconocida completa la RPC con error y una consulta posterior funciona en el mismo enlace. Una tabla sintética de réplicas que excede 1 MiB produce un error de tamaño/capacidad; una consulta canónica posterior sigue funcionando. En el guion manual se capturaron fichas y contadores de donantes durante las reservas. El formato almacena la versión en los cuatro bytes finales de cada ficha de página de 40 bytes.

## 17. Demostración en Linux

Cada donante se inicia en su terminal con `./bin/dsm_unir config/pool-lab.ini`. Los comandos interactivos de la demostración son `estado` y `salir`. Desde terminales adicionales se ejecutan `./bin/apps/matrices config/pool-lab.ini 1000 1000 1000` y `./bin/apps/mergesort config/pool-lab.ini 2000000 aleatorio 2026`. Se solicita estado al comienzo, durante reservas y después del cierre. Al retirar y reincorporar un donante se comprueba el cambio de ficha y se lanza trabajo nuevo.

## 18. Dos o tres equipos

En A y B ejecutar el mismo `dsm_unir` con la configuración común; en un tercer equipo se puede ejecutar también en C. Para distribuir un cálculo abrir terminales adicionales en A y B (y opcionalmente C), fijar el mismo nombre de grupo y lanzar el mismo comando de aplicación. La guía incluye posiciones por terminal, capacidad requerida, plazos y cómo validar el resultado. [Guía de laboratorio](guia-laboratorio.md)

## 19. Material para el informe

El documento técnico incluye arquitectura, API, señales, estructuras, límites, diagrama de una copia MSI, las familias de mensajes TCP y la coordinación de ambas aplicaciones. Las carpetas de los originales conservan las licencias MIT. [Arquitectura final](arquitectura-final.md) · [Matrices](../apps/matrices/UPSTREAM.md) · [Merge Sort](../apps/mergesort/UPSTREAM.md)

## 20. Pendiente para el laboratorio

Reemplazar las 20 IPv4 de ejemplo por las direcciones autorizadas, comprobar `sha256sum` del INI compartido, permisos de red, disponibilidad de memoria y repetir el guion en equipos físicos. Las pruebas locales y las de espacios de red virtuales no miden la latencia, las políticas de firewall ni la disponibilidad física del TEC.

## 21. Limitaciones

No hay consenso durante particiones, recuperación redundante ni transacción distribuida de escritura. Los GPN se mantienen dentro de una misma configuración. Los puestos de un equipo apagado no se reasignan. El límite del manifiesto y la fragmentación pueden restringir asignaciones. Los contadores se reinician con el proceso y las consultas son observaciones por hogar en instantes distintos. Los ensayos concurrentes tienen plazo y muestras finitas. Las mediciones sobre CPU virtual compartida sirven para reproducir el resultado funcional, no para estimar aceleración en el laboratorio.

## 22. Estado de entrega

Rama `entrega-final-3`. Se volvió a comprobar la rama publicada y seguía en `f6a9cf210b1338728111bc721eeadd940a3f15da`. Quedan 25 archivos rastreados modificados y 14 nuevos, todos pendientes de revisión y fuera del índice. No se hizo commit, merge ni push. Los binarios se generaron dentro de la VM. Los logs, INI de ensayo, manifiestos SHA-256 y scripts de repetición se entregan en el paquete de evidencias externo al repositorio. El hash del archivo de fuentes compiladas es `71d4cf7dfcf662a5102ca903bf7d990a363294ae7a19bf40a28c6fdf1d418410`.
