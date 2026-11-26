# Validación del Paso 14D

Informe del 26 de noviembre de 2026. Rama `entrega-final-3`, con base en `dcfd28fd78f97c80216038efcd85ff292ee4bd48`. Los cambios de 14D se entregan como árbol de trabajo, sin commit ni push automático.

Las rutas de evidencias de este documento son **relativas a la raíz de `evidencias-paso-14d.zip`**, no al repositorio. Por ejemplo, `e-final/resultados.tsv` se encuentra al descomprimir ese ZIP. `e-final/` es la copia exacta de los resultados definitivos de entrega. Los registros intermedios conservan su nombre y no sustituyen esa batería.

## 1. Resumen ejecutivo

La versión recibida aprobó las **23 aceptaciones existentes** antes de modificarla. La entrega de 14D aprobó **30/30 aceptaciones (23 anteriores y siete nuevas)**, además de compilar los tres programas `test_*`. Se completó una matriz de **12 escenarios críticos por cinco repeticiones**, con todas las salidas en cero, y nueve escenarios instrumentados con UBSan sin diagnósticos. Las dos filas que cambiaron en la última revisión —consola e inventario— se repitieron cinco veces con sus versiones definitivas.

Se implementaron 60 puestos configurables, tres por IP en la plantilla oficial, grupos compactos DGR3, selección de hogares conectados, una consola persistente que usa el API, tareas reales mediante Pthreads, consultas resumidas y paginadas, procedencia pública/histórica y propietario lógico estable. Se conservaron el directorio multihogar, la paginación, las aplicaciones independientes y las garantías de aislamiento de páginas perdidas.

Las pruebas de esta etapa se ejecutaron en **Debian 12, kernel 6.1.0-39-amd64, GCC 12.2.0**, en una VM con dos CPU virtuales y aproximadamente 960 MiB visibles. Se usaron loopback y espacios de red Linux con bridge/veth y direcciones de documentación `192.0.2.0/24`. **No constituyen pruebas físicas en el TEC ni una nueva validación en Ubuntu. Tampoco se levantaron 60 nodos activos simultáneamente.** Se probaron 60 puestos configurados, una propuesta serializada de 60 miembros y grupos reales pequeños, incluidos IDs 40, 58 y 59.

## 2. Archivos modificados y nuevos

| Área | Archivos principales | Resultado |
|---|---|---|
| Capacidad y configuración | `include/config.h`, `include/protocol.h`, `config/pool-lab.ini` | Límite 60, máscaras de 64 bits conservadas y tres endpoints por equipo. Las configuraciones históricas siguen disponibles. |
| Acuerdo de aplicaciones | `apps/demanda_grupo.c`, nuevo `apps/demanda_grupo_privado.h` | IDs compactos en DGR3, conversiones y validación de participantes, digest, sesiones, nonce y token conservados. |
| Asignación y API | `src/dsm.c`, `include/dsm.h` | Prioridad local/conectados, sondeo común, consultas por nodo/rango y estado real de la vinculación local. |
| Directorio y transporte de consultas | `src/directory.c`, `include/directory.h`, `src/coherence.c`, `include/coherence.h`, `src/protocol.c` | Procedencia, inventario fiable, cursores, recepción de STATS_REPLY extendido y cierre seguro de una copia M perdida. |
| Consola y lanzamiento | `tools/dsm_unir.c`, junto con los nuevos `tools/dsm_consola.c`, `consola_arranque.h`, `consola_tareas.c/.h`, `consola_consultas.c/.h` | Alta automática, múltiples reservas, errores recuperables, tareas y consultas sin duplicar el motor DSM. |
| Integración de compilación | `Makefile` | Consola, módulos y nuevas aceptaciones. Conserva GCC, C11, Pthreads y las opciones de advertencias. |
| Pruebas | Siete aceptaciones nuevas y ajuste de sincronización en `tests/aceptacion_demanda.c` | Cobertura de 60 puestos, grupos, disponibilidad, consola, tareas, inventario y cierre perdido. |
| Documentación | `README.md`, `docs/README.md`, arquitectura, guía y tres documentos de 14D | Contratos actuales, índice, evidencia, manual y guion físico pendiente. 13B se conserva como histórico. |

Los siete casos nuevos son `aceptacion_60puestos`, `aceptacion_grupo_compacto`, `aceptacion_asignacion_disponibles`, `aceptacion_consola`, `aceptacion_tareas`, `aceptacion_inventario` y `aceptacion_cierre_perdido`. El caso de consola se implementa en Python y ejecuta los binarios C reales. No se eliminaron aceptaciones anteriores ni se reemplazaron los algoritmos de matrices y Merge Sort.

## 3. Decisiones técnicas y defectos corregidos

Se mantuvo un contexto y un proceso lógico por proceso Linux. La consola administra ese contexto, aporta memoria y ejecuta Pthreads sobre él. Las aplicaciones continúan reclamando sus propios puestos. No se implementaron reutilización del contexto de consola por matrices/Merge Sort ni daemon IPC.

La asignación intenta primero la capacidad local y los hogares ya conectados. Si falta memoria, hace una ronda de descubrimiento con presupuesto común y prueba los nuevos participantes. Conserva PREPARE/COMMIT/CANCEL/RELEASE, la continuidad de ordinales y la limpieza de preparaciones parciales. No incorpora un coordinador permanente.

