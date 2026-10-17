#!/bin/sh
# Cross-checks ibnetlens against the real ibnetdiscover/iblinkinfo (infiniband-diags) on fabrics
# simulated by ibsim. Needs ibsim and infiniband-diags:
#   apt install ibsim-utils infiniband-diags      dnf install ibsim infiniband-diags
# Without root, unpack the .deb/.rpm files somewhere and point IBPREFIX at the unpacked tree.
#
# usage: xcheck-ibnetdiscover.sh PATH_TO_IBNETLENS PATH_TO_FATTREE
# env:   IBPREFIX=/path/to/unpacked/root   (optional)
#        XCHECK_LARGE=1                    also run fabrics of 16k to 61k nodes (about two minutes)
# ibsim cannot represent a node with more than 255 ports, so the large fabrics keep every switch below that.
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

find_tool() { # name dir...
    name=$1; shift
    for d in "$@"; do
        [ -x "$d/$name" ] && { echo "$d/$name"; return 0; }
    done
    command -v "$name" 2>/dev/null
}

P=${IBPREFIX:-}
DIRS="$P/usr/bin $P/usr/sbin $P/bin $P/sbin"
IBSIM=$(find_tool ibsim $DIRS)
DISCOVER=$(find_tool ibnetdiscover $DIRS)
LINKINFO=$(find_tool iblinkinfo $DIRS)
SO=
for d in $P/usr/lib/x86_64-linux-gnu $P/usr/lib64 $P/usr/lib $P/usr/lib/aarch64-linux-gnu; do
    [ -f "$d/umad2sim/libumad2sim.so" ] && { SO=$d/umad2sim/libumad2sim.so; LIBDIR=$d; break; }
done

if [ -z "$IBSIM" ] || [ -z "$DISCOVER" ] || [ -z "$LINKINFO" ] || [ -z "$SO" ]; then
    echo "skip real ibnetdiscover cross-check (ibsim or infiniband-diags not found)"
    exit 0
fi
[ -n "$P" ] && export LD_LIBRARY_PATH=$P/usr/lib:$P/usr/lib/x86_64-linux-gnu:$P/usr/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}

fail=0
ok()  { printf 'ok   %s\n' "$1"; }
bad() { printf 'FAIL %s\n' "$1"; fail=1; }

# start_sim NETFILE
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
simrun() { LD_PRELOAD=$SO "$@" 2>/dev/null; }

CONFIGS="4 2 3|16 4 8|32 8 16"
[ -n "${XCHECK_LARGE:-}" ] && CONFIGS="$CONFIGS|250 32 64|250 100 128|254 16 239"
OLDIFS=$IFS
IFS='|'
set -- $CONFIGS
IFS=$OLDIFS
for cfg in "$@"; do
    set -- $cfg
    "$GEN" "$1" "$2" "$3" > "$TMP/in.net"
    start_sim "$TMP/in.net"
    t0=$(date +%s)
    simrun "$DISCOVER" > "$TMP/dump.topo"
    secs=$(($(date +%s) - t0))
    links=$(simrun "$LINKINFO" | grep -c '==>')
    nsw=$(simrun "$DISCOVER" -S | grep -c '^Switch')
    nca=$(simrun "$DISCOVER" -H | grep -c '^Ca')
    stop_sim

    name="fat-tree $1x$2x$3"
    if [ ! -s "$TMP/dump.topo" ]; then bad "$name: real ibnetdiscover produced no output"; continue; fi
    ok "$name: real ibnetdiscover discovered $("$BIN" info -q "$TMP/dump.topo" | awk '/switches:|adapters:/ {n += $2} END {print n}') nodes in ${secs}s"

    "$BIN" parse --strict -F json "$TMP/dump.topo" > /dev/null 2>&1 && ok "$name: parses real ibnetdiscover output (strict)" \
        || bad "$name: parses real ibnetdiscover output (strict)"

    mine_sw=$("$BIN" info -q "$TMP/dump.topo" | awk '/switches:/ {print $2}')
    mine_ca=$("$BIN" info -q "$TMP/dump.topo" | awk '/adapters:/ {print $2}')
    mine_ln=$("$BIN" info -q "$TMP/dump.topo" | awk '/port entries:/ {print $3}')
    [ "$nsw" = "$mine_sw" ] && [ "$nca" = "$mine_ca" ] && ok "$name: switch and adapter counts match (real $nsw/$nca)" \
        || bad "$name: counts differ (real $nsw/$nca, ibnetlens $mine_sw/$mine_ca)"
    [ "$links" = "$mine_ln" ] && ok "$name: port count matches iblinkinfo ($links)" \
        || bad "$name: port count differs (iblinkinfo $links, ibnetlens $mine_ln)"

    "$BIN" -F csv "$TMP/in.net" | cut -d, -f1-6 | sort > "$TMP/links.in"
    "$BIN" -F csv "$TMP/dump.topo" | cut -d, -f1-6 | sort > "$TMP/links.out"
    if cmp -s "$TMP/links.in" "$TMP/links.out"; then
        ok "$name: discovered links equal the simulated fabric"
    else
        bad "$name: discovered links differ from the simulated fabric"
    fi

    "$BIN" check "$TMP/dump.topo" > /dev/null && ok "$name: check finds no problems in a healthy fabric" \
        || bad "$name: check reports problems in a healthy fabric"
