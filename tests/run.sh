#!/bin/sh
# Golden, round-trip and exit-code tests. usage: run.sh PATH_TO_IBNETLENS PATH_TO_FATTREE
set -u
BIN=$1
GEN=$2
G=$(dirname "$0")/golden
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0

ok()  { printf 'ok   %s\n' "$1"; }
bad() { printf 'FAIL %s\n' "$1"; fail=1; }

same() { # name expected actual
    if cmp -s "$2" "$3"; then ok "$1"; else bad "$1"; diff "$2" "$3" | head -5; fi
}

for t in ft-small ft-medium variety; do
    "$BIN" -F legacy "$G/$t.topo" > "$TMP/$t.legacy"
    same "$t legacy" "$G/$t.legacy" "$TMP/$t.legacy"

    "$BIN" -F ibnd "$G/$t.topo" > "$TMP/$t.ibnd"
    "$BIN" -F json "$G/$t.topo" > "$TMP/$t.json"
    "$BIN" -F json "$TMP/$t.ibnd" > "$TMP/$t.json2"
    same "$t ibnd round trip" "$TMP/$t.json" "$TMP/$t.json2"
done

for t in ft-small ft-medium variety broken; do
    "$BIN" -j 1 -F json "$G/$t.topo" > "$TMP/$t.j1" 2>&1
    "$BIN" -j 5 -F json "$G/$t.topo" > "$TMP/$t.j5" 2>&1
    same "$t threaded parse" "$TMP/$t.j1" "$TMP/$t.j5"
done

for net in "$G/ibsim.net" "$G/mixed-spine.net"; do
    "$BIN" -j 1 -F json "$net" > "$TMP/n.j1" 2>&1
    "$BIN" -j 4 -F json "$net" > "$TMP/n.j4" 2>&1
    same "$(basename "$net") threaded parse" "$TMP/n.j1" "$TMP/n.j4"
done

for mode in ibsim ibsim-messy; do
    "$GEN" 64 8 16 "$mode" > "$TMP/big.net"
    "$BIN" -j 1 -F json "$TMP/big.net" > "$TMP/b.j1" 2> "$TMP/b.e1"
    "$BIN" -j 7 -F json "$TMP/big.net" > "$TMP/b.j7" 2> "$TMP/b.e7"
    same "$mode 64x8x16 threaded output" "$TMP/b.j1" "$TMP/b.j7"
    same "$mode 64x8x16 threaded diagnostics" "$TMP/b.e1" "$TMP/b.e7"
    "$BIN" -j 7 info -q --stats "$TMP/big.net" 2>&1 > /dev/null | grep -q 'threads=7' \
        && ok "$mode really used 7 threads" || bad "$mode really used 7 threads"
done
"$BIN" -j 1 -F json "$TMP/big.net" > /dev/null 2>&1
grep -q "defined twice" "$TMP/b.e1" && ok "duplicate node ID reported" || bad "duplicate node ID reported"

# a dump followed by named nodes: threads must fall back to the single-thread result
cat "$G/ft-small.topo" "$G/ibsim.net" > "$TMP/mixed.topo"
"$BIN" -j 1 -F json "$TMP/mixed.topo" > "$TMP/x.j1" 2>&1
"$BIN" -j 5 -F json "$TMP/mixed.topo" > "$TMP/x.j5" 2>&1
same "mixed dump and ibsim net, threaded" "$TMP/x.j1" "$TMP/x.j5"

"$BIN" -j 4 check "$G/broken.topo" > "$TMP/broken.check4"
same "broken threaded check" "$G/broken.check" "$TMP/broken.check4"

"$GEN" 64 8 16 > "$TMP/mid.topo"
printf 'vendid=0x2c9\nvendid=\n' >> "$TMP/mid.topo"
"$BIN" -j 1 --strict check "$TMP/mid.topo" > "$TMP/s1" 2>&1; r1=$?
"$BIN" -j 6 --strict check "$TMP/mid.topo" > "$TMP/s6" 2>&1; r6=$?
[ $r1 -eq 2 ] && [ $r6 -eq 2 ] && ok "strict stops in threaded mode" || bad "strict stops in threaded mode"
same "strict diagnostics" "$TMP/s1" "$TMP/s6"

for t in ft-medium variety; do
    "$BIN" -F slurm "$G/$t.topo" > "$TMP/$t.slurm"
    same "$t slurm" "$G/$t.slurm" "$TMP/$t.slurm"
done

