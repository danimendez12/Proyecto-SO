# Merge Sort paralelo con Pthreads y DSM

El creador publica dos reservas de `int32_t`; los trabajadores las abren desde procesos Linux independientes. El ordenamiento local y la fusión global adaptan la aplicación Pthreads de Brandon Lavello. El mismo binario tiene un modo secuencial para comparar tiempos. [Procedencia](UPSTREAM.md) · [Licencia MIT](LICENSE) · [Resultados](RESULTADOS.md)

## Demostración de un millón de enteros en tres terminales

Desde la raíz del proyecto, compilá con `make apps` (Linux x86-64, GCC C11, GNU Make y Pthreads). En una sola máquina usá los INI de loopback incluidos. Cada comando de la tabla ocupa una terminal. Iniciá el creador primero y esperá a que imprima `CONTROL id=…` y los dos comandos `EJECUTAR`. Los ejemplos usan el ID 1 de un creador nuevo. Si se anuncia otro, usá ese número en ambos trabajadores.

| Terminal | Comando |
| --- | --- |
| 1 · nodo 0 | `./bin/apps/mergesort crear config/mergesort-grande.ini 1000000 2 2 aleatorio 2026` |
| 2 · nodo 1 | `./bin/apps/mergesort trabajar config/mergesort-grande.ini 1 1` |
| 3 · nodo 2 | `./bin/apps/mergesort trabajar config/mergesort-grande.ini 2 1` |

Cada proceso inicia el servicio DSM de su nodo. Reservá un puerto distinto para cada ID y utilizá una sola instancia por ID. Los dos trabajadores permanecen en ejecución hasta devolver las vistas y los aportes. El creador debe imprimir `comparados=1000000 ordenado=si coincide=si`, `VERIFICACION OK` y `LIBERACION OK`. Cada trabajador debe imprimir `TRABAJADOR OK`. Los tres deben salir con código 0. La traza de páginas del motor puede ser extensa. Para seguir solo los hitos, redirigí la salida del comando a un log, por ejemplo `>nodo0.log 2>&1`, y consultalo con `grep -E '^(CONTROL|EJECUTAR|HOGAR|CALCULO|VERIFICACION|LIBERACION|ERROR)' nodo0.log`.

## Laboratorio con Linux Mint 22.3 y un INI común

Compilá la misma revisión en cada computadora. Comprobá con `getconf PAGESIZE` la compatibilidad con páginas DSM de 4096 bytes. Los participantes deben tener conectividad TCP entre sí en los puertos del INI (27200–27202 en esta guía). La demostración manual usa terminales ordinarias.

1. En la computadora A, desde la raíz del proyecto, generá una copia del INI con las IPv4 reales. En dos computadoras, el nodo 2 comparte la IP del 0. En tres computadoras, ingresá la IP del tercer equipo para el nodo 2.

```bash
read -rp 'IPv4 de A (nodo 0): ' ip0
read -rp 'IPv4 de B (nodo 1): ' ip1
read -rp 'IPv4 de C (Enter si el nodo 2 corre en A): ' ip2
ip2=${ip2:-$ip0}
awk -v a="$ip0" -v b="$ip1" -v c="$ip2" \
  '$1=="nodo" {$5=($3==0 ? a : ($3==1 ? b : c))} {print}' \
  config/mergesort-grande.ini > /tmp/mergesort-tec.ini
sha256sum /tmp/mergesort-tec.ini
```

2. Copiá ese **mismo archivo** a `/tmp/mergesort-tec.ini` en B, y también en C si participa. Podés usar `scp "/tmp/mergesort-tec.ini" "usuario@$ip1:/tmp/mergesort-tec.ini"` desde A, con la cuenta real del laboratorio. Compará el SHA-256 en cada equipo. En las líneas `nodo = ID NOMBRE IPv4 PUERTO BYTES`, el puerto es único y la capacidad se expresa en bytes múltiplos de página. Al ajustar la capacidad, dejá al menos tres páginas en el nodo 0 para el control de dos trabajadores.
3. Ejecutá en orden de arranque, reutilizando el ID anunciado.

| Equipo con dos máquinas | Equipo con tres | Comando desde la raíz |
| --- | --- | --- |
| A, terminal 1 | A | `./bin/apps/mergesort crear /tmp/mergesort-tec.ini 1000000 2 2 aleatorio 2026` |
| B | B | `./bin/apps/mergesort trabajar /tmp/mergesort-tec.ini 1 1` |
| A, terminal 2 | C | `./bin/apps/mergesort trabajar /tmp/mergesort-tec.ini 2 1` |

El INI grande presta 1 MiB, 16 MiB y 4 KiB en los nodos 0, 1 y 2. A cruza los hogares 0 y 1 y B reside en 1. Para reproducir el ensayo de **dos reservas individualmente multihogar** con un millón de enteros, conservá 1 MiB en el nodo 0 y poné 4194304 y 5242880 bytes en los nodos 1 y 2 del INI común. A cruza de 0 a 1 y B de 1 a 2. Este cambio usa la política ordinaria de asignación del DSM. En cada ejecución, las líneas `RESERVA`, `TRAMO` y `HOGAR` dan el mapa obtenido y los bytes realmente ocupados, incluido el control.

## Tamaños, patrones y pruebas reproducibles

La sintaxis del creador es `crear INI N TRABAJADORES HILOS PATRON SEMILLA [ARCHIVO]`. La semilla es un entero de 32 bits sin signo. Los patrones admitidos son `aleatorio`, `inverso`, `ordenado`, `negativos`, `repetidos`, `iguales` y `archivo`. Este último requiere exactamente N enteros decimales con signo de 32 bits separados por espacios, tabuladores o saltos de línea. Solo el creador necesita el archivo. El patrón aleatorio toma valores de un generador congruencial reproducible, y `repetidos` concentra los datos en nueve valores, de −4 a 4.