1. **STATS_REPLY de rango.** La nueva respuesta utiliza `RESPUESTA|RANGO`. El dispatcher solo reconocía `RESPUESTA`. La consulta remota podía cerrar el enlace y devolver `DSM_ERR_NODO`. Se aceptó esa combinación exacta únicamente para STATS_REPLY. El fallo se conserva en `consola-i2.log` y `terminal-i2/terminal-2.log`. Consola e inventario definitivos prueban la ruta corregida.
2. **Precondición del hogar 0 en demanda.** El escenario histórico exige utilizar expresamente al donante 0. Que el donante escuchara no acreditaba que ya estuviera enlazado con cada aplicación nueva. Priorizar hogares conectados hizo visible esa carrera del fixture. Se añadió una barrera de sesiones reales y sondeo en el controlador de prueba, manteniendo el requisito del hogar 0, las comprobaciones numéricas, la reutilización de capacidad y los escenarios de llegada tardía/fallo. No se forzó el orden del asignador de producción. Véanse `e-candidato/aceptacion_demanda.log` y `e-demanda-fixture/`.
3. **Cierre de una M perdida.** Una concesión M realmente instalada podía quedar aislada tras una devolución ambigua. Al reconectar, COPY_CLOSE devolvía ENOTCONN indefinidamente y el pin impedía liberar. El hogar ahora exige una revocación confirmada por INVALIDATE_ACK antes de olvidar la copia. La página permanece perdida y sin permisos. No se reconstruyen datos inciertos. Un ACK descartado conserva el pin y el primer cierre falla. El reintento confirmado permite liberar y reutilizar el GPN con otra identidad. La reproducción anterior y la corregida están en `red/cierre/determinista-antes.log` y `red/cierre/determinista-despues.log`.

La batería de la base pasó completa. El último defecto se acreditó mediante un intercalado adicional con M, SIGSTOP y reconexión, que esa ejecución inicial no había expuesto. Los errores de las inyecciones negativas quedaron registrados y las aceptaciones definitivas terminaron en cero después de comprobarlos.

## 4. Cambios de configuración

La plantilla oficial conserva las 20 IPv4 de ejemplo.

| Parámetro | Valor |
|---|---:|
| Puestos máximos | 60, IDs 0–59 |
| Puestos por equipo | 3 |
| Puertos por IP | 27200, 27201, 27202 |
| Rango autorizado | 27200–27299 |
| Memoria por puesto | 8 MiB = 8 388 608 bytes |
| Página | 4096 bytes |
| Páginas por puesto | 2048 |
| Capacidad nominal configurada | 480 MiB = 503 316 480 bytes |
| Hilos configurados en la plantilla | 32 |
| Heartbeat / fallos | 1000 ms / 3 |
| Timeout RPC | 2000 ms |

Los puestos se expanden de forma determinista por el orden de las IP. Tres procesos pueden reclamar los tres endpoints locales. El cuarto recibe un rechazo mientras estén ocupados. Un puesto liberado se reutiliza. No se presta a una IP el cupo asignado a otra IP apagada. Antes de iniciar el clúster deben sustituirse los ejemplos por las direcciones autorizadas. No debe modificarse el INI con reservas vigentes.

`config/lab.ini` y `config/pool.ini` históricos no se convirtieron indiscriminadamente a 60. `aceptacion_60puestos` comprueba esa conservación además de la plantilla nueva.

## 5. Cambios del protocolo y compatibilidad

Las máscaras de nodos siguen siendo `uint64_t` y los IDs internos siguen siendo de 16 bits. Solo la lista serializada de `struct miembro` usa IDs de ocho bits. El formato privado se identifica como **DGR3** y una propuesta de 60 miembros ocupa **104 bytes**, dentro de los 128 bytes de ANUNCIO. Se preservan las comprobaciones estáticas, el digest canónico de IDs de 16 bits, nonce, sesiones, token, orden y confirmaciones. El codec rechaza IDs fuera de rango, duplicados, orden incorrecto y versiones antiguas.

Los manifiestos públicos añaden los cuatro bytes **PUB1** después de sus tramos. Los manifiestos históricos conservan su tamaño anterior y no se identifican como reservas públicas por compartir el formato del ID. La marca acompaña preparación, confirmación y apertura, incluido el cliente remoto. Las comparaciones idempotentes incluyen la procedencia. Con un tramo, REGION_INFO ocupa 76 bytes histórico u 80 público. El manifiesto dentro de CAP ocupa respectivamente 100 o 104 bytes.

Los selectores STATS históricos conservan solicitudes de 40 bytes y sus tablas de asignadas. Los selectores nuevos de inventario y copias usan **48 bytes**, versión del formato, GPN inicial y límite. La respuesta de rango tiene prefijo de 120 bytes, ficha de nodo, filas de página de 40 bytes, cursor y continuación. Los estados libre/preparada/asignada se representan independientemente de MSI. Con 1024 filas y una ficha, la respuesta máxima de este recorrido es **41 184 bytes**. La solicitud cabe en la cola de 64 bytes. La cota general sigue en 1 MiB.

La validación limita cantidades antes de reservar memoria, verifica IDs antes de índices/máscaras, comprueba los GPN contra el hogar configurado y exige cursores crecientes y filas sin duplicados. El inventario autoritativo comprueba continuidad exacta. Estas extensiones requieren desplegar **la misma revisión 14D en todos los nodos**. Reconocer el formato histórico no equivale a garantizar un clúster mixto entre revisiones de aplicaciones.

## 6. Sintaxis definitiva de la consola

El arranque habitual continúa siendo `./bin/dsm_unir CONFIG [CANTIDAD]`. El lanzador usa `dsm_consola` por defecto y acredita cada alta con PID, puesto y puerto. La cantidad admite 1–8 intentos. El último es interactivo y los anteriores exitosos siguen como donantes. `dsm_node` mantiene sus modos históricos para las demostraciones y aceptaciones anteriores.

