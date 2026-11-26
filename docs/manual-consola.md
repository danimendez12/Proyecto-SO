# Manual de la consola DSM

Desde la raíz del repositorio en Linux, compilar e iniciar una terminal por puesto:

```bash
make
./bin/dsm_unir config/pool-lab.ini
```

Todos usan la misma configuración. En el laboratorio hay que sustituir previamente las veinte direcciones de ejemplo por las direcciones autorizadas. Una terminal reclama un puerto local, imprime `ALTA OK` con PID, nodo y puerto, y abre un proceso lógico. Con el INI de 14D ofrece 8 MiB, equivalentes a 2048 páginas de 4096 bytes. El primer prompt aparece con todas esas páginas libres en un nodo recién iniciado.

`./bin/dsm_unir CONFIG N` admite de 1 a 8 intentos locales. Con varios intentos, el último es interactivo. Los anteriores que lograron el alta siguen prestando memoria después de salir de la consola interactiva. El lanzador informa el resultado y el PID de cada intento. En un equipo con tres puestos, el cuarto intento falla mientras los tres estén reclamados. Para cerrar un secundario, devolver primero los préstamos y ejecutar `kill -TERM PID` usando el PID que imprimió el lanzador. Con una terminal independiente por puesto se puede utilizar `salir` en cada una.

Para proyectar únicamente los resultados en el laboratorio, en la terminal operativa ejecutar `script -q -f -c './bin/dsm_unir config/pool-lab.ini' sesion.log`. En otra terminal se puede seguir la salida de interés, por ejemplo `tail -F sesion.log | grep --line-buffered -E 'MEMORIA |MAPA TRAMO |LEER OK |TAREA OK |CIERRE |ERROR comando'`. El registro completo conserva las trazas TCP y MSI.

## Reservas y datos

Los argumentos numéricos son decimales. `OFFSET` cuenta bytes desde cero, `VALOR` es un byte de 0 a 255 y `ID` es el identificador completo que devolvió el creador. El texto se escribe literalmente hasta el fin de línea, incluidos los espacios entre palabras. Las comillas también son caracteres del texto. El máximo de la línea interactiva es de 4095 bytes.

| Comando | Resultado |
|---|---|
| `ayuda` | Imprime la sintaxis disponible. |
| `reservar TAMAÑO` | Crea una reserva propia. Admite bytes, `K`/KiB o `M`/MiB, por ejemplo `4097`, `64K` y `15M`. Devuelve ID, tamaño, páginas, creador y propietario. |
| `reservas` | Enumera todas las vinculaciones de esta consola, su origen, estado y tamaño. |
| `abrir ID` | Vincula por ID una reserva existente. Se pueden mantener varias a la vez. |
| `leer ID OFFSET BYTES` | Muestra los bytes en hexadecimal y texto escapado. El máximo por comando es 256. Una solicitud mayor informa el límite. |
| `escribir ID OFFSET TEXTO` | Escribe los bytes del texto en la reserva, sin terminador NUL. Puede atravesar una frontera de página o de hogar. |
| `llenar ID VALOR` | Escribe el mismo byte en toda la extensión lógica. |
| `verificar ID VALOR` | Compara todos los bytes con el valor indicado. Informa el primer desajuste y su offset. |
| `mapa ID` | Describe los tramos en **orden lógico de la reserva** con ordinal inicial, número de páginas, hogar y GPN inicial. |
| `tarea patron ID HILOS VALOR` | Reparte páginas entre Pthreads registrados y escribe y comprueba en cada byte un patrón que depende del offset y del valor. Informa rangos semiabiertos, bytes, resultado por trabajador y suma final. |
| `desvincular ID` | Cierra la vinculación del cliente con una reserva remota. |
| `liberar ID` | El creador devuelve los aportes de una reserva propia. Si hay clientes activos, conserva el ID para poder reintentar. |
| `salir` | Retira las vinculaciones y cierra el contexto. `CIERRE OK` confirma el cierre. Si aparecen recursos pendientes, resolverlos desde el cliente o el creador y reintentar. |

