#!/bin/sh
# Cross-checks ibnetlens against NVIDIA's ibdiagnet and ibtopodiff (package ibutils2, part of DOCA-Host and
# MLNX_OFED) on fabrics simulated by ibsim.
#
# The packages come from NVIDIA's public DOCA repository. Without root, download ibutils2, infiniband-diags,
# libibumad3, libibmad5, libibnetdisc5 and ibsim from
#   https://linux.mellanox.com/public/repo/doca/<version>/ubuntu24.04/x86_64/pool/
# unpack them with `dpkg -x`, and point NVPREFIX at the unpacked tree.
# NVIDIA UFM is a licensed product and is not covered here.
#
# usage: xcheck-nvidia.sh PATH_TO_IBNETLENS PATH_TO_FATTREE
# env:   NVPREFIX=/path/to/unpacked/root   XCHECK_LARGE=1 adds a 16k-node fabric
# Prints "ok", "FAIL" or "skip" lines like tests/run.sh.
set -u
BIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
GEN=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
TMP=$(mktemp -d)
SIM_PID=
cleanup() {
    [ -n "$SIM_PID" ] && kill "$SIM_PID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

P=${NVPREFIX:-}
find_tool() {
    name=$1
    for d in "$P/usr/bin" "$P/usr/sbin" "$P/bin" "$P/sbin"; do
        [ -x "$d/$name" ] && { echo "$d/$name"; return 0; }
    done
    command -v "$name" 2>/dev/null
}
IBDIAGNET=$(find_tool ibdiagnet)
TOPODIFF=$(find_tool ibtopodiff)
IBSIM=$(find_tool ibsim)
SO=
for d in "$P/usr/lib/umad2sim" "$P/usr/lib/x86_64-linux-gnu/umad2sim" "$P/usr/lib64/umad2sim"; do
    [ -f "$d/libumad2sim.so" ] && { SO=$d/libumad2sim.so; break; }
done
if [ -z "$IBDIAGNET" ] || [ -z "$TOPODIFF" ] || [ -z "$IBSIM" ] || [ -z "$SO" ]; then
    echo "skip NVIDIA ibdiagnet/ibtopodiff cross-check (ibutils2 or ibsim not found, see the header of this script)"
    exit 0
fi
if [ -n "$P" ]; then
    LD_LIBRARY_PATH=$P/usr/lib:$P/usr/lib/x86_64-linux-gnu:$P/usr/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
    export LD_LIBRARY_PATH
fi

fail=0
ok()  { printf 'ok   %s\n' "$1"; }
bad() { printf 'FAIL %s\n' "$1"; fail=1; }

start_sim() {
    nodes=$("$BIN" info -q "$1" | awk '/switches:|adapters:|routers:/ {n += $2} END {print n}')
    sw=$("$BIN" info -q "$1" | awk '/switches:/ {print $2}')
    ports=$("$BIN" info -q "$1" | awk '/port entries:/ {print $3}')
    "$IBSIM" -s -N $((nodes + 16)) -S $((sw + 16)) -P $((ports + 1024)) "$1" > "$TMP/sim.log" 2>&1 &
    SIM_PID=$!
    sleep 1.5
}
stop_sim() {
    kill "$SIM_PID" 2>/dev/null
    wait "$SIM_PID" 2>/dev/null
    SIM_PID=
}

CONFIGS="4 2 3|16 4 8|32 8 16"
[ -n "${XCHECK_LARGE:-}" ] && CONFIGS="$CONFIGS|250 32 64"
OLDIFS=$IFS
IFS='|'
set -- $CONFIGS
IFS=$OLDIFS
for cfg in "$@"; do
    set -- $cfg
    name="fat-tree $1x$2x$3"
    "$GEN" "$1" "$2" "$3" > "$TMP/in.net"
    start_sim "$TMP/in.net"
    rm -rf "$TMP/diag"
    mkdir "$TMP/diag"
    LD_PRELOAD=$SO timeout 1200 "$IBDIAGNET" -o "$TMP/diag" --skip all > "$TMP/ibdiagnet.out" 2>&1
    stop_sim

    topo=$TMP/diag/ibdiagnet2.ibnetdiscover
    if [ ! -s "$topo" ]; then bad "$name: ibdiagnet produced no topology file"; continue; fi

    real_sw=$(awk -F: '/IB Switches/ {gsub(/ /, "", $2); print $2}' "$TMP/ibdiagnet.out")
    real_ca=$(awk -F: '/IB Channel Adapters/ {gsub(/ /, "", $2); print $2}' "$TMP/ibdiagnet.out")
    ok "$name: ibdiagnet discovered $real_sw switches and $real_ca adapters"

    "$BIN" parse --strict -F json "$topo" > /dev/null 2>&1 && ok "$name: parses ibdiagnet's own topology file (strict)" \
        || bad "$name: parses ibdiagnet's own topology file (strict)"
    mine_sw=$("$BIN" info -q "$topo" | awk '/switches:/ {print $2}')
    mine_ca=$("$BIN" info -q "$topo" | awk '/adapters:/ {print $2}')
    [ "$real_sw" = "$mine_sw" ] && [ "$real_ca" = "$mine_ca" ] && ok "$name: node counts match ibdiagnet ($mine_sw/$mine_ca)" \
        || bad "$name: node counts differ (ibdiagnet $real_sw/$real_ca, ibnetlens $mine_sw/$mine_ca)"

    "$BIN" -F csv "$TMP/in.net" | cut -d, -f1-6 | sort > "$TMP/links.in"
    "$BIN" -F csv "$topo" | cut -d, -f1-6 | sort > "$TMP/links.diag"
    cmp -s "$TMP/links.in" "$TMP/links.diag" && ok "$name: links in ibdiagnet's file equal the simulated fabric" \
        || bad "$name: links in ibdiagnet's file differ from the simulated fabric"
    "$BIN" check "$topo" > /dev/null && ok "$name: check is clean on ibdiagnet's file" || bad "$name: check is not clean on ibdiagnet's file"
done

# A fabric with one link at a lower speed: ibdiagnet records the speeds it discovers, ibnetlens check reads them.
# (ibdiagnet's own link checks are not comparable on ibsim: the simulator leaves every link in the INI state
# and does not answer vendor MADs, so ibdiagnet reports all links.)
"$GEN" 4 2 3 degrade > "$TMP/deg.net"
start_sim "$TMP/deg.net"
rm -rf "$TMP/diag"
mkdir "$TMP/diag"
LD_PRELOAD=$SO timeout 300 "$IBDIAGNET" -o "$TMP/diag" --skip all > /dev/null 2>&1
stop_sim
found=$("$BIN" check "$TMP/diag/ibdiagnet2.ibnetdiscover" 2> /dev/null | grep -c 'degraded-link\|speed-mismatch')
[ "$found" -ge 1 ] && ok "degraded link: check finds it in the topology file written by ibdiagnet" \
    || bad "degraded link: check finds nothing in the topology file written by ibdiagnet"

# ibtopodiff against ibnetlens diff and ibnetlens's own ibnd writer
"$GEN" 4 2 3 > "$TMP/a.ibnd"
"$GEN" 4 2 2 > "$TMP/b.ibnd"
cd "$TMP" || exit 2
"$TOPODIFF" -t a.ibnd -d b.ibnd -e > topodiff.out 2>&1
nv_missing=$(sed -n 's/.*Total: \([0-9]*\) Missing nodes.*/\1/p' topodiff.out)
"$BIN" diff a.ibnd b.ibnd > mine.diff
rc=$?
mine_missing=$(grep -c '^- node' mine.diff)
[ -n "$nv_missing" ] && [ "$nv_missing" = "$mine_missing" ] && [ $rc -eq 1 ] \
    && ok "diff: ibtopodiff and ibnetlens diff both find $mine_missing removed hosts" \
    || bad "diff: ibtopodiff reports '${nv_missing:-nothing}' missing nodes, ibnetlens diff $mine_missing (exit $rc)"

"$TOPODIFF" -t a.ibnd -d a.ibnd -e > same.out 2>&1
grep -q 'Fabric Topologies Match' same.out && "$BIN" diff a.ibnd a.ibnd > /dev/null \
    && ok "diff: both tools see identical files as equal" || bad "diff: both tools see identical files as equal"

"$BIN" -F ibnd a.ibnd > rewritten.ibnd
"$TOPODIFF" -t a.ibnd -d rewritten.ibnd -e > rewrite.out 2>&1
grep -q 'Fabric Topologies Match' rewrite.out \
    && ok "ibnd writer: ibtopodiff reads ibnetlens's output and finds it identical to the original" \
    || bad "ibnd writer: ibtopodiff sees a difference in ibnetlens's output"

exit $fail
