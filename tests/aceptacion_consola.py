#!/usr/bin/env python3
"""Aceptación por terminales reales. Ejecutar desde la raíz del proyecto en Linux."""
import argparse
import contextlib
import re
import subprocess
import tempfile
import time
from pathlib import Path


class Consola:
    def __init__(self, ruta, ini, prefijo=()):
        self.log = ruta.open("wb")
        self.lector = ruta.open("rb")
        self.p = subprocess.Popen([*prefijo, "./bin/dsm_unir", str(ini)],
                                  stdin=subprocess.PIPE, stdout=self.log,
                                  stderr=subprocess.STDOUT)
        self.nodo = None
        self.pendiente = bytearray()

    def esperar(self, patron, segundos=60):
        limite = time.monotonic() + segundos
        expresion = re.compile(patron)
        desde = 0
        while time.monotonic() < limite:
            coincidencia = expresion.search(self.pendiente, desde)
            if coincidencia:
                fin = coincidencia.end()
                datos = self.pendiente[:fin]
                del self.pendiente[:fin]
                return datos.decode(errors="replace")
            # Los marcadores de este test ocupan menos de 100 bytes.
            desde = max(0, len(self.pendiente) - 100)
            fragmento = self.lector.read(65536)
            if fragmento:
                self.pendiente.extend(fragmento)
                continue
            if self.p.poll() is not None:
                break
            time.sleep(0.015)
        raise RuntimeError(f"PID {self.p.pid}, rc {self.p.poll()}, esperaba {patron!r}: "
                           + self.pendiente[-1600:].decode(errors="replace"))

    def iniciar(self, id_esperado):
        cabecera = self.esperar(rb"DSM\[\d+\]>")
        assert re.search(rf"ALTA OK pid=\d+ nodo={id_esperado} puerto=\d+", cabecera), cabecera
        self.nodo = id_esperado

    def enviar(self, texto):
        self.p.stdin.write((texto + "\n").encode())
        self.p.stdin.flush()

    def comando(self, texto, esperado=0):
        self.enviar(texto)
        salida = self.esperar(rb"COMANDO FIN rc=-?\d+\r?\n", 120)
        rc = int(re.search(r"COMANDO FIN rc=(-?\d+)", salida)[1])
        assert (rc < 0 if esperado is None else rc == esperado), (texto, rc, salida[-800:])
        return salida

    def cerrar(self):
        self.p.stdin.write(b"salir\n")
        self.p.stdin.flush()
        salida = self.esperar(rb"CIERRE OK nodo=\d+")
        assert f"CIERRE OK nodo={self.nodo}" in salida and self.p.wait(15) == 0

    def recoger(self):
        if self.p.poll() is None:
            self.p.kill()
        self.p.wait()
        self.p.stdin.close()
        self.lector.close()
        self.log.close()


def memoria(consola, activos, ocupadas):
    limite = time.monotonic() + 25
    while time.monotonic() < limite:
        salida = consola.comando("memoria")
        fila = next((l[l.index("MEMORIA "):] for l in salida.splitlines() if "MEMORIA " in l), "")
        campos = dict(re.findall(r"(\w+)=([^\s]+)", fila))
        if (campos.get("respondieron"), campos.get("paginas_ocupadas")) == (str(activos), str(ocupadas)):
            total = 2048 * activos
            assert int(campos["activa_bytes"]) == total * 4096
            assert int(campos["ocupada_bytes"]) == ocupadas * 4096
            assert int(campos["libre_bytes"]) == (total - ocupadas) * 4096
            assert int(campos["paginas_libres"]) == total - ocupadas
            print(f"observador={consola.nodo} {fila}", flush=True)
            return campos
        time.sleep(0.1)
    raise AssertionError(("memoria", activos, ocupadas, fila))


def reservar(c, tam):
    salida = c.comando("reservar " + tam)
    m = re.search(r"RESERVA OK id=(\d+) bytes=(\d+) paginas=(\d+)", salida)
    assert m, salida
    return tuple(map(int, m.groups()))


def leer(c, rid, offset, texto):
    s = c.comando(f"leer {rid} {offset} {len(texto.encode())}")
    assert f'texto="{texto}"' in s, s[-800:]


def pagina(c, hogar, gpn):
    salida = c.comando(f"paginas nodo {hogar} {gpn} 1")
    linea = next(l for l in salida.splitlines() if f"PAGINA gpn={gpn} " in l)
    return dict(re.findall(r"(\w+)=([^\s]+)", linea))


