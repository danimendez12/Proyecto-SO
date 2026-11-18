# Documentación del DSM

Este directorio reúne las decisiones de arquitectura y las evidencias de prueba.

## Paso 11B del Prototipo 2 · 18 de noviembre de 2026

### Auditoría de partida

La rama `prototipo-2` estaba limpia en `49ce66e` y coincidía con la referencia
local `origin/prototipo-2`. Se inspeccionaron los módulos `dsm`, `coherence`,
`directory`, `pager`, `signals`, `memory`, `protocol`, `network` y `config`
con sus cabeceras, la CLI en `main.c`, el Makefile, los dos ejemplos, las
aceptaciones y la documentación del repositorio. También se contrastaron los
mensajes de cierre y asignación con el diseño del Entregable 1. Las nueve
aceptaciones originales se ejecutaron en Linux sobre una copia de esa base.

El API publicado ya devolvía un ID de 32 bits y una dirección local, y
aceptaba tamaños y desplazamientos de tipo `size_t`. Su implementación
preparaba una sola reserva de una página y el paginador registraba una sola
ventana. El directorio tenía tablas de
capacidad fija y algunas rutas de coherencia trabajaban con la base de Cr
de la reserva. La CLI ya usaba un servicio TCP reutilizable. Una sola espera
RPC por par impedía atender bien varias solicitudes del mismo proceso. El
hogar carecía de un cierre de apertura remota confirmado de extremo a extremo.

### Cambios de arquitectura para el Entregable 3

| Archivos | Decisión y efecto |
| --- | --- |
| `include/dsm.h`, `src/dsm.c` | Se conservan las firmas públicas. El contexto inicia TCP y el paginador, registra varias vinculaciones y distingue creación, apertura, desvinculación y liberación. Las copias validan ambos extremos cuando el buffer también pertenece a una vista DSM. |
| `include/directory.h`, `src/directory.c` | El directorio gestiona capacidad por hogar, manifiestos, identidades de página y aperturas por nodo. Coordina las revocaciones antes de confirmar el cierre. |
| `include/pager.h`, `src/pager.c`, `include/signals.h`, `src/signals.c` | El hilo puede registrarse antes de crear o abrir. El manejador localiza la ventana por la dirección del fallo y el ordinal por desplazamiento. La cabeza se publica atómicamente y las ventanas se retiran cuando ya no hay trabajadores. |
| `include/coherence.h`, `src/coherence.c`, `src/main.c` | El API y la CLI usan el mismo servicio. El lector TCP correlaciona por par, identificador y tipo. Cada resolución local de permisos se coordina por GPN entre fallos directos y copias controladas. |
| `include/protocol.h`, `src/protocol.c` | Se añadieron `REGION_CLOSE` y `REGION_CLOSE_ACK` con el ID de reserva y el resultado. Se mantienen los mensajes de página y las campañas MSI existentes. |
| `examples/demo_creator.c`, `examples/demo_reader.c` | Aplicaciones independientes que incluyen el API público, registran sus propios trabajadores y comparten una o tres páginas. |
| `Makefile`, `tests/aceptacion_api.c`, `tests/api_prueba.h`, cuatro nuevas aceptaciones | Se enlazan los dos ejemplos con el servicio. Los hijos de prueba se crean antes de inicializar DSM. Las tuberías coordinan las acciones y recogen resultados, mientras el contenido de la reserva se transfiere por TCP. El fallo de `mmap` se inyecta únicamente al enlazar la prueba local. |
| `README.md`, `docs/README.md` | Contrato de vida, comandos de dos terminales, arquitectura, evidencia y alcance de cada fase. |

**Apertura y MSI.** El creador publica la reserva después de preparar el
respaldo y registrar su ventana. El cliente solicita `REGION_LOOKUP`, valida
`REGION_INFO` y crea su propio par Ar/Cr con Ar inicialmente en I. Al tocar
Ar, el SIGSEGV real registra la página en la ranura del trabajador. El
paginador solicita los datos o la exclusividad al hogar. El directorio aplica
`mprotect` a la página indicada antes de confirmar una revocación. Una
concesión M requiere las confirmaciones de las réplicas afectadas. Una
lectura de una página en M remota recupera la imagen vigente del poseedor.
El mismo lector TCP instala cada respuesta correlacionada. La versión y el
poseedor se mantienen por GPN.

