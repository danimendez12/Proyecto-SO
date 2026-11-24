# Guía de laboratorio

Esta guía describe cómo arrancar terminales y ejecutar una prueba en las computadoras del laboratorio. Los comandos se ejecutan desde la raíz del proyecto, en Linux x86-64.

El archivo de las computadoras es `config/pool-lab.ini`. Los puertos van de **27200** a **27299**. Una terminal no se anota en el archivo: al arrancar ocupa el primer puerto libre y busca procesos activos en las IP registradas.

## Antes de ir

1. En `config/pool-lab.ini`, dejar una línea `equipo` por cada computadora que vaya a participar, con su IPv4 real. Quitar las que no se usen.

```ini
equipo = 192.168.1.15
equipo = 192.168.1.20
```

2. Copiar ese mismo archivo a todas las computadoras. Debe ser idéntico en cada una.
3. En cada máquina, una vez:

```bash
make
```

Con las veinte líneas de ejemplo, cada computadora admite dos procesos a la vez. Si el archivo incluye menos máquinas, cada una recibe más puertos.

## Inicializar la primera terminal

En una computadora donde no hay ningún proceso:

```bash
./bin/dsm_unir config/pool-lab.ini
```

El proceso ocupa el puerto 27200 de esa máquina y permanece en la consola. `estado` muestra ese puesto. `salir` lo cierra.

## Inicializar una terminal en otra computadora

La primera terminal permanece abierta. En la otra máquina, con el mismo `config/pool-lab.ini`:

```bash
./bin/dsm_unir config/pool-lab.ini
```

El proceso ocupa el puerto 27200 de la segunda máquina, recorre las IP del archivo y se enlaza con la primera. En cualquiera de las dos, `estado` lista los dos puestos.

Cada terminal adicional usa el mismo comando, en la computadora que todavía tenga un puerto libre.

## Ejecutar Merge Sort

Las terminales ya abiertas permanecen en ejecución y solo prestan memoria. El ordenamiento se lanza **una vez**, en una terminal nueva de cualquiera de esas computadoras:

```bash
./bin/apps/mergesort config/pool-lab.ini 40000 aleatorio 2026
```

| Parte | Qué es |
| --- | --- |
| `./bin/apps/mergesort` | El programa. |
| `config/pool-lab.ini` | Las IP y el rango de puertos. No describe el experimento. |
| `40000` | Cantidad de enteros. |
| `aleatorio` | Cómo se generan. También admite `inverso`, `ordenado`, `negativos`, `repetidos` o `iguales`. |
| `2026` | Semilla. El mismo número produce los mismos datos. |

Ese proceso ocupa otro puerto libre, encuentra las terminales activas y les pide páginas. Ordena por sí solo. Al terminar imprime `VERIFICACION OK`.

Para que otra computadora también ordene una parte, se ejecuta el mismo comando en esa máquina, con pocos segundos de diferencia. Las dos copias realizan un solo ordenamiento, repartido.

## Ejecutar la multiplicación

El procedimiento es el mismo que en Merge Sort: una sola vez, con las terminales ya abiertas.

```bash
./bin/apps/matrices config/pool-lab.ini 32 32 32
```

Los tres números son las dimensiones de las matrices. El proceso que termina imprime `VERIFICACION OK`.