| Grupo | Comandos |
|---|---|
| Ayuda y cierre | `ayuda`, `salir` |
| Reservas | `reservar TAMAÑO`, `reservas`, `abrir ID`, `mapa ID`, `liberar ID`, `desvincular ID` |
| Datos | `leer ID OFFSET BYTES`, `escribir ID OFFSET TEXTO`, `llenar ID VALOR`, `verificar ID VALOR` |
| Procesamiento | `tarea patron ID HILOS VALOR` |
| Resumen | `estado`, `local`, `nodos`, `memoria` |
| Tablas | `paginas local [DESDE [LIMITE]]`, `paginas global [DESDE [LIMITE]]`, `paginas nodo ID [DESDE [LIMITE]]`, `copias local [DESDE [LIMITE]]` |
| Contadores | `estadisticas`, `estadisticas nodo ID` |

Los números son decimales. Los sufijos K y M expresan KiB y MiB. Así, `64K` son 65 536 bytes y `15M` son 15 728 640. OFFSET cuenta bytes. DESDE es un GPN inclusivo. La lectura interactiva limita cada comando a 256 bytes y la línea completa a 4095. El texto no añade NUL. Las tablas muestran hasta 64 filas por defecto, admiten límites 1–1024 e informan `siguiente`, `mas` y cobertura. No se presenta un truncado como tabla completa.

La tarea de patrón reparte páginas sin solapamiento, registra y desregistra cada trabajador, procesa bytes reales y verifica un patrón dependiente del offset. El padre une los hilos y valida resultados y cantidad procesada. Se probaron páginas finales parciales, rangos vacíos cuando sobran hilos y acceso Ar sano en la aceptación. La tarea interactiva usa copias controladas para comunicar errores recuperables. El hilo del prompt se registra solo cuando la operación lo requiere, lo que permite liberar y seguir trabajando después de terminar una tarea.

El manual completo está en [manual-consola.md](manual-consola.md). Un error recuperable devuelve el prompt. Un ID con retiro pendiente se conserva para reintentar. `CIERRE OK` solo se anuncia después de cerrar el contexto.

Las consultas distinguen capacidad nominal configurada y capacidad confirmada por los hogares que respondieron. Un ausente no aporta memoria libre conocida. `paginas` describe el inventario autoritativo del hogar elegido. `copias local` describe ventanas vinculadas de este contexto, incluida una ventana I. No se equipara el permiso local con el estado global de la página.

Cada nodo acumula ocho contadores durante su sesión.

| Columnas | Significado |
|---|---|
| `lectura`, `escritura` | Intentos de resolver faltas de permiso, incluidos los del API de copia controlada y aquellos cuya RPC falla. |
| `inv_env`, `inv_rec` | INVALIDATE enviado correctamente y solicitudes de invalidación recibidas, incluidas las que encuentran la copia ya I. Una bajada M→S no equivale por sí sola a INVALIDATE. |
| `pag_env`, `pag_rec` | Cargas de página remotas no vacías, incluida la devolución del antiguo M. Excluyen el camino local y concesiones sin datos. |
| `bytes_env`, `bytes_rec` | Bytes de esas cargas, sin cabeceras de protocolo. |

Un acierto de permisos puede completar un acceso sin incrementar esos valores. La suma global separa envío y recepción para evitar contar dos veces los extremos de una transferencia. Los campos se leen individualmente y las respuestas de hogares distintos corresponden a instantes distintos. No hay una instantánea transaccional de la malla ni una atribución exclusiva a la tarea que acaba de ejecutarse.

## 7. Resultado de las 23 aceptaciones anteriores

La fuente de la línea base es `e-base/resultados.tsv`. La definitiva es `e-final/resultados.tsv`. Los tiempos son segundos de pared observados, no estimaciones de rendimiento físico. **Todas las filas de esta tabla terminaron con salida 0 en ambas versiones.**

| Aceptación | Base (s) | Entrega (s) |
|---|---:|---:|
| api | 2.125 | 2.111 |
| api_errores | 8.120 | 8.118 |
| api_remoto | 1.116 | 1.215 |
| app_matrices | 9.293 | 9.371 |
| app_mergesort | 12.707 | 12.710 |
| avisos | 1.611 | 1.613 |
| coherencia | 1.288 | 1.379 |
| consultas | 3.218 | 3.312 |
| copias | 0.105 | 0.105 |
| demanda | 45.634 | 35.329 |
| directorio | 0.005 | 0.004 |
| escritura_local | 0.030 | 0.031 |
| escritura_remota | 0.518 | 0.523 |
| mascaras | 17.004 | 1.005 |
| memoria | 0.007 | 0.003 |
| multihogar | 0.412 | 0.413 |
| multihogar_errores | 4.161 | 4.319 |
| multipagina | 0.407 | 0.413 |
| multipagina_remota | 1.132 | 1.215 |
| paginacion_remota | 1.354 | 1.345 |
| paginador | 0.006 | 0.004 |
| pool | 2.228 | 2.319 |
| red | 0.133 | 0.122 |

| Comando | Base (s) | Entrega (s) | Salida |
|---|---:|---:|---:|
| `make clean` | 0.007 | 0.005 | 0 |
| `make` | 6.317 | 10.554 | 0 |
| `make apps examples tests aceptacion` | 13.861 | 39.976 | 0 |