**Direcciones.** El ID de reserva combina el ID del creador en los 16 bits
altos y un contador local de 1 a 65535 en los bajos. El contador avanza
durante la vida del contexto y vuelve a empezar al reiniciar. Para tamaño
lógico `L > 0` y página configurada `P`, se asignan
`1 + (L - 1) / P` páginas por división entera. La base del nodo `i` en
páginas es `B[i] = (M[0] + … + M[i−1]) / P`, con aportes alineados de su
INI. El hogar de un GPN satisface `B[i] <= GPN < B[i+1]`. El manifiesto
cubre los ordinales lógicos mediante tramos. Dentro de uno, el GPN es
`gpn_inicial + ordinal - orden_inicial`. Ar y Cr son contiguas en el
proceso aunque haya huecos entre GPN. Datos y protecciones se aplican a
`base + ordinal * P`. La traducción y las funciones de copia usan `L`
como límite. El puntero directo se rige por la granularidad de Linux.

**Vida y concurrencia.** Una apertura repetida devuelve la misma dirección
y mantiene una vinculación por nodo. Desvincular recupera las páginas
modificadas y revoca las compartidas del cliente antes de desmontar sus
vistas. El propietario conserva las suyas. Un trabajador registrado impide
desmontar vistas en su contexto. Una vinculación remota impide al creador
liberar la reserva aunque ya haya desregistrado a sus propios trabajadores.
Un timeout ambiguo conserva la información para reintentar el cierre. El
cierre del contexto retira primero las vistas remotas, comprueba las propias
y al terminar restaura las señales. Las copias controladas por página
adquieren los permisos MSI y copian por Cr bajo el mutex del directorio.
Una espera de red libera ese mutex. Las copias con rangos que se solapan
se coordinan dentro del proceso. El programa debe sincronizar sus accesos
directos a Ar. El paginador y la cola de campañas del hogar conservan su
modelo de atención serial. Una campaña lenta puede retrasar otro fallo
aunque sus páginas mantengan datos, permisos y versiones independientes.

**Fase C pendiente.** Cada nodo puede crear reservas con hogar propio.
Una sola reserva usa la capacidad del creador y hasta 16 tramos de ese
hogar. Cuando la capacidad local o la representación en tramos se agotan,
la creación devuelve `DSM_ERR_NOMEM`. Para usar otro aporte dentro de la
misma reserva hacen falta asignación y liberación coordinadas en cada hogar.
El diseño prevé `ALLOC_REQUEST`, `ALLOC_GRANT`, `ALLOC_DENY` y
`ALLOC_RELEASE`. Faltan su implementación de extremo a extremo, la
publicación después de todas las concesiones y la reversión de las
asignaciones parciales. Queda pendiente la aceptación con dos hogares
reales y un tercer participante.