"$BIN" -F slurm "$G/mixed-spine.net" > "$TMP/mixed.slurm"
same "spine with hosts slurm" "$G/mixed-spine.slurm" "$TMP/mixed.slurm"

for t in broken variety; do
    "$BIN" check "$G/$t.topo" > "$TMP/$t.check"
    same "$t check" "$G/$t.check" "$TMP/$t.check"
done

for ext in check slurm json; do
    case $ext in
        check) "$BIN" check "$G/ibsim.net" > "$TMP/ibsim.$ext" ;;
        slurm) "$BIN" -F slurm "$G/ibsim.net" > "$TMP/ibsim.$ext" ;;
        json)  "$BIN" -F json "$G/ibsim.net" > "$TMP/ibsim.$ext" ;;
    esac
    same "ibsim dialect $ext" "$G/ibsim.$ext" "$TMP/ibsim.$ext"
done

# the extended layout written by NVIDIA's ibdiagnet (port labels in parentheses) is the same fabric as ft-medium
"$BIN" parse --strict -F csv "$G/ibdiagnet-ext.topo" 2> "$TMP/ext.err" | cut -d, -f1-6 | sort > "$TMP/ext.links"
"$BIN" -F csv "$G/ft-medium.topo" | cut -d, -f1-6 | sort > "$TMP/med.links"
[ ! -s "$TMP/ext.err" ] && same "ibdiagnet extended format: links equal ft-medium" "$TMP/med.links" "$TMP/ext.links" \
    || bad "ibdiagnet extended format parses strictly"

"$BIN" diff "$G/variety.topo" "$G/broken.topo" > "$TMP/d.diff"
rc=$?
same "diff output" "$G/variety-broken.diff" "$TMP/d.diff"
[ $rc -eq 1 ] && ok "diff exit status" || bad "diff exit status"

"$BIN" diff "$G/variety.topo" "$G/variety.topo" > /dev/null
[ $? -eq 0 ] && ok "diff identical" || bad "diff identical"

"$GEN" 16 8 8 > "$TMP/clean.topo"
"$BIN" check "$TMP/clean.topo" > /dev/null
[ $? -eq 0 ] && ok "check clean fabric" || bad "check clean fabric"

"$GEN" 16 8 8 degrade > "$TMP/deg.topo"
"$BIN" check "$TMP/deg.topo" > /dev/null
[ $? -eq 1 ] && ok "check degraded fabric" || bad "check degraded fabric"

"$BIN" check - < "$TMP/clean.topo" > /dev/null
[ $? -eq 0 ] && ok "stdin input" || bad "stdin input"

"$BIN" --strict -F json "$G/broken.topo" > /dev/null 2>&1
[ $? -eq 0 ] && ok "strict accepts well-formed input" || bad "strict accepts well-formed input"

"$BIN" info "$G/ft-medium.topo" > /dev/null 2> "$TMP/err.default"
[ ! -s "$TMP/err.default" ] && ok "no progress output when stderr is not a terminal" || bad "no progress output when stderr is not a terminal"

"$BIN" info --no-progress "$G/ft-medium.topo" > /dev/null 2> "$TMP/err.off"
[ ! -s "$TMP/err.off" ] && ok "--no-progress is silent" || bad "--no-progress is silent"

"$BIN" info --progress "$G/ft-medium.topo" > /dev/null 2> "$TMP/err.on"
grep -q 'parsing 100%' "$TMP/err.on" && ok "--progress reports completion" || bad "--progress reports completion"

"$BIN" info --stats "$G/ft-medium.topo" > /dev/null 2> "$TMP/err.stats"
grep -q 'stats parse_s=.* peak_kb=[0-9]* threads=[0-9]* ' "$TMP/err.stats" && ok "--stats line" || bad "--stats line"

"$BIN" info --no-progress --progress "$G/ft-medium.topo" > /dev/null 2> "$TMP/err.last"
grep -q 'parsing 100%' "$TMP/err.last" && ok "last progress option wins" || bad "last progress option wins"

printf 'garbage\n' > "$TMP/garbage.topo"
"$BIN" --strict "$TMP/garbage.topo" > /dev/null 2>&1
[ $? -eq 2 ] && ok "strict rejects malformed input" || bad "strict rejects malformed input"

"$BIN" "$TMP/does-not-exist" > /dev/null 2>&1
[ $? -eq 2 ] && ok "missing file" || bad "missing file"

"$BIN" -F nonsense "$G/variety.topo" > /dev/null 2>&1
[ $? -eq 2 ] && ok "unknown format" || bad "unknown format"

exit $fail