Los archivos `e-base/advertencias.txt` y `e-final/advertencias.txt` están vacíos. Los programas `test_memory`, `test_protocol` y `test_signals` también terminaron en cero. Tardaron respectivamente 0.003, 0.004 y 0.003 s en la entrega. **Siguen siendo plantillas mínimas con `main` que devuelve cero**. Esas tres ejecuciones acreditan compilación/arranque, no nuevas pruebas funcionales. La cobertura funcional indicada en este informe procede de las aceptaciones.

## 8. Resultado de las siete aceptaciones nuevas

Las siete terminaron con salida 0 en la batería definitiva.

| Aceptación | Entrega (s) | Cobertura principal |
|---|---:|---|
| 60puestos | 0.007 | Plantilla de 20 IPv4/60 puestos, tres reclamos locales, cuarto rechazo, reutilización y bits 40/58/59. |
| grupo_compacto | 2.627 | Propuesta de 60 miembros/104 bytes, límites y versiones y acuerdo real con procesos de IDs 40, 58 y 59. |
| asignacion_disponibles | 6.878 | Hogares disponibles con IDs separados, memoria remota y nueva incorporación sin recorrido secuencial de todos los ausentes. |
| consola | 2.454 | Varias terminales reales, 15 MiB distribuidos, comandos, errores, propietario, MSI, consultas, tareas, liberación y continuación. |
| tareas | 0.409 | Pthreads, patrón independiente, última página parcial, tamaños/hilos y acceso protegido Ar sano. |
| inventario | 0.218 | Procedencia histórica/pública local y remota, propietario estable, PREPARE/CANCEL, libres/asignadas, ventanas I, cursores y serialización. |
| cierre_perdido | 2.229 | M realmente concedida, aislamiento, revocación con ACK, ACK descartado, pin conservado y reutilización con otra reserva. |

El inventario identifica las páginas libres al cruzar el mapa de ocupación con las preparaciones. El caso verifica que una página preparada esté ocupada aunque aún no tenga descriptor confirmado. El donante sin Ar vinculada queda fuera de `copias local`. La ventana abierta puede aparecer I y sin permisos. La capacidad libre de un hogar remoto se muestra desconocida en esa tabla, en lugar de copiar la cifra del observador.

## 9. Cinco repeticiones de los 12 casos críticos

La matriz siguiente contiene **60 ejecuciones seleccionadas, todas con salida 0**. Se toman nueve filas de `e-rondas/resultados.tsv`, demanda de `e-rondas-demanda/resultados.tsv` y las versiones definitivas de consola/inventario de `e-rondas-ultima/resultados.tsv`. Las primeras rondas de esas dos últimas también se conservan, pero no se confunden con los binarios finales.

| Aceptación | R1 (s) | R2 (s) | R3 (s) | R4 (s) | R5 (s) |
|---|---:|---:|---:|---:|---:|
| consultas | 3.221 | 3.318 | 3.213 | 3.719 | 3.318 |
| copias | 0.104 | 0.104 | 0.104 | 0.109 | 0.105 |
| mascaras | 0.999 | 1.007 | 0.994 | 1.014 | 1.002 |
| pool | 2.318 | 2.315 | 2.315 | 2.219 | 2.317 |
| 60puestos | 0.007 | 0.008 | 0.007 | 0.007 | 0.007 |
| grupo_compacto | 2.625 | 2.622 | 2.621 | 2.623 | 2.640 |
| asignacion_disponibles | 6.972 | 6.867 | 6.955 | 6.867 | 6.966 |
| consola, última | 2.571 | 2.867 | 2.564 | 2.363 | 2.562 |
| tareas | 0.409 | 0.408 | 0.410 | 0.409 | 0.408 |
| inventario, último | 0.219 | 0.216 | 0.218 | 0.218 | 0.216 |
| cierre_perdido | 2.231 | 2.230 | 2.229 | 2.231 | 2.230 |
| demanda | 35.289 | 35.304 | 35.244 | 35.412 | 34.457 |

`binarios-comparados.tsv` contrasta SHA256 de los binarios de la primera serie y los de entrega, incluidas sus aplicaciones/dependencias. Son idénticos salvo `dsm_consola` y `aceptacion_inventario`. Por eso se utiliza la serie final de cinco para ambos recorridos. El script de aceptación de consola es idéntico, pero su dependencia C cambió y también se volvió a probar.

Como comprobaciones adicionales, `e-demanda-fixture/repeticiones.tsv` registra cinco pasadas exitosas de la barrera de sesiones, de 34.355–35.309 s. `red/cierre/resultados.tsv` conserva cinco pasadas de cierre perdido, API de errores, copias y errores multihogar. No se suman esas series al denominador 60 de la matriz principal.

## 10. Sanitizador y operaciones sensibles

La compilación instrumentada utilizó estas opciones.

```text
-std=c11 -Wall -Wextra -Wpedantic -pthread -O1 -g
-fsanitize=undefined -fno-sanitize-recover=undefined
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
```

`e-ubsan/resultados.tsv` y `e-ubsan/build.log` documentan nueve resultados con salida 0 y sin diagnósticos de UBSan.

| Caso UBSan | Tiempo (s) |
|---|---:|
| 60puestos | 0.009 |
| grupo_compacto | 2.635 |
| asignacion_disponibles | 6.965 |
| inventario | 0.220 |
| tareas | 0.410 |
| mascaras | 0.997 |
| consultas | 3.819 |
| copias | 0.106 |
| pool | 2.323 |

Se ejercitan las máscaras altas, conversiones de participantes, el rechazo de IDs 60/255 y propuestas de cantidad 61, serialización de 60 fichas, bit 59, solicitante 58, versión `UINT32_MAX`, cursores `uint64_t`, cargas truncadas/corruptas y límites de filas. La regresión de copias conserva NULL/cero/rangos, alias Ar, solapamiento, registro temporal, fallo posterior y restauración condicional. Estas pruebas no prueban por sí solas todos los intercalados ni sustituyen las comprobaciones de límites del código.