done

# ibsim net files: node GUIDs must equal the ones ibsim itself hands out (default counters and absolute GUID bases)
"$GEN" 6 3 4 ibsim > "$TMP/plain.net"
awk '/^Switch/ {sw++; if (sw == 2) print "switchguid=0x500000"}
     /^(Ca|Hca)/ {ca++; if (ca == 6) print "hcaguids=0x900000"; if (ca == 12) print "caguid=0xa00000"}
     {print}' "$TMP/plain.net" > "$TMP/bases.net"
for net in plain bases; do
    start_sim "$TMP/$net.net"
    simrun "$DISCOVER" > "$TMP/sim-$net.topo"
    stop_sim
    "$BIN" -F csv-nodes -q "$TMP/$net.net" | tail -n +2 | awk -F, '{print $1","$2}' | sort > "$TMP/guids.ours"
    "$BIN" -F csv-nodes "$TMP/sim-$net.topo" | tail -n +2 | awk -F, '{print $1","$2}' | sort > "$TMP/guids.real"
    if [ -s "$TMP/guids.real" ] && cmp -s "$TMP/guids.ours" "$TMP/guids.real"; then
        ok "ibsim net file ($net): node GUIDs equal the ones ibsim assigns ($(wc -l < "$TMP/guids.real") nodes)"
    else
        bad "ibsim net file ($net): node GUIDs differ from ibsim's"
    fi
done

# diff against the real tool's cache diff: fabric A has 3 hosts per leaf, fabric B has 2
"$GEN" 4 2 3 > "$TMP/a.net"
"$GEN" 4 2 2 > "$TMP/b.net"
for v in a b; do
    start_sim "$TMP/$v.net"
    simrun "$DISCOVER" --cache "$TMP/$v.cache" > "$TMP/$v.topo"
    stop_sim
done
if [ -s "$TMP/a.cache" ] && [ -s "$TMP/b.cache" ]; then
    real=$("$DISCOVER" --load-cache "$TMP/a.cache" --diff "$TMP/b.cache" 2>/dev/null | grep -c .)
    "$BIN" diff "$TMP/a.topo" "$TMP/b.topo" > "$TMP/mine.diff"
    rc=$?
    removed=$(grep -c '^- node' "$TMP/mine.diff")
    named=$("$DISCOVER" --load-cache "$TMP/a.cache" --diff "$TMP/b.cache" 2>/dev/null | grep -cE 'node00(08|09|10|11) HCA')
    [ "$named" = 4 ] && [ $rc -eq 1 ] && ok "diff: the real tool names the 4 removed hosts and ibnetlens reports $removed removed nodes" \
        || bad "diff: real tool names $named of the 4 removed hosts, ibnetlens exit $rc"
    [ "$removed" = 4 ] && ok "diff: ibnetlens finds exactly the 4 removed hosts" || bad "diff: ibnetlens removed-node count is $removed, expected 4"
else
    echo "skip diff cross-check (ibnetdiscover --cache unavailable)"
fi

exit $fail