La tarea es síncrona para su terminal. Al terminar reaparece el prompt con las reservas vigentes. Se pueden consultar otros puestos mientras trabaja. El número de hilos debe estar entre uno y el máximo configurado. Si hay más hilos que páginas, algunos reciben un rango vacío. El último rango termina en el tamaño lógico incluso si sobra espacio en la página física. La tarea interactiva usa las operaciones controladas de lectura y escritura. La aceptación de Pthreads también ejercita la vista protegida `Ar` y las faltas de escritura reales.

Un resultado negativo en `COMANDO FIN` indica error. Tras capacidad insuficiente, argumentos inválidos o un fallo recuperable de una operación, se puede consultar `reservas`, corregir el comando y continuar. Un retiro pendiente se reintenta por ID. Las operaciones largas no constituyen transacciones sobre toda la reserva.

## Consultas

| Comando | Qué observar |
|---|---|
| `estado` | Identidad del puesto y resumen de capacidad y cobertura. |
| `local` | Ficha del propio puesto. |
| `nodos` | Una fila por puesto configurado con IPv4, puerto, nombre, estado conocido, capacidad, páginas y respuesta. Un signo `?` indica ocupación o disponibilidad desconocida. |
| `memoria` | Equipos potenciales, puestos configurados, respuestas, ausencias, bytes nominales y activos, ocupados y libres confirmados, páginas, utilización y cobertura. |
| `paginas local [DESDE [LIMITE]]` | Inventario autoritativo del hogar local. |
| `paginas nodo ID [DESDE [LIMITE]]` | Inventario del hogar indicado por su **ID de nodo**. Un hogar explícito inaccesible produce error. |
| `paginas global [DESDE [LIMITE]]` | Inventario por GPN de los hogares consultables. Una cobertura parcial se señala en la salida. |
| `copias local [DESDE [LIMITE]]` | Vistas del puesto observador, con sus permisos y validez. Una vista invalidada puede seguir vinculada. |
| `estadisticas` | Ocho contadores por puesto que respondió y suma por columna con cobertura. |
| `estadisticas nodo ID` | Los ocho contadores de un puesto concreto. |

`DESDE` es un GPN inclusivo. La salida por defecto contiene hasta 64 filas y admite un `LIMITE` de 1 a 1024. Cuando aparece `mas=SI`, repetir la misma consulta con `siguiente` como nuevo `DESDE`. Las filas autoritativas distinguen páginas libres, preparadas y asignadas. Una copia local no añade capacidad al total. La ocupación confirmada incluye los apartados en preparación. Un puesto sin respuesta aporta cero bytes a la capacidad **confirmada**, mientras su capacidad configurada sigue incluida en el total nominal. Las consultas a distintos hogares se toman en instantes distintos.

En una página asignada, `creador` identifica al nodo que creó la reserva y `propietario` al proceso lógico público estable. Las reservas históricas de servicio se distinguen por su procedencia y muestran propietario lógico cero. `solicitante` es el último proceso anotado, `hogar` administra el GPN y `poseedor` identifica al titular de la copia exclusiva cuando corresponde. La consola muestra un guion para un nodo ausente o un campo inaplicable. En el API, el centinela de nodo es 65535. Una página libre tiene procedencia `NINGUNA`. En una copia de un hogar remoto, `libre_hogar=?` indica que no se midió allí la capacidad libre. `orden` es el ordinal de la página en la reserva, `version` y `mascara` corresponden al estado de coherencia. Los permisos describen la vista del emisor. Para comprobar un cruce entre hogares, obtener en `mapa` el primer ordinal del segundo tramo y usar el offset `ordinal × tamaño_de_página − 4` al escribir ocho bytes.

Las columnas `lectura` y `escritura` cuentan intentos de resolver faltas de permiso. `inv_env` y `inv_rec` registran invalidaciones en sentidos distintos. `pag_env`, `pag_rec`, `bytes_env` y `bytes_rec` miden transferencias de datos de página. Para estimar el volumen contabilizado en la malla, usar por separado la columna de envío o la de recepción. Sumarlas contaría ambos extremos de una transferencia. Un acierto de caché puede completar una operación sin aumentar estos contadores.

La consola administra su propio contexto y un proceso lógico. Matrices y Merge Sort siguen siendo ejecutables independientes que reclaman sus propios puestos. El procedimiento completo con varias terminales y los controles de capacidad, coherencia y cierre están en [el guion de laboratorio](pruebas-fisicas-14d.md).
