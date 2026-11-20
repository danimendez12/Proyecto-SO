# Multiplicación de matrices con Pthreads y DSM

El nodo 0 crea A, B y C mediante `dsm_reservar`. Los procesos de cálculo abren los mismos tres IDs, leen las entradas con `dsm_leer` y escriben los bloques de C con `dsm_escribir`. El creador recupera C por la misma reserva y verifica el producto contra una referencia secuencial. La adaptación parte de `thread2.c` de Mathias Perlet. [Procedencia y licencia](UPSTREAM.md) · [Resultados](RESULTADOS.md)

## Compilar y probar en Linux

Se necesita Linux x86-64, GCC para C11, GNU Make y Pthreads. La aplicación usa el transporte y el paginador existentes del DSM. Desde la raíz del proyecto:

```bash
make apps
make aceptacion
timeout -k 2s 90s ./bin/tests/aceptacion_app_matrices
```

Para ejecutar una multiplicación de **128 × 128** en una máquina, abrí tres terminales en la raíz. Iniciá el creador y esperá la línea `CONTROL id=…`. Luego iniciá los dos trabajadores. Usá el ID anunciado (en un proceso nuevo es 1).

| Terminal | Comando |
| --- | --- |
| Nodo 0 | `./bin/apps/matrices creador config/matrices-lab.ini 128 128 128 2 300` |
| Nodo 1 | `./bin/apps/matrices trabajador config/matrices-lab.ini 1 1 2 300` |
| Nodo 2 | `./bin/apps/matrices trabajador config/matrices-lab.ini 2 1 2 300` |

El creador imprime los comandos de arranque, los IDs, los bytes y los primeros GPN de cada tramo. Con el INI del ejemplo y las contribuciones libres, A y B ocupan 32 páginas cada una en el nodo 0. La primera mitad de C comienza en GPN 65 del nodo 0 y la segunda en GPN 81 del nodo 1. Los trabajadores 1 y 2 calculan 16 páginas cada uno con dos Pthreads por proceso. Esperá `VERIFICACION OK`, `LIBERACION OK` y `TRABAJADOR OK` en ambos trabajadores. El cierre es automático.

### Uso en dos o tres computadoras del TEC

1. Compilá con `make apps` en cada computadora desde la misma revisión. Usá una sesión Linux Mint 22.3 x86-64 con páginas de 4096 bytes (`getconf PAGESIZE`). Los tres procesos necesitan poder comunicarse entre ellos por TCP en los puertos 27200–27202.
2. En **cada terminal**, ejecutá el bloque siguiente. En dos computadoras, ingresá la IPv4 de la computadora A para los nodos 0 y 2, y la de la computadora B para el nodo 1. Para tres computadoras, ingresá la IPv4 de cada una en orden 0, 1, 2. Cada proceso obtiene su propio archivo de configuración.

```bash
read -rp 'IPv4 del nodo 0: ' ip0
read -rp 'IPv4 del nodo 1: ' ip1
read -rp 'IPv4 del nodo 2: ' ip2
INI=$(mktemp)
awk -v a="$ip0" -v b="$ip1" -v c="$ip2" \
  '$1=="nodo" {$5=($3==0 ? a : ($3==1 ? b : c))} {print}' \
  config/matrices-lab.ini > "$INI"
```

3. Ejecutá los comandos de la tabla en orden. En las terminales de los trabajadores, sustituí `1` en la posición de ID de control si el creador anunció otro número. Conservá la ruta del INI **local** en cada equipo.

| Terminal | Ubicación con dos computadoras | Ubicación con tres | Comando | Confirmación |
| --- | --- | --- | --- | --- |
| Creador | A | A | `./bin/apps/matrices creador "$INI" 128 128 128 2 300` | `CONTROL id=1` |
| Trabajador 1 | B | B | `./bin/apps/matrices trabajador "$INI" 1 1 2 300` | `ALTA nodo=1` |
| Trabajador 2 | A | C | `./bin/apps/matrices trabajador "$INI" 2 1 2 300` | `ALTA nodo=2` |

4. Dejá vivos los tres procesos durante la comprobación y la devolución de memoria. El trabajador que aporta páginas sigue atendiendo peticiones después de terminar su bloque hasta que el creador libere sus aportes. Cada proceso termina con código 0 al completar su rol. No hace falta compartir discos, copiar matrices al trabajador ni iniciar `dsm_node` junto a la aplicación.

Para **1000 × 1000**, sustituí `matrices-lab.ini` por `matrices-grande.ini` al generar los INI y ejecutá el creador con `1000 1000 1000 2 1800`. En los comandos de los trabajadores usá también `1800` de plazo. A, B y C requieren **24 000 000 bytes lógicos**. El archivo grande mantiene aportes de 16 MiB, 12 MiB y una página en los nodos 0, 1 y 2.

El DSM puede producir muchas líneas de traza al transferir miles de páginas. Para una terminal de laboratorio podés guardar la traza completa y seguir las fases principales así:

```bash
set -o pipefail
./bin/apps/matrices creador "$INI" 1000 1000 1000 2 1800 2>&1 |
  tee matrices-nodo0.log |
  grep --line-buffered -E '^(CONTROL|EJECUTAR|MATRIZ|TRAMO|INICIALIZACION|VERIFICACION|LIBERACION|ERROR)'
```

### Integración en dos redes Linux