Las etapas de `e-consola/` incluyen cinco pruebas adicionales de tareas instrumentadas. `red/cierre/` conserva el ensayo instrumentado de cierre. Se mantienen como evidencia complementaria, sin presentarlos como filas adicionales de la tabla anterior.

## 11. Rendimiento con mayoría de puestos ausentes

Se midió `dsm_reservar` para **15 MiB**, con 60 puestos configurados, solo los nodos **0 y 59** activos y timeout RPC de 2000 ms. Para reproducir el caso anterior fue necesario permitir 60 en los límites de la fuente base, manteniendo su recorrido secuencial sin la optimización. No es una ejecución del binario original limitado a 40 con un ID inexistente.

| Variante | Duración medida de reserva | Resultado |
|---|---:|---|
| Recorrido secuencial anterior | 116.024532133 s | DSM_OK |
| Local + conectados + sondeo acotado | 0.015045038 s | DSM_OK |

Las mediciones exactas están en `red/antes.log` y `red/despues.log`, como `duracion_ns=116024532133` y `15045038`. El mapa en ambos casos mantiene 2048 páginas en hogar 0 y 1792 en hogar 59. Su último GPN es 122623. Los registros confirman datos, devolución de M, liberación y cierre con DSM_OK.

El propio `comparar.sh` incluido en el ZIP se volvió a ejecutar con la fuente final, cuyos 105 archivos coinciden con el árbol congelado. `reproduccion-comparar.log` registra **116.034086327 s antes y 0.012031301 s después**, también con datos, liberación y cierre en cero. Este segundo par acredita la reproducción literal del script de entrega. El primero conserva la medición de la etapa.

La mejora elimina las 58 esperas innecesarias por puestos ausentes en este caso con el donante ya conectado. Son dos pares antes/después, no una estimación estadística de aceleración general. El presupuesto común acota el descubrimiento. Una operación real con donantes conectados que dejen de responder sigue sometida a sus RPC y a las reglas de error, no a una garantía universal de reserva en 15 ms.

## 12. Consumo de recursos con uno, dos y tres procesos

Las muestras se obtuvieron con 60 puestos configurados, en una sola VM. `red/perfil-antes/recursos.json` y `red/perfil-despues/recursos.json` contienen PID, RSS, espacio virtual, hilos y FD. Cada par de cifras es **antes / después**, en KiB cuando corresponde. El ordinal identifica la posición del proceso en cada muestra, no un PID compartido entre ambas ejecuciones.

| Procesos activos | Ordinal | RSS KiB | Virtual KiB | Hilos | FD |
|---:|---:|---:|---:|---:|---:|
| 1 | 1 | 2268 / 4368 | 596024 / 596032 | 66 / 66 | 6 / 6 |
| 2 | 1 | 2268 / 4368 | 596024 / 596032 | 66 / 66 | 6 / 6 |
| 2 | 2 | 4332 / 2296 | 661560 / 661568 | 66 / 66 | 6 / 6 |
| 3 | 1 | 2268 / 4368 | 596024 / 596032 | 66 / 66 | 7 / 7 |
| 3 | 2 | 4332 / 2296 | 661560 / 661568 | 66 / 66 | 7 / 7 |
| 3 | 3 | 4396 / 2232 | 661560 / 661568 | 66 / 66 | 8 / 7 |

Con tres activos se observaron 198 hilos en total en ambas variantes. El RSS agregado de esas muestras fue 10 996 / 8896 KiB. No se interpreta esa variación puntual como una optimización de memoria demostrada. Los FD son una muestra temporal de conexiones y recursos, no una prueba de ausencia absoluta de fugas.

El diseño conserva 59 trabajadores de enlace y 59 buffers de recepción de 1 MiB por proceso con 60 puestos, además de los demás hilos y sus pilas. El tamaño virtual reservado no equivale a RSS ni a memoria DSM ocupada. Los 8 MiB aportados al sistema son capacidad lógica. No se suman las pilas/buffers Linux como si fueran páginas prestables. No se rediseñó la red para reducir ese coste.

## 13. Matrices y Merge Sort independientes

El ensayo definitivo `e-virtual/aplicaciones.tsv` utilizó cuatro espacios de red unidos por bridge/veth, con tres consolas/donantes persistentes de IDs 0, 3 y 6. Los cálculos reclamaron sus propios puestos, sin sustituir esos contextos. Todas las ejecuciones de la tabla terminaron en cero y acreditaron verificación y cierre de cada participante.

| Caso | Parámetros | Procesos calculadores / IDs | Tiempo (s) | Hogares usados |
|---|---|---|---:|---|
| Matrices rectangular | 7×31×13 | 1 / 1 | 5.218 | 1 |
| Matrices grande | 1000×1000×1000 | 1 / 1 | 10.257 | 0, 1, 3 |
| Merge Sort | 1 000 000, aleatorio, 2026 | 1 / 1 | 5.051 | 1 |
| Merge Sort | 2 000 000, aleatorio, 2026 | 1 / 1 | 7.032 | 0, 1 |
| Matrices | 128×128×128 | 2 / 1,4 | 5.020 | 1 |
| Misma invocación de matrices, nueva sesión | 128×128×128 | 2 / 1,4 | 5.018 | 1 |
| Matrices | 128×128×128 | 3 / 1,4,7 | 5.034 | 1 |
| Merge Sort | 40 000, negativos, 2026 | 2 / 1,4 | 5.019 | 1 |
| Merge Sort | 100 001, repetidos, 2026 | 3 / 1,4,7 | 5.025 | 1 |
| Misma invocación de Merge Sort, nueva sesión | 100 001, repetidos, 2026 | 3 / 1,4,7 | 5.020 | 1 |
| Dos grupos independientes | Dos matrices 32×32×32 | 2, creadores separados 1 y 4 | 5.057 | 1 y 4 |