```bash
make apps aceptacion
timeout -k 5s 180s ./bin/tests/aceptacion_app_mergesort
./bin/apps/mergesort secuencial 1000000 aleatorio 2026

# Tres procesos y dos hogares; los logs quedan en RESULTADOS=…
bash apps/mergesort/prueba_red.sh --loopback 65539 config/mergesort-lab.ini 2 repetidos 2026
bash apps/mergesort/prueba_red.sh --loopback 1000000 config/mergesort-grande.ini 2 aleatorio 2026
bash apps/mergesort/prueba_red.sh --loopback 2000000 config/mergesort-grande.ini 2 aleatorio 2026

# Dos pilas TCP, nodos 0 y 2 en A y nodo 1 en B
bash apps/mergesort/prueba_red.sh --namespaces 65539 config/mergesort-lab.ini 2 aleatorio 2026

# Dos procesos, una página de datos, archivo con extremos y duplicados
printf '%s\n' '-2147483648 7 7 0 2147483647' > /tmp/mergesort-entrada.tsv
MERGESORT_TRABAJADORES=1 MERGESORT_EXIGIR_MULTIHOGAR=0 \
  bash apps/mergesort/prueba_red.sh --loopback 5 config/mergesort-lab.ini 1 archivo 0 /tmp/mergesort-entrada.tsv
```

El guion requiere Bash, iproute2, util-linux y las utilidades usuales de GNU. Para `--namespaces`, si la política de Ubuntu en VirtualBox exige privilegios, se puede invocar el mismo comando con `sudo bash`; la prueba manual de tres terminales sigue disponible sin cambiar la configuración de seguridad. Para casos pequeños que caben en el hogar local, usá `MERGESORT_EXIGIR_MULTIHOGAR=0` en el guion. La aplicación aplica un plazo global de coordinación de 1800 s, ajustable con `MERGESORT_PLAZO_SEG`. Cada operación de red conserva además su propio `rpc_timeout_ms` del INI. El guion también limita la duración de cada proceso a 240 s con `timeout`. Para un ensayo largo, por ejemplo, ejecutá `MERGESORT_PRUEBA_LIMITE=1800 MERGESORT_PLAZO_SEG=1700 bash apps/mergesort/prueba_red.sh --namespaces 2000000 config/mergesort-grande.ini 2 aleatorio 2026`.

Para ejecutar todos los binarios de aceptación en serie y detenerse en el primer fallo:

```bash
set -e
make clean
make
make apps examples tests aceptacion
for prueba in bin/tests/aceptacion_*; do
    timeout -k 5s 180s "$prueba"
    printf '%s exit=0\n' "$prueba"
done
```

## De la entrada al resultado global

1. **Alta e inicialización.** El creador valida la entrada, guarda una copia privada para la referencia y reserva primero una cabecera y una página de confirmación por trabajador. Cada página de control tiene un solo escritor. Los trabajadores abren el control y confirman la generación 0. Solo entonces el creador obtiene A y B con `dsm_reservar`, inicializa A con `dsm_escribir` y publica los IDs. Cada trabajador comprueba el tamaño de las dos reservas al abrirlas.
2. **Bloques.** Los bloques abarcan de 1 a 16 páginas DSM. Cada proceso recibe bloques contiguos exclusivos, y reparte los suyos entre Pthreads con `pthread_create` y `pthread_join`. Cada hilo se registra, lee un bloque, lo ordena con Merge Sort iterativo, lo escribe en A y se desregistra. Con páginas de 4 KiB son hasta 128 KiB de datos y auxiliar por hilo. Si hay más participantes que bloques, los que reciben cero tareas siguen confirmando cada barrera.
3. **Fusión.** En cada generación, una tarea combina dos bloques consecutivos. Lee dos páginas de entrada por cursores, escribe la salida una página a la vez y copia la cola impar. La fuente y el destino alternan entre A y B. Un proceso solo escribe páginas de sus propias tareas. El ancho se duplica después de que cada trabajador confirma el mismo número de generación. En el último nivel queda una sola pareja, que fusiona un hilo. La aplicación usa `dsm_leer` y `dsm_escribir` para propagar errores de acceso y preservar MSI. No necesita obtener punteros directos con `dsm_traducir`.
4. **Cierre de vistas y verificación.** La fase de cierre exige un único bloque global. Los trabajadores retiran las vistas de A y B antes de confirmarla. El creador contrasta todos los enteros con el original guardado. Ordena tramos privados de hasta 65 536 valores con `qsort` y obtiene el siguiente valor esperado mediante una cola de prioridad, independiente de la combinación del algoritmo. Revisa además el orden ascendente y guarda la primera diferencia. Los dos archivos de `tmpfile()` pertenecen exclusivamente al verificador local. Requieren aproximadamente `8*N` bytes de almacenamiento temporal en el creador; los trabajadores intercambian datos únicamente por las reservas DSM.
5. **Devolución.** El creador libera B, A y control. Tras desvincularse, un trabajador que aporta páginas mantiene el servicio activo hasta que `dsm_cerrar` acepta el cierre. Un código de error de la aplicación, de la referencia o del DSM produce una salida distinta de cero. Un error permanente de red puede impedir devolver un aporte antes del plazo. La salida distingue el resultado numérico de la liberación.

El creador organiza las fases de esta ejecución de la aplicación. El transporte, la asignación de hogares y los permisos siguen a cargo del motor DSM en cada nodo. Los roles se eligen al ejecutar el binario y se asignan los IDs de la demostración desde el INI.
