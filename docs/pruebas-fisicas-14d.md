# Revisión en las computadoras físicas del TEC

Esta hoja se completa **en el laboratorio**. Los valores de abajo son criterios esperados para un clúster vacío con páginas de 4096 bytes. Las ejecuciones en espacios de red Linux se registran en [la validación de desarrollo](validacion-14d.md).

## Preparación

Usar la misma revisión revisada en A, B, C y D y **una sola plantilla** `config/pool-lab.ini`. Antes de arrancar, sustituir las 20 IP de ejemplo por las IPv4 autorizadas conservando orden, capacidades y puertos. Para reproducir los IDs del ejemplo, colocar A, B, C y D en las primeras cuatro posiciones. Confirmar la igualdad del INI en cada terminal:

```bash
sha256sum config/pool-lab.ini
getconf PAGESIZE
make clean && make && make apps examples tests aceptacion
```

No cambiar el INI durante la sesión. Deben poderse aceptar conexiones TCP directas entre los puestos autorizados. Mantener una terminal por proceso y copiar los IDs reales que aparezcan tras cada alta y reserva. `ID` en los comandos siguientes significa el entero completo devuelto por `reservar`.

## Guion por terminal

| Terminal | Acción | Qué registrar |
|---|---|---|
| A, B y C, una por equipo | En cada una, `./bin/dsm_unir config/pool-lab.ini`. Usar `nodos`, `estado`, `memoria`. | ID, puerto, puestos que respondieron, nominal y memoria libre. Con tres consolas vacías se esperan 24 MiB activos y 6144 páginas libres. |
| A | `reservar 15M`, `reservas`, `mapa ID`, `memoria`. | 15 728 640 bytes, 3840 páginas, propietario, tramos y GPN. Con A vacío se esperan 2048 locales y 1792 remotas. La reserva debe dejar 2304 páginas libres entre los tres puestos. |
| A | `escribir ID 0 Hola TEC`, `leer ID 0 8`, `escribir ID 4092 DosPag01`, `leer ID 4092 8`. | Dos lecturas exactas; la segunda cruza la primera frontera de página. |
| B | `abrir ID`, `leer ID 0 8`, `escribir ID 0 AdiosTEC`. | Mismo tamaño lógico y lectura inicial de ocho bytes. |
| A | `leer ID 0 8`. | Debe leer `AdiosTEC` después de la escritura remota. |
| B y después A | Si el mapa sitúa el cambio de hogar en la página 2048, B ejecuta `escribir ID 8388604 DosHog02` y A ejecuta `leer ID 8388604 8`. Para otro tramo usar `orden_inicial × 4096 − 4`. | Ocho bytes exactos atravesando dos hogares. |
| C | `paginas nodo HOGAR GPN 1`, con los valores reales del primer tramo remoto. Comparar antes y después de una escritura desde B. | Propietario y ordinal estables. Registrar último solicitante, MSI, poseedor, máscara y versión. |
| A | `tarea patron ID 4 37`, luego `reservas`. | Cuatro rangos, suma y bytes verificados. La consola queda disponible. |
| A, B y C | En cada una, `estado`, `nodos`, `paginas global 0 8`, `estadisticas`. En B, `copias local 0 8`. | Confirmados y ausentes, procedencia de las filas, contadores y continuación de la tabla. Para observar durante la tarea, consultar desde una segunda terminal mientras A trabaja. |
| A, B y A | A prueba `liberar ID` con B vinculado. B hace `desvincular ID`. A repite `liberar ID`, `memoria`, `reservar 64K`, `llenar NUEVO 42`, `verificar NUEVO 42` y `liberar NUEVO`. | Primer intento ocupado. Comprobar recuperación de las 6144 páginas libres y reutilización en la misma sesión. |
| A, terminales nuevas | Abrir un segundo y tercer `dsm_unir`. Probar un cuarto. Cerrar uno de los adicionales y volver a abrirlo. | Tres IDs, puertos 27200–27202, rechazo del cuarto y reutilización. Cerrar ambos adicionales para dejar puestos a los cálculos. |
| D | Iniciar otro `dsm_unir` con el mismo INI. Observar `nodos` desde A y luego cerrar D. | Alta tardía, paso de 24 a 32 MiB activos y regreso a 24 MiB después de la baja. |

## Aplicaciones independientes

Con A, B y C activos, usar una terminal adicional. La aplicación reclama su propio puesto.

```bash
./bin/apps/matrices config/pool-lab.ini 7 31 13
./bin/apps/matrices config/pool-lab.ini 1000 1000 1000
./bin/apps/mergesort config/pool-lab.ini 1000000 aleatorio 2026
./bin/apps/mergesort config/pool-lab.ini 2000000 aleatorio 2026
```

Para dos o tres procesos, ejecutar este bloque en terminales adicionales de A y B (también C si corresponde), dentro de la ventana de agrupación. Empezar con un cálculo y esperar su cierre antes del siguiente:

```bash
export DSM_DEMANDA_GRUPO=revision-tec
export DSM_DEMANDA_ESPERA_MS=10000
./bin/apps/matrices config/pool-lab.ini 128 128 128
echo "$?"
./bin/apps/mergesort config/pool-lab.ini 100001 repetidos 2026
echo "$?"
```

Registrar participantes, token, verificación del creador, cierre de cada trabajador y `memoria` antes y después. Desde todas las consolas usar `salir` al finalizar. Si un cliente o un aporte sigue activo, completar su entrega y reintentar el cierre.

## Registro del laboratorio

| Campo | Resultado físico |
|---|---|
| Fecha, distribución, kernel y revisión exacta | Pendiente |
| Equipos participantes y hash del INI compartido | Pendiente |
| IDs, puertos y cobertura desde A, B y C | Pendiente |
| Capacidad inicial, durante el préstamo y tras liberar | Pendiente |
| Propietario, hogares, ordinales, versiones y lectura cruzada | Pendiente |
| Tarea, resultados numéricos de las aplicaciones y códigos de salida | Pendiente |
| Cuarto intento, reutilización y alta tardía | Pendiente |
| Incidencias observadas y registros asociados | Pendiente |