Las repeticiones registran tokens distintos. Los casos pequeños ejercitaron varios calculadores con reservas que cabían en un solo hogar. El caso grande sí obliga a usar memoria remota.

En matrices 1000, A, B y C contienen **24 000 000 bytes lógicos**, además del control. Cada matriz ocupa 1954 páginas físicas. Con una página de control son 5863 páginas y 24 014 848 bytes redondeados. La traza `e-virtual/matrices1000-0.log` registra la siguiente distribución.

| Matriz / ID | Ordinal inicial | Páginas | Hogar | GPN inicial |
|---|---:|---:|---:|---:|
| A / 65538 | 0 | 1954 | 1 | 2049 |
| B / 65539 | 0 | 93 | 1 | 4003 |
| B / 65539 | 93 | 1861 | 0 | 0 |
| C / 65540 | 0 | 187 | 0 | 1861 |
| C / 65540 | 187 | 1767 | 3 | 6144 |

El verificador habitual del caso grande comprobó cuatro proyecciones, 2957 muestras de C y las entradas deterministas completas. Eso **no** equivale a recalcular toda C. Separadamente, el oráculo de `e-oraculo/resultado.json` comparó **exactamente los 1 000 000 valores** de C contra el ejecutable secuencial original de Perlet, con A/B TSV exportadas, una envoltura de ensayo externa y exportación controlada de toda C. Las entradas enteras permiten comparar exactamente pese a que el original imprime seis decimales. No se modificó el algoritmo ni la aplicación para producir ese oráculo.

El oráculo usó 12 MiB por puesto, creador 2, donantes 0 y 1 y hogares efectivos 0 y 2, en loopback aislado. Es una configuración distinta del ensayo virtual oficial de 8 MiB. El original tardó **1.729 s**. La aplicación DSM con verificación y exportación tardó **5.612 s**. Son tiempos de recorridos con I/O, no comparaciones del núcleo de cómputo. Las muestras C[0], C[500000] y C[999999] fueron −76, 26 y 572. El SHA256 de `C-dsm.bin` es `5dd947ca69c0983390428cfb6ec77bfcba408eec32612c9cddb66c5200ceae4e`.

Merge Sort verificó exhaustivamente contra la referencia independiente los 1 000 000 y 2 000 000 enteros, incluyendo orden e igualdad de valores. No se dio por correcto solo por imprimir una marca. Se conservan el detector de duplicados alterados, negativos, repetidos y tamaños impares. El caso de 2 M mostró 12 generaciones de trabajo y comparación de 2 000 000 valores en `e-virtual/sort2m-0.log`.

## 14. Propiedad, orden y coherencia observados

La reserva pública de la consola de ID 0 conservó propietario lógico **1**, aunque otro nodo abriera y modificara sus páginas. El ID de reserva no se interpreta como PID Linux. En el modelo actual, propietario público = `(nodo_creador << 16) | 1`. Solo se aplica cuando la procedencia está acreditada. Un servicio histórico mantiene propietario cero incluso tras un acceso remoto, como exige `aceptacion_inventario`.

El ensayo de 15 MiB de `virtual.log` creó la reserva **2** con dos tramos consecutivos en orden lógico.

| Ordinal inicial | Páginas | Hogar | GPN inicial |
|---:|---:|---:|---:|
| 0 | 2048 | 0 | 0 |
| 2048 | 1792 | 3 | 6144 |

En el GPN 6144, el registro anterior mostraba creador 0, propietario 1, reserva 2, ordinal 2048, M, poseedor 3, versión 0 y máscara 8. Tras la escritura remota y relectura se observaron **el mismo propietario, reserva y ordinal**, S, sin poseedor exclusivo, versión 1 y máscara 9. El último solicitante cambió y el propietario se conservó. La aceptación también compara lecturas exactas después de escrituras y cruces entre páginas/hogares.

Los estados de asignación y MSI se presentan separados. Una página LIBRE no se etiqueta como una copia I. Muestra procedencia NINGUNA y campos inaplicables. Una PREPARADA tiene reserva/ordinal y consume capacidad aunque todavía no exista descriptor confirmado. Una ventana vinculada I puede aparecer en `copias local` con `valida=NO`. El backing de un donante sin ventana vinculada no aparece como copia efectiva. `libre_hogar=?` evita atribuir a un hogar remoto el contador del observador.

## 15. Recuperación de capacidad y continuidad de la sesión

Los observadores 0, 3 y 6 confirmaron en `virtual.log` el mismo resumen de capacidad.

| Momento | Activa (bytes) | Ocupada (bytes) | Libre (bytes) | Páginas ocupadas/libres |
|---|---:|---:|---:|---:|
| Tres consolas vacías | 25 165 824 | 0 | 25 165 824 | 0 / 6144 |
| Reserva de 15 MiB | 25 165 824 | 15 728 640 | 9 437 184 | 3840 / 2304 |
| Tras desvincular y liberar | 25 165 824 | 0 | 25 165 824 | 0 / 6144 |

La cobertura era PARCIAL porque respondían tres de los 60 puestos. El nominal permaneció en 503 316 480 bytes, pero no se sumó memoria disponible de los ausentes. La utilización confirmada pasó de 0 a 62.50 % y volvió a cero.