def escenario(directorio, ini, ids, prefijos):
    with contextlib.ExitStack() as recursos:
        consolas = []
        for i, (nodo, prefijo) in enumerate(zip(ids, prefijos)):
            c = Consola(directorio / f"terminal-{i}.log", ini, prefijo)
            recursos.callback(c.recoger)
            c.iniciar(nodo)
            consolas.append(c)
        a, b, c = consolas
        for terminal in consolas:
            memoria(terminal, 3, 0)
        for linea in ["desconocido", "reservar 0", "reservar -2K", "reservar 18446744073709551616M",
                      "abrir 0", "leer 0 0 1", "reservar 1G"]:
            a.comando(linea, None)
        inexistente = (ids[1] << 16) | 65000
        a.comando(f"abrir {inexistente}", None)
        assert "RESERVAS FIN cantidad=0" in a.comando("reservas")
        # Supera la capacidad activa aunque la nominal del montaje virtual sea mayor.
        a.comando("reservar 25M", None)
        principal, largo, paginas = reservar(a, "15M")
        assert (largo, paginas) == (15 * 1048576, 3840)
        mapa = a.comando(f"mapa {principal}")
        tramos = [tuple(map(int, m)) for m in re.findall(
            rf"MAPA TRAMO id={principal} orden=(\d+) paginas=(\d+) hogar=(\d+) gpn=(\d+)", mapa)]
        assert [(o, n, h) for o, n, h, g in tramos] == [(0, 2048, ids[0]), (2048, 1792, ids[1])], tramos
        print(f"reserva={principal} bytes={largo} tramos={tramos}", flush=True)
        propietario = (ids[0] << 16) | 1
        hogar, gpn = tramos[1][2:]
        antes = pagina(c, hogar, gpn)
        assert antes["asignacion"] == "ASIGNADA" and antes["procedencia"] == "PUBLICA"
        assert int(antes["propietario"]) == propietario and int(antes["orden"]) == 2048
        for t in consolas:
            memoria(t, 3, 3840)
            assert "NODO id=" in t.comando("nodos")
            for query in ["local", "estado", "estadisticas", f"estadisticas nodo {ids[1]}",
                          "paginas local 0 3", "paginas global 0 5", "copias local 0 5"]:
                t.comando(query)
        adicional, tam, n = reservar(a, "64K")
        assert (tam, n) == (65536, 16)
        a.comando(f"llenar {adicional} 42")
        a.comando(f"verificar {adicional} 42")
        a.comando(f"verificar {adicional} 41", None)
        a.comando(f"liberar {adicional}")
        assert "hex=0000000000000000" in a.comando(f"leer {principal} 0 8")
        a.comando(f"escribir {principal} 0 Hola TEC")
        apertura = b.comando(f"abrir {principal}")
        assert f"bytes={largo}" in apertura
        b.comando(f"abrir {principal}")
        leer(b, principal, 0, "Hola TEC")
        b.comando(f"escribir {principal} 0 AdiosTEC")
        leer(a, principal, 0, "AdiosTEC")
        for offset, texto in [(4092, "DosPag01"), (2048 * 4096 - 4, "DosHog02")]:
            b.comando(f"escribir {principal} {offset} {texto}")
            leer(a, principal, offset, texto)
        despues = pagina(c, hogar, gpn)
        assert int(despues["propietario"]) == propietario and int(despues["orden"]) == 2048
        assert int(despues["version"]) >= int(antes["version"])
        print("hogar antes", antes, "despues", despues, flush=True)
        a.comando(f"leer {principal} {largo} 1", None)
        a.comando(f"leer {principal} 0 257", None)
        a.enviar(f"tarea patron {principal} 4 37")
        a.esperar(rb"TAREA INICIO[^\n]*\n")
        memoria(c, 3, 3840)
        assert "ESTADISTICAS global=SI" in c.comando("estadisticas")
        tarea = a.esperar(rb"COMANDO FIN rc=-?\d+\r?\n", 120)
        assert "TAREA OK" in tarea and "COMANDO FIN rc=0" in tarea, tarea[-1200:]
        a.comando(f"liberar {principal}", None)
        a.comando("salir", None)
        assert str(principal) in a.comando("reservas")
        b.comando(f"liberar {principal}", None)
        b.comando(f"desvincular {principal}")
        a.comando(f"liberar {principal}")
        for terminal in consolas:
            memoria(terminal, 3, 0)
        for tam, hilos in [("1", 1), ("4097", 2), ("12303", 3)]:
            rid, total, _ = reservar(a, tam)
            assert f"bytes={total}" in a.comando(f"tarea patron {rid} {hilos} 255")
            a.comando(f"liberar {rid}")
        c.cerrar()
        memoria(a, 2, 0)
        b.cerrar()
        a.cerrar()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--virtual", nargs=2, type=int, metavar=("PID_B", "PID_C"))
    parser.add_argument("--logs", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="dsm-consola-") as tmp:
        salida = args.logs or Path(tmp)
        salida.mkdir(exist_ok=True, parents=True)
        ini = salida / "consola.ini"
        if args.virtual:
            plantilla = Path("config/pool-lab.ini").read_text()
            equipos = iter(range(1, 21))
            plantilla, cambios = re.subn(r"(?m)^equipo\s*=.*$", lambda _: f"equipo = 192.0.2.{next(equipos)}", plantilla)
            assert cambios == 20
            ini.write_text(plantilla)
            ids = [0, 3, 6]
            prefijos = [[], *[["nsenter", "-t", str(pid), "-n", "--"] for pid in args.virtual]]
        else:
            ini.write_text("page_size=4096\nmemoria_inicial=8388608\nmax_nodos=3\nmax_hilos=8\n"
                           "heartbeat_intervalo_ms=100\nheartbeat_fallos=5\nrpc_timeout_ms=1500\n"
                           "equipo=127.0.0.1 3 27280\n")
            ids = [0, 1, 2]
            prefijos = [[], [], []]
        escenario(salida, ini, ids, prefijos)
        print("CONSOLA 14D OK", flush=True)


if __name__ == "__main__":
    main()
