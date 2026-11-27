# Documentación del DSM

La entrega 14D incluye el [informe de validación](validacion-14d.md), la [arquitectura](arquitectura-final.md), el [manual de la consola](manual-consola.md) y el [guion para las computadoras del TEC](pruebas-fisicas-14d.md). Los comandos de instalación están en la [guía de laboratorio](guia-laboratorio.md).

La [validación del Paso 13B](validacion-13b.md) y las secciones siguientes conservan los resultados y decisiones de los prototipos anteriores.

Este directorio reúne las decisiones de arquitectura y las evidencias de prueba.

## Paso 11B-C · Reservas multihogar

### Del diseño anterior a una sola reserva distribuida

El punto de partida fue la rama limpia `prototipo-2` en `6d68c45`. Se
revisaron API, directorio, memoria, paginador, protocolo, coherencia, red,
configuración, ejemplos, Makefile y aceptaciones. El manejador ya entregaba
un ordinal y el paginador ya podía resolver su GPN. El directorio rechazaba
tramos de otro hogar, el asignador comprobaba solo la capacidad local y el
cierre de la reserva suponía que el creador era su único hogar. Además, la
inicialización de Cr escribía ceros en todas las páginas del respaldo.

| Archivos | Cambio para el diseño de arquitectura |
| --- | --- |
| `src/dsm.c`, `include/dsm.h` | El proceso creador coordina una asignación puntual y conserva la lista de confirmaciones pendientes. La consulta aditiva `dsm_ubicacion_pagina` permite inspeccionar desde C la misma reserva en el creador y en sus clientes. |
| `src/directory.c`, `include/directory.h` | Cada nodo usa su mapa de ocupación para todas las reservas. Distingue las páginas que aporta, las copias locales de otros hogares y su posible vinculación como aplicación. |
| `src/memory.c`, `include/memory.h` | Las vistas siguen usando un memfd. `fallocate` respalda solo los tramos aportados por ese hogar. Los tramos remotos permanecen dispersos hasta que se instalan copias en Cr. |
| `src/protocol.c`, `include/protocol.h` | Codificación de preparación, confirmación, cancelación, liberación y sus respuestas. El manifiesto admite hasta 16 tramos por hogar y 256 en total. Las solicitudes de página con identidad ampliada incluyen reserva y ordinal. |
| `src/coherence.c`, `include/coherence.h` | Se conserva el lector único por conexión. Una cola de capacidad tramita asignaciones. Cada hogar procesa la devolución de copias en la misma cola que sus solicitudes de página. |
| `src/main.c` | Al recibir SIGINT, la CLI que aportó memoria a otro creador mantiene TCP y MSI activos hasta la devolución de los tramos. |
| `examples/demo_creator.c`, `config/lab-multihogar.ini` | Modo `aportar`, impresión del mapa y configuración reproducible de tres nodos. |
| `Makefile`, `tests/api_prueba.h`, `tests/aceptacion_multihogar.c`, `tests/aceptacion_multihogar_errores.c` | Dos aceptaciones con procesos independientes. El auxiliar consulta el mapa y traduce direcciones. La prueba de errores descarta un `CAP_RELEASE_ACK` con `--wrap=send` en su propio ejecutable. |
| `README.md`, `docs/README.md` | Comandos de laboratorio, decisiones y evidencia de esta revisión. |

### Asignación, acceso y liberación

1. **Preparar.** El creador combina su ID de nodo con un consecutivo de
   16 bits. Calcula la cantidad de páginas con el tamaño configurado,
   consulta primero su propio aporte y después a los otros nodos por ID.
   `CAP_PREPARE` devuelve los tramos que el hogar pudo marcar en ese
   instante bajo su mutex. Dos solicitudes concurrentes no pueden obtener
   el mismo GPN. Las transacciones de capacidad se identifican por reserva
   y por un token de sesión del creador.