La consola siguió funcionando tras un comando desconocido, falta de capacidad y un intento ocupado de liberación. El cliente desvinculó, el creador liberó y se reutilizó capacidad en el mismo contexto. También se acreditaron tres puestos locales, rechazo del cuarto y reapertura de un puesto devuelto. La incorporación tardía de ID 9 elevó los confirmados de tres a cuatro y la capacidad de 24 a 32 MiB. Al cerrarlo se volvió a 24 MiB. Una baja adicional mostró dos confirmados y 16 MiB.

Después de **cada** ejecución de aplicaciones, los tres donantes persistentes volvieron a 6144 páginas libres y cero ocupadas. La reparación del cierre perdido permite devolver capacidad tras una revocación acreditada, pero conserva aislada la identidad perdida hasta su liberación. La nueva reserva inicializada vuelve a admitir lecturas.

## 16. Limitaciones y aspectos no comprobados

- La plantilla asigna tres puestos fijos por IP. No hay reclamación global de cupos ajenos ni redistribución de GPN con el clúster activo.
- Se validaron 60 configurados y la representación de un grupo de 60. Los grupos reales probados fueron pequeños. No hay evidencia de 60 procesos activos simultáneos.
- La nueva consola no crea una página de saludo. El ejecutable histórico puede conservar su reserva de demostración. Sus contadores no deben confundirse con los de una consola vacía.
- Hay un proceso lógico por contexto y no hay daemon IPC. Matrices y Merge Sort no se ejecutan dentro del contexto de una consola existente.
- La cota sigue en 256 tramos por manifiesto y hasta 16 solicitados por hogar. La fragmentación puede impedir una reserva aun si la suma libre parece suficiente.
- No hay persistencia, redundancia de una M perdida, consenso durante particiones ni transacción distribuida de escritura. Ar directo exige trabajadores registrados y sincronización de la aplicación. Para recuperar el prompt ante errores se usan las operaciones controladas.
- Los contadores corresponden a actividad observada por sesión, no a operaciones exitosas exclusivas de una tarea. Las lecturas de hogares/campos no forman una instantánea transaccional. Envío y recepción se agregan por separado.
- La red conserva un coste de hilos y espacio virtual considerable con muchos puestos ausentes. Los perfiles son muestras finitas de la VM y no prometen un RSS fijo.
- Los tiempos sobre CPU virtual compartida sirven como evidencia reproducible del recorrido, no para predecir aceleración ni latencia de las computadoras del laboratorio.
- Las pruebas finitas no demuestran ausencia universal de carreras/interbloqueos. No se realizó en esta etapa la prueba física del TEC ni una batería nueva en Ubuntu. La hoja de laboratorio permanece pendiente.

## 17. Cambios documentales

Se actualizaron el resumen del repositorio, el [índice de documentación](README.md), [arquitectura-final.md](arquitectura-final.md) y [guia-laboratorio.md](guia-laboratorio.md). Se añadieron [manual-consola.md](manual-consola.md), este informe y [pruebas-fisicas-14d.md](pruebas-fisicas-14d.md). El índice enlaza el material de 14D y conserva la documentación de 13B como evidencia histórica de aquella revisión.

La arquitectura explica los 60 puestos, DGR3, PUB1, consultas, propiedad, contadores y coste de recursos. El manual define límites y continuación de tablas, campos desconocidos y cierre pendiente. Las casillas del guion físico están pendientes de completar en el laboratorio.

## 18. Ubicación y reproducción de las evidencias

Las siguientes rutas se encuentran en **`evidencias-paso-14d.zip`**.

| Ruta relativa dentro del ZIP | Contenido |
|---|---|
| `base.tar`, `fuente.tar` | Fuente de comparación y fuente final congelada. |
| `SHA256SUMS`, `binarios-comparados.tsv`, `estado-git.txt`, `LEEME.md` | Integridad, correspondencia de binarios, árbol de trabajo y orientación. |
| `e-base/`, `e-final/` | Compilación, entorno, advertencias y resultados individuales base/entrega. |
| `e-rondas/`, `e-rondas-demanda/`, `e-rondas-ultima/` | Series de cinco con las últimas versiones de consola e inventario identificadas. |
| `e-ubsan/` | Compilación instrumentada y nueve resultados. |
| `e-virtual/`, `virtual.log` | Terminales, mapa, capacidad, alta tardía, aplicaciones y grupos sobre veth. |
| `e-oraculo/` | Entradas TSV, C original y DSM completa, wrapper, hashes y resultado exhaustivo. |
| `red/antes.log`, `red/despues.log`, `red/perfil-*/` | Medición del recorrido ausente y recursos de 1/2/3 procesos. |
| `reproduccion-comparar.log` | Segunda medición antes/después, ejecutada mediante el script incluido y la fuente final. |
| `red/cierre/`, `e-demanda-fixture/`, `e-consola/` | Reproducciones enfocadas, fixture de sesiones, tareas y lanzador. |
| `e-candidato/`, `consola-i2.log`, `terminal-i2/` | Fallos intermedios conservados para distinguirlos de la entrega corregida. |
| `documentacion/` | Documentación de la revisión, incluido este informe. |

La raíz conserva `bateria.sh`, `repetir.sh`, `ubsan.sh`, `red-virtual.sh`, `aplicaciones.py`, `oraculo.sh`, `oraculo.py`, `wrap.c` y `comparar.sh`. Antes de reproducir, revisar `LEEME.md`, comprobar los hashes y extraer la fuente en un directorio independiente. No ejecutar los ensayos de red sobre las direcciones físicas del TEC sin autorización. Los scripts virtuales usan espacios de red aislados. `aceptacion_demanda` puede requerir privilegios adicionales según la política del Linux utilizado, sin cambiarla para evitar el requisito.