Los comandos reproducibles de compilación, de la suite con plazo de 45 s y
de las dos demostraciones en Ubuntu o Linux Mint están en la
[guía del proyecto](../README.md#dos-aplicaciones-desde-terminales-independientes).

### Verificaciones del Paso 11B

Las demostraciones se ejecutaron con dos procesos Linux en espacios de nombres
de red distintos unidos por veth, usando 192.0.2.1 y 192.0.2.2 en la máquina
virtual y los puertos TCP 27200 y 27201. En el caso de una página, el creador
recuperó `Adios TEC`. En el de tres páginas, la página central cambió y las
dos vecinas conservaron sus cadenas originales. Intentar liberar desde el
creador con el cliente conectado devolvió `DSM_ERR_OCUPADA`. Ambos procesos
salieron con código 0 al cerrar primero el cliente. La prueba física entre
dos equipos Linux Mint queda pendiente.

La aceptación local del API del Paso 11A exigía rechazar una segunda reserva
y trataba el tamaño redondeado como límite público. El Paso 11B exige
varias reservas y el tamaño lógico original. Se actualizaron esas aserciones
para comprobar que la segunda reserva conserva sus datos al liberar la
primera, que el relleno queda fuera del rango y que el manejador sigue
activo mientras vive el contexto. Se conserva la comprobación de restaurar
SIGSEGV al cerrar. Las otras ocho aceptaciones históricas conservan sus
archivos originales.

En pruebas adicionales del directorio se ocupó y liberó capacidad hasta
dejar huecos. Una reserva de tres páginas con GPN 1, 3 y 4 mantuvo una
vista virtual contigua. Con 16 huecos de una página y un tramo libre de 18
páginas, el asignador pudo representar la petición de 18 con un solo tramo.
Con 17 huecos aislados y sin más páginas libres, la petición de 17 se
rechazó sin consumir capacidad ni identificador.

Las fases A, B local y B remota quedaron **implementadas y probadas**.
La fase C queda **sin implementar** por la dependencia de asignación
distribuida descrita arriba.

En Debian 12 x86-64, kernel 6.1.0-39-amd64, GCC 12.2.0 y GNU Make 4.3,
`make clean`, `make` y `make examples tests aceptacion` terminaron con
código 0 y sin advertencias de compilación. Cada binario se ejecutó bajo
`timeout -k 1s 45s`. El ejemplo local breve también terminó con 0.

| Aceptación | Código | Evidencia |
| --- | ---: | --- |
| `aceptacion_red` | 0 | Alta TCP de los pares y diálogo. |
| `aceptacion_memoria` | 0 | Dos vistas del mismo respaldo y protección individual. |
| `aceptacion_directorio` | 0 | Reserva, publicación y apertura del manifiesto. |
| `aceptacion_paginador` | 0 | SIGSEGV real, reanudación del trabajador y página vecina protegida. |
| `aceptacion_paginacion_remota` | 0 | Lectura por TCP con dos y tres procesos y aportes de distinto tamaño. |
| `aceptacion_escritura_local` | 0 | Fallos de escritura y concesiones del paginador local. |
| `aceptacion_api` | 0 | Contrato público actualizado, límites lógicos, reservas independientes, cierre y restauración de señales. |
| `aceptacion_coherencia` | 0 | Invalidaciones, ACK, concurrencia de solicitudes y reconexión. |
| `aceptacion_escritura_remota` | 0 | Escritura directa, lectura actualizada y dos escritores con el hogar en otro nodo. |
| `aceptacion_api_remoto` | 0 | Dos procesos, SIGSEGV, invalidación y ACK por TCP, paso S/S v1 → I/M v2 → S/S v3. Dos hilos del cliente adquieren una sola concesión M. Desvincular devuelve al hogar la última imagen escrita en M. |
| `aceptacion_multipagina` | 0 | Dos trabajadores, cruces, último byte válido, límites, copias solapadas y fallo inyectado al crear Cr, con P de 4096 y 8192. Comprueba cierre del descriptor, desmontaje de Ar y capacidad recuperada. |
| `aceptacion_multipagina_remota` | 0 | Tres páginas con 12 271 bytes lógicos. Se comprueban los permisos tras escribir la página central, la escritura que cruza un límite y todos los bytes en ambos procesos. |
| `aceptacion_api_errores` | 0 | Nodo ausente, ID inexistente, duplicados, vencimiento real, protección después de perder el hogar, reconexión con otra reserva y cierre ocupado. |

Tras la suite completa, `aceptacion_api_remoto`,
`aceptacion_multipagina_remota` y `aceptacion_api_errores` pasaron cinco
repeticiones adicionales cada una (15/15). Los puertos quedaron libres.
En una verificación adicional de seis rondas, los nodos 0 y 1 mantuvieron
abiertas simultáneamente las reservas 1 y 65537. Cada trabajador comprobó
sus escrituras en la vista propia y en la remota y ambos contextos cerraron
correctamente. Una variante con desfase de 125 ms devolvió
`DSM_ERR_OCUPADA` al primer nodo mientras seguía abierta su reserva y
permitió completar el cierre por reintento.

Al detener a la vez los participantes de algunas aceptaciones de la CLI,
el cierre de una vista puede avisar que el par ya salió. El API público
devuelve el error al llamador cuando necesita una confirmación del hogar.

La revisión queda en el árbol de trabajo de `prototipo-2`, con el mismo
HEAD `49ce66e`, 19 archivos versionados modificados y cinco archivos
nuevos de pruebas. El índice sigue vacío.

---

# Informe histórico de integración · 17 de noviembre de 2026

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