2. **Confirmar y publicar.** Una vez cubiertos todos los ordinales, cada
   participante recibe el mismo mapa por `CAP_COMMIT` y respalda su parte.
   El creador registra Ar en su paginador y habilita `REGION_LOOKUP`
   después de recibir las confirmaciones. Si falla una etapa, envía
   `CAP_CANCEL` también a los hogares que pudieron preparar antes de un
   timeout. Conserva los pendientes y reintenta la cancelación en futuras
   reservas o al cerrar. Una respuesta tardía se correlaciona por par y
   por solicitud.
3. **Enrutar.** Para un desplazamiento `d`, la página lógica es
   `d / P` y la posición interna es `d % P`. El tramo proporciona
   `GPN = gpn_inicial + ordinal - orden_inicial`. Su hogar es el nodo
   cuyo intervalo de GPN contiene ese valor. Los límites se calculan
   acumulando los aportes del INI y dividiendo por `P`. El tamaño lógico
   conserva su valor original aunque el último tramo termine en una
   página parcial. Se usa el mismo mapa en el acceso directo y las copias
   que cruzan un límite.
4. **Mantener MSI.** El hogar de cada GPN coordina versiones, lectores y
   poseedor M. Un fallo en Ar solicita una sola página o su exclusividad.
   El lector instala la respuesta por Cr y el directorio ajusta el permiso
   de esa página. I usa `PROT_NONE`, S admite lectura y M admite
   lectura y escritura. Una página vecina puede conservar cualquiera de
   los otros estados.
5. **Desvincular.** Antes de cerrar su apertura ante el creador, el
   cliente obtiene una confirmación de cada hogar. Una copia M entrega
   su imagen vigente y una copia S pierde su permiso. Un donante que
   también usó la reserva como aplicación conserva su respaldo
   autoritativo después de desvincularse.
6. **Liberar.** Una apertura vigente impide despublicar y devuelve
   `DSM_ERR_OCUPADA`. Sin clientes ni trabajadores activos, el creador
   cierra sus copias en los demás hogares y pide `CAP_RELEASE`. Recuerda
   qué hogares respondieron y termina después de todas las confirmaciones.
   El registro terminal del participante permite reconocer las
   retransmisiones. El donante puede cerrar su propio contexto después de
   devolver todos sus aportes.

### Resultados en Linux x86-64

Se compiló una copia de las fuentes del árbol de trabajo en Debian 12.12,
kernel 6.1.0-39-amd64, GCC 12.2.0, GNU Make 4.3 y páginas de 4096 bytes.
`make clean`, `make` y `make examples tests aceptacion` terminaron con
código 0. GCC compiló los objetivos del proyecto sin advertencias.

| Aceptación anterior | Código | Aceptación anterior | Código |
| --- | ---: | --- | ---: |
| `aceptacion_red` | 0 | `aceptacion_memoria` | 0 |
| `aceptacion_directorio` | 0 | `aceptacion_paginador` | 0 |
| `aceptacion_paginacion_remota` | 0 | `aceptacion_escritura_local` | 0 |
| `aceptacion_api` | 0 | `aceptacion_coherencia` | 0 |
| `aceptacion_escritura_remota` | 0 | `aceptacion_api_remoto` | 0 |
| `aceptacion_multipagina` | 0 | `aceptacion_multipagina_remota` | 0 |
| `aceptacion_api_errores` | 0 | | |

| Aceptación nueva | Primera ejecución | Cinco repeticiones adicionales | Plazo por ejecución |
| --- | ---: | --- | ---: |
| `aceptacion_multihogar` | 0 | 5/5 con código 0 | 60 s |
| `aceptacion_multihogar_errores` | 0 | 5/5 con código 0 | 60 s |

**Mapa y coherencia observados.** El creador publicó el ID 1 con 12 277 bytes.
El cliente del nodo 2 abrió el mismo ID y obtuvo una sola Ar contigua en su
proceso. El ensayo ocupó previamente el GPN 2 para obligar a atravesar un
hueco en el espacio global.

| Ordinal | Primer byte en Ar | Hogar | GPN |
| ---: | ---: | ---: | ---: |
| 0 | 0 | 0 | 0 |
| 1 | 4096 | 0 | 1 |
| 2 | 8192 | 1 | 3 |

