#!/bin/sh
# Compares ibnetlens against the plain Python reference parser on a generated fat-tree.
# usage: bench.sh IBNETLENS FATTREE [LEAVES SPINES HOSTS]
BIN=$1
GEN=$2
L=${3:-1024}
S=${4:-32}
H=${5:-32}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
DIR=$(dirname "$0")/../bench

"$GEN" "$L" "$S" "$H" > "$TMP/fabric.topo"
printf 'input: %s MB, %s leaves, %s spines, %s hosts/leaf\n' \
    "$(( $(wc -c < "$TMP/fabric.topo") / 1048576 ))" "$L" "$S" "$H"

timeit() { # label cmd...
    label=$1; shift
    start=$(date +%s.%N)
    "$@" > /dev/null
    end=$(date +%s.%N)
    printf '%-28s %6.2f s\n' "$label" "$(echo "$end - $start" | bc)"
}

timeit "ibnetlens parse (json)" "$BIN" -F json "$TMP/fabric.topo"
timeit "ibnetlens check" "$BIN" check "$TMP/fabric.topo"
timeit "ibnetlens slurm" "$BIN" -F slurm "$TMP/fabric.topo"
if command -v python3 > /dev/null; then
    timeit "python reference parse" python3 "$DIR/ref_parse.py" "$TMP/fabric.topo"
fi