El guion crea dos espacios de nombres de red y un par veth. Los nodos 0 y 2 corren en una pila TCP y el nodo 1 en la otra. Usa `bash`, `ip`, `unshare`, `nsenter` y `timeout`. El sistema debe permitir crear espacios de nombres de usuario y red. Deja los INI independientes y las tres trazas en el directorio temporal que imprime al comenzar.

```bash
make apps
bash apps/matrices/prueba_red.sh 128 config/matrices-lab.ini 2 2 300
bash apps/matrices/prueba_red.sh 1000 config/matrices-grande.ini 2 2 1800
```

## Dimensiones, entradas y referencia original

En `creador INI F K N W [PLAZO_SEG]`, A tiene forma F×K, B tiene forma K×N y C tiene forma F×N. Los trabajadores son los nodos 1 a W, deben figurar en el INI y cada uno recibe al menos una página de C. En `trabajador INI NODO CONTROL HILOS [PLAZO_SEG]`, los hilos solicitados deben caber en `max_hilos`. Si un nodo recibe menos páginas que hilos, crea un hilo por página. El plazo predeterminado de coordinación es de 1800 segundos. Los RPC conservan el plazo del INI.

El modo generado usa enteros en `[-8,8]` representados como `double`, con semillas fijas 2026 y 2027. Para usar archivos, solo el creador necesita A y B. Cada línea representa una fila, con columnas separadas por tabulaciones o espacios. La aplicación comprueba los anchos, la compatibilidad de dimensiones, los valores finitos y los tamaños antes del cálculo.

```bash
# Referencia con los mismos datos que usaría «creador … 4 3 5».
D=$(mktemp -d)
./bin/apps/matrices exportar A 4 3 "$D/A.tsv"
./bin/apps/matrices exportar B 3 5 "$D/B.tsv"
time ./bin/apps/matrices_secuencial "$D/A.tsv" "$D/B.tsv" > "$D/C-original.tsv"

# Terminal 0, con los archivos de entrada en este equipo
./bin/apps/matrices archivos config/matrices-lab.ini "$D/A.tsv" "$D/B.tsv" 1 300
# Terminal 1, después de CONTROL id=1
./bin/apps/matrices trabajador config/matrices-lab.ini 1 1 2 300
```

El exportador crea archivos nuevos. Para usar la referencia original, conservá la tabulación entre columnas y el salto de línea al final de cada fila. La ejecución secuencial incluye el análisis de los TSV y la escritura de todo C. La aplicación distribuida imprime muestras, tiempos y el resultado de la verificación.

## Distribución, sincronización y corrección

- **Almacenamiento.** Cada matriz posee un ID independiente. A y C se guardan por filas. B se almacena transpuesta, de modo que `B_t[j*K+k]` es el elemento matemático `B[k,j]`. Ningún puntero de un proceso se guarda en el DSM. La línea `TRAMO` procede de `dsm_ubicacion_pagina`. Un único tramo puede cubrir muchas páginas.
- **Inicio.** El creador reserva primero una página de control con dimensiones y una marca por trabajador. Cada trabajador abre ese ID y confirma su alta. Cuando están todos disponibles, el creador reserva A, B y C, inicializa las entradas y llena C con NaN. Solo entonces publica los tres IDs y la fase de cálculo. La espera consulta marcas reales en el DSM.
- **Cálculo.** Un proceso recibe el intervalo de páginas de C desde `floor(P*(nodo-1)/W)` hasta `floor(P*nodo/W)`, con extremo superior excluido. Divide ese intervalo entre Pthreads siguiendo el reparto estático del original. Cada hilo llama a `dsm_hilo_registrar`, trae A y B con `dsm_leer` y conserva el orden de suma en `k`. Publica cada página de C con `dsm_escribir` y se desregistra. Así un fallo de red se devuelve como código del API y no termina el proceso. La memoria auxiliar de salida ocupa como máximo una página, más las copias locales de A y B.
- **Finalización.** Cada trabajador espera a sus Pthreads, desvincula A, B y C, y escribe su resultado en su propia marca de control. El creador exige las W marcas de éxito para verificar. Un error, una marca pendiente o el vencimiento del plazo impiden anunciar éxito. El creador libera C, B, A y control y cierra su contexto. El trabajador desvincula control y mantiene vivo su contexto mientras el DSM indique que quedan aportes en uso.
- **Verificación generada.** Si las tres dimensiones son como máximo 256, el creador revisa todas las entradas y compara cada elemento de C con un producto secuencial independiente. Para dimensiones mayores recorre toda C, comprueba integridad numérica y contrasta cuatro proyecciones por fila de `C·r` con `A·(B·r)`. También recalcula productos punto de muestras distribuidas por filas y páginas. Estas proyecciones reproducibles pueden no distinguir ciertos errores simultáneos que se cancelen en todas ellas.
- **Verificación desde archivos.** El creador conserva las entradas originales para calcular una referencia independiente con acumulación en `long double` y comparar cada salida con tolerancia según la suma de magnitudes y la precisión de `double`. Los trabajadores reciben los datos exclusivamente mediante DSM.
- **Fallos y mediciones.** Se usan `CLOCK_MONOTONIC`, códigos del API y confirmaciones de cierre. El tiempo de cálculo incluye adquirir las páginas de entrada y publicar C. El total del creador incluye esperar el arranque de los trabajadores. El API carece de contadores globales de tráfico y fallos. Ante pérdida definitiva del hogar de un acceso directo a Ar, el DSM puede terminar al trabajador con SIGSEGV. Una reserva con copias o aportes sin devolver puede quedar retenida hasta resolver la pérdida.