`fuente.tar` tiene SHA256 **`bd108d1653ceb1a182940f9d0359f0db4a14c8fd55f36303872264f57fc61c39`**. Se compararon sus 105 archivos con el árbol de ejecución, byte por byte. La comparación de binarios acredita qué series corresponden literalmente a la entrega. No se infiere igualdad por el nombre de un directorio. `SHA256SUMS` cubre el contenido del paquete salvo a sí mismo y se completa con la documentación final.

Desde la raíz de la fuente extraída se puede reproducir la compilación.

```bash
make clean
make
make apps examples tests aceptacion
```

Después deben ejecutarse los programas individuales, como hace `bateria.sh`. `make aceptacion` por sí solo no acredita sus salidas. Los scripts registran códigos, tiempos y límites de espera. Los controladores de la ejecución definitiva confirmaron mediante `waitpid` las salidas cero de los procesos lanzados.

## 19. Estado Git y correspondencia de versión

La base y el HEAD de referencia siguen siendo `dcfd28fd78f97c80216038efcd85ff292ee4bd48` en `entrega-final-3`. Los archivos nuevos y modificados quedaron en el árbol de trabajo para revisión. **No se hizo commit, merge ni push**. El estado detallado se conserva en `estado-git.txt` del paquete.

La fuente compilada se identifica mediante `fuente.tar` y sus hashes, no mediante un commit inexistente. La documentación final se añade después de congelar el código. Los resultados de `e-final/` son la copia exacta de la batería de entrega completa. Las primeras series y fallos intermedios quedan separados. La última batería contiene cabecera y 36 registros correspondientes a tres comandos de construcción, tres programas mínimos `test_*` y 30 aceptaciones, todos con salida cero.

## 20. Guion manual y prueba física pendiente

El guion completo por terminal, con tabla de registro aún pendiente, está en [pruebas-fisicas-14d.md](pruebas-fisicas-14d.md). Este resumen es reproducible con las IP autorizadas reales. `ID`, `HOGAR` y `GPN` son los valores impresos por la ejecución, no palabras que deba recibir literalmente el parser.

1. En A, B y C, con el mismo INI y revisión, ejecutar `./bin/dsm_unir config/pool-lab.ini`. Ejecutar `estado`, `nodos` y `memoria`. Para tres consolas nuevas de 8 MiB se esperan 24 MiB confirmados y 6144 páginas libres. Registrar el resultado real.
2. En A ejecutar `reservar 15M`, `reservas`, `mapa ID`, `memoria`. Registrar propietario y tramos. Si el hogar local estaba vacío, se esperan 2048 páginas locales y 1792 remotas, con 2304 libres entre los tres puestos.
3. En A ejecutar `escribir ID 0 Hola TEC`, `leer ID 0 8`, `escribir ID 4092 DosPag01`, `leer ID 4092 8`. En B ejecutar `abrir ID`, leer los ocho bytes, `escribir ID 0 AdiosTEC`. Releer desde A y comparar exactamente.
4. Obtener en `mapa ID` el primer ordinal de un hogar diferente. Si es 2048 y la página es de 4096, B ejecuta `escribir ID 8388604 DosHog02` y A lee ocho bytes desde el mismo offset. Para otro mapa usar `ordinal × tamaño_de_página − 4`.
5. En C ejecutar `paginas nodo HOGAR GPN 1` antes/después. Comprobar propietario y ordinal estables, versión, solicitante, poseedor y máscara. En B usar `copias local 0 8`. Para otra tanda seguir el cursor impreso.
6. En A ejecutar `tarea patron ID 4 37`, comprobar resultados de todos los hilos y el prompt. Desde las otras terminales observar `estado` y `estadisticas` durante la tarea, sin interpretar las consultas como instantáneas simultáneas.
7. Probar `liberar ID` en A mientras B sigue vinculado y comprobar que la consola conserva el ID para reintentar. B ejecuta `desvincular ID`. A reintenta `liberar ID` y verifica capacidad. En el mismo contexto crear `reservar 64K`, llenar/verificar y liberar la nueva reserva.
8. En A iniciar segundo y tercer puesto, probar el cuarto, cerrar uno y reutilizarlo. Registrar las tres identidades/puertos automáticos. Iniciar D desde una cuarta IP ya configurada y comprobar su incorporación sin editar el INI. Cerrarlo y comprobar la baja.
9. Manteniendo donantes disponibles, ejecutar las aplicaciones independientes desde terminales nuevas.

```bash
./bin/apps/matrices config/pool-lab.ini 7 31 13
./bin/apps/matrices config/pool-lab.ini 1000 1000 1000
./bin/apps/mergesort config/pool-lab.ini 1000000 aleatorio 2026
./bin/apps/mergesort config/pool-lab.ini 2000000 aleatorio 2026
```

Para dos o tres calculadores, usar el mismo `DSM_DEMANDA_GRUPO`, argumentos y ventana de agrupación en sus terminales. Para ejecuciones independientes, nombres de grupo distintos. Registrar cada código de salida, participantes, verificación y cierre. Comprobar `memoria` entre casos. Finalizar con `salir` en cada consola y completar retiros pendientes antes de afirmar cierre.

Los resultados de estas instrucciones **en equipos físicos del TEC permanecen pendientes**. La evidencia terminada en este informe corresponde exclusivamente al entorno Debian y a las redes virtuales descritas.
