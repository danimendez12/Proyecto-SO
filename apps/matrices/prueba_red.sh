#!/usr/bin/env bash
# Ejecutar desde la raiz del proyecto. Requiere util-linux e iproute2.
set -euo pipefail
if [[ ${1:-} != --aislado ]]; then
    exec unshare --user --map-root-user --net bash "$0" --aislado "$@"
fi
shift
n=${1:-128}
ini=${2:-config/matrices-lab.ini}
hilos=${3:-2}
trabajadores=${4:-2}
plazo=${5:-1800}
[[ $trabajadores == 1 || $trabajadores == 2 ]]
[[ $plazo =~ ^[1-9][0-9]*$ ]]
out=$(mktemp -d)
echo "Directorio de resultados: $out"

# El veth y las dos pilas TCP solo existen durante este ensayo.
unshare --net sleep infinity &
peer=$!
pids=()
limpiar() {
    for p in "${pids[@]}"; do kill "$p" 2>/dev/null || :; done
    kill "$peer" 2>/dev/null || :
    wait 2>/dev/null || :
}
trap limpiar EXIT
trap 'exit 1' INT TERM
for ((i=0; i<200; i++)); do
    peer_ns=$(readlink "/proc/$peer/ns/net") || exit 1
    [[ $peer_ns != "$(readlink /proc/self/ns/net)" ]] && break
    sleep .01
done
[[ $peer_ns != "$(readlink /proc/self/ns/net)" ]]
remoto=(nsenter --target "$peer" --net)
ip link add matriz-a type veth peer name matriz-b
ip link set matriz-b netns "$peer"
ip addr add 192.0.2.1/24 dev matriz-a
ip link set matriz-a up
ip link set lo up
"${remoto[@]}" ip addr add 192.0.2.2/24 dev matriz-b
"${remoto[@]}" ip link set matriz-b up
"${remoto[@]}" ip link set lo up
for ((i=0; i<=trabajadores; i++)); do
    awk '$1=="nodo" {$5=($3==1 ? "192.0.2.2" : "192.0.2.1")} {print}' "$ini" > "$out/nodo$i.ini"
done
app=./bin/apps/matrices
: > "$out/nodo0.log"
timeout -k 5 "$((plazo+90))" "$app" creador "$out/nodo0.ini" "$n" "$n" "$n" "$trabajadores" "$plazo" > "$out/nodo0.log" 2>&1 &
pids+=("$!")
id=''
until [[ -n $id ]]; do
    id=$(sed -n 's/^CONTROL id=\([0-9][0-9]*\) .*/\1/p' "$out/nodo0.log")
    if ! kill -0 "${pids[0]}" 2>/dev/null; then cat "$out/nodo0.log"; exit 1; fi
    [[ -n $id ]] || sleep .05
done
for ((i=1; i<=trabajadores; i++)); do
    ns=()
    if (( i == 1 )); then ns=("${remoto[@]}"); fi
    "${ns[@]}" timeout -k 5 "$((plazo+90))" "$app" trabajador "$out/nodo$i.ini" "$i" "$id" "$hilos" "$plazo" > "$out/nodo$i.log" 2>&1 &
    pids+=("$!")
done
for p in "${pids[@]}"; do wait "$p"; done
pids=()
grep -hE '^(MATRIZ |TRAMO |INICIALIZACION |APERTURA |CALCULO |CALCULO_FIN |VERIFICACION |LIBERACION |TRABAJADOR OK)' "$out"/nodo*.log
