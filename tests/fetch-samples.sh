#!/bin/sh
# Downloads public ibnetdiscover dumps into build/samples and runs ibnetlens on them.
# The files are not redistributed with this repository because their licenses differ.
# usage: fetch-samples.sh PATH_TO_IBNETLENS
set -eu
BIN=$1
DIR=$(dirname "$0")/../build/samples
mkdir -p "$DIR"

fetch() { # name url
    [ -s "$DIR/$1" ] || curl -fsS --max-time 60 -o "$DIR/$1" "$2"
}

fetch loginoff-sx6036.topo https://raw.githubusercontent.com/loginoff/infiniband_topology/master/topo
fetch fattree-k4n3.topo https://raw.githubusercontent.com/cyberang3l/InfiniBand-Graphviz-ualization/master/k-4-n-3-Full.topo
fetch ibsim-net.2sw2path4hca.topo https://raw.githubusercontent.com/linux-rdma/ibsim/master/net-examples/net.2sw2path4hca

status=0
for f in "$DIR"/*.topo; do
    if "$BIN" check --strict "$f" > /dev/null &&
       "$BIN" -F ibnd "$f" | "$BIN" parse --strict -F json - > /dev/null; then
        printf 'ok   %s\n' "$(basename "$f")"
    else
        printf 'FAIL %s\n' "$(basename "$f")"
        status=1
    fi
done
exit $status
