#!/usr/bin/env bash
# Requiere Bash, coreutils, awk, grep, sed y ss; namespaces agrega util-linux e ip.
# Tres procesos reales; el modo namespaces crea dos pilas TCP sin tocar la red anfitriona.
set -euo pipefail

modo=${1:---loopback}
if [[ $modo == --aislado ]]; then
    shift
    modo=--namespaces
    aislado=1
else
    aislado=0
    [[ $modo == --loopback || $modo == --namespaces ]] || {
        echo "uso: bash $0 [--loopback|--namespaces] [N [INI [HILOS [PATRON [SEMILLA [ARCHIVO]]]]]]" >&2
        exit 2
    }
    (($# == 0)) || shift
fi
if [[ $modo == --namespaces && $aislado == 0 ]]; then
    if ((EUID == 0)); then
        exec unshare --net bash "$0" --aislado "$@"
    fi
    exec unshare --user --map-root-user --net bash "$0" --aislado "$@"
fi

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$repo"
n=${1:-65539}
ini=${2:-config/mergesort-lab.ini}
hilos=${3:-2}
patron=${4:-aleatorio}
semilla=${5:-12345}
archivo=${6:-}
trabajadores=${MERGESORT_TRABAJADORES:-2}
limite=${MERGESORT_PRUEBA_LIMITE:-240}
exigir_multihogar=${MERGESORT_EXIGIR_MULTIHOGAR:-1}
[[ $n =~ ^[1-9][0-9]*$ && $hilos =~ ^[1-9][0-9]*$ && $semilla =~ ^[0-9]+$ ]]
[[ $trabajadores == 1 || $trabajadores == 2 ]]
[[ $limite =~ ^[1-9][0-9]*$ && $exigir_multihogar =~ ^[01]$ ]]
[[ -r $ini && -x ./bin/apps/mergesort ]]
if [[ $patron == archivo ]]; then [[ -n $archivo && -r $archivo ]]; fi
out=$(mktemp -d)
printf 'RESULTADOS=%s\n' "$out"
printf 'ENSAYO modo=%s elementos=%s trabajadores=%s hilos=%s patron=%s semilla=%s\n' \
    "$modo" "$n" "$trabajadores" "$hilos" "$patron" "$semilla"

pids=()
peer=''
limpiar() {
    for p in "${pids[@]}"; do kill "$p" 2>/dev/null || :; done
    if [[ -n $peer ]]; then kill "$peer" 2>/dev/null || :; fi
    wait 2>/dev/null || :
}
trap limpiar EXIT
trap 'exit 1' INT TERM
remoto=()
if [[ $modo == --namespaces ]]; then
    unshare --net sleep infinity &
    peer=$!
    for ((i=0; i<200; i++)); do
        peer_ns=$(readlink "/proc/$peer/ns/net") || exit 1
        [[ $peer_ns != "$(readlink /proc/self/ns/net)" ]] && break
        sleep .01
    done
    [[ $peer_ns != "$(readlink /proc/self/ns/net)" ]]
    remoto=(nsenter --target "$peer" --net)
    ip link add merge-a type veth peer name merge-b
    ip link set merge-b netns "$peer"
    ip addr add 192.0.2.1/24 dev merge-a
    ip link set merge-a up
    ip link set lo up
    "${remoto[@]}" ip addr add 192.0.2.2/24 dev merge-b
    "${remoto[@]}" ip link set merge-b up
    "${remoto[@]}" ip link set lo up
    awk '$1=="nodo" {$5=($3==1 ? "192.0.2.2" : "192.0.2.1")} {print}' "$ini" >"$out/comun.ini"
    printf 'RED nodos0y2=192.0.2.1 nodo1=192.0.2.2 (dos namespaces y veth aislados)\n'
else
    cat -- "$ini" >"$out/comun.ini"
fi

# Evitar dos contextos con el mismo ID/puerto. No se detienen servicios ajenos.
for puerto in $(awk '$1=="nodo" && $3<=2 {print $6}' "$out/comun.ini"); do
    [[ $puerto =~ ^[0-9]+$ ]]
    if [[ -n $(ss -ltnH "sport = :$puerto") ]]; then
        printf 'Puerto ocupado en namespace local: %s\n' "$puerto" >&2
        exit 1
    fi
done
app=./bin/apps/mergesort
args=(crear "$out/comun.ini" "$n" "$trabajadores" "$hilos" "$patron" "$semilla")
if [[ $patron == archivo ]]; then args+=("$archivo"); fi
: >"$out/nodo0.log"
timeout -k 5s "${limite}s" "$app" "${args[@]}" >"$out/nodo0.log" 2>&1 &
pids+=("$!")
id=''
fin=$((SECONDS+limite))
until [[ -n $id ]]; do
    id=$(sed -n 's/^CONTROL id=\([0-9][0-9]*\).*/\1/p' "$out/nodo0.log" | head -1)
    if ! kill -0 "${pids[0]}" 2>/dev/null || ((SECONDS >= fin)); then
        cat "$out/nodo0.log"
        exit 1
    fi
    [[ -n $id ]] || sleep .02
done
for ((i=1; i<=trabajadores; i++)); do
    ns=()
    if ((i == 1)) && [[ $modo == --namespaces ]]; then ns=("${remoto[@]}"); fi
    "${ns[@]}" timeout -k 5s "${limite}s" "$app" trabajar "$out/comun.ini" "$i" "$id" \
        >"$out/nodo$i.log" 2>&1 &
    pids+=("$!")
done

fallo=0
for i in "${!pids[@]}"; do
    if wait "${pids[i]}"; then rc=0; else rc=$?; fallo=1; fi
    printf 'PROCESO nodo=%s exit=%s\n' "$i" "$rc"
    unset 'pids[i]'
done
if ((fallo)); then
    tail -100 "$out"/nodo*.log
    exit 1
fi
grep -q '^VERIFICACION OK' "$out/nodo0.log"
grep -q '^LIBERACION OK' "$out/nodo0.log"
for ((i=1; i<=trabajadores; i++)); do grep -q '^TRABAJADOR OK' "$out/nodo$i.log"; done
for nombre in A B; do
    reserva=$(sed -n "s/^RESERVA $nombre id=\([0-9][0-9]*\).*/\1/p" "$out/nodo0.log")
    [[ -n $reserva ]]
    for hogar in $(awk -v nombre="$nombre" '$1=="TRAMO" && $2==nombre {for(i=3;i<=NF;i++) if($i~/^hogar=/) {split($i,a,"=");print a[2]}}' "$out/nodo0.log" | sort -nu); do
        if [[ $hogar != 0 ]]; then
            grep -Eq "CAP_RELEASE_ACK hogar $hogar reserva $reserva .*resultado 0([[:space:]]|$)" "$out/nodo0.log"
            printf 'CAPACIDAD reserva=%s hogar=%s liberada=si\n' "$reserva" "$hogar"
        fi
    done
done
if [[ $exigir_multihogar == 1 ]]; then
    grep -Eq '^TRAMO A .*hogar=0([[:space:]]|$)|^TRAMO A hogar=0([[:space:]]|$)' "$out/nodo0.log"
    grep -Eq '^TRAMO A .*hogar=1([[:space:]]|$)|^TRAMO A hogar=1([[:space:]]|$)' "$out/nodo0.log"
fi
grep -hE '^(CONTROL |RESERVA |TRAMO |HOGAR |ORDEN|FASE |FUSION|VERIFICACION |LIBERACION |TRABAJADOR OK|TIEMPO|ESTADISTICAS|APERTURA |TRABAJO |INICIALIZACION |CALCULO |ENTRADA )' \
    "$out"/nodo*.log
printf 'INTEGRACION OK modo=%s elementos=%s procesos=%s\n' "$modo" "$n" "$((trabajadores+1))"