Los fallos SIGSEGV generaron solicitudes TCP a los hogares 0 y 1. La traza
registró versión 2 para GPN 0 y versión 3 para GPN 3. Dos procesos
escribieron simultáneamente bytes distintos del GPN 3. En las seis
ejecuciones quedó un único permiso `rw-s` y el otro proceso mostró
`---s`. Las dos páginas vecinas conservaron `r--s`. Las relecturas de
los dos escritores y una apertura posterior en el hogar 1 recuperaron
ambos bytes. Después de liberar se pudieron reservar de nuevo las dos
páginas de cada hogar.

**Errores y concurrencia.** Se solicitaron cinco páginas cuando solo quedaban
cuatro libres. Ambos hogares cancelaron los tramos parciales y la siguiente
reserva obtuvo las cuatro. Con un hogar detenido por `SIGSTOP`, el creador
devolvió `DSM_ERR_TIEMPO`. Al reanudarlo, se completó la cancelación y se
pudo reservar de nuevo. Perder un ACK de liberación también produjo
`DSM_ERR_TIEMPO`. Repetir la operación con el mismo ID devolvió `DSM_OK`.
Con un cliente abierto, liberar devolvió `DSM_ERR_OCUPADA`. Dos solicitudes
competidoras de tres páginas con solo cuatro libres nunca obtuvieron ambas
la concesión. Dos reservas concurrentes de dos páginas obtuvieron IDs y GPN
disjuntos.

**Medición del respaldo.** En un ensayo complementario, una reserva de
48 MiB (12 288 páginas) ocupó 32 MiB en el memfd del hogar 0 y 16 MiB en el
del hogar 1. La medición usó `st_blocks × 512` de cada memfd. El cliente
tenía 48 MiB de tamaño lógico y cero bloques de datos asignados al abrir.
Después de leer la primera página tenía 4096 bytes asignados y después de
leer la última, 8192. Al cerrar se liberaron los memfd y los puertos.

**Otras ejecuciones.** Se probaron tres procesos en dos espacios de nombres
de red Linux conectados por un par veth, con INI independientes, IPv4
192.0.2.1 y 192.0.2.2 y puertos 27200–27202. El lector modificó la tercera
página de una reserva de cuatro y el creador verificó las cuatro antes de
cerrar. En otra ejecución el creador había agotado su aporte y obtuvo una
reserva exclusivamente de los hogares 1 y 2. Se recuperó una escritura
que cruzaba esa frontera. También se verificaron un reinicio ordenado del
creador con donante activo, mensajes duplicados de una sesión anterior y
un `SIGINT` en la CLI donante. La CLI sirvió una lectura y escritura después
de la señal y terminó con código 0 tras la devolución del aporte.

### Límites y reproducción

- La recuperación por reintento requiere que los participantes conserven su
  estado. Si se pierde definitivamente un participante, pueden quedar páginas
  retenidas o indisponibles. Las marcas de transacciones finalizadas se
  conservan en memoria durante la sesión. El ID público tiene 16 bits de
  consecutivo por creador y puede repetirse al reiniciar el contexto. El
  token distingue las operaciones de capacidad entre sesiones. Después de
  una caída, el ID por sí solo no identifica una apertura antigua.
- Una reserva puede cubrir miles de páginas consecutivas por tramo. Con
  más de 16 intervalos libres separados por hogar, la fragmentación puede
  impedir una solicitud incluso si la suma de páginas libres es suficiente.
  El procesamiento de las campañas MSI sigue siendo serial en cada hogar.
- La validación de este paso se ejecutó en una VM Debian. Queda pendiente
  repetirla con GCC 13.3 en los equipos físicos Linux Mint 22.3 del TEC.
  Los ejemplos usan las IPv4 del INI y admiten varios nodos por equipo.

Los comandos exactos de compilación y ejecución para Ubuntu y Mint están
en la [guía de tres terminales](../README.md#demostración-multihogar-en-tres-terminales)
y en la [lista de aceptaciones](../README.md#pruebas-de-aceptación).

El trabajo quedó en el árbol de `prototipo-2`, sobre `6d68c45`, con
16 archivos versionados modificados y 3 nuevos pendientes de agregar.
El índice permaneció vacío de cambios y se conservó la referencia de la rama.

---

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
