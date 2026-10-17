#!/bin/sh
# Downloads NVIDIA's ibdiagnet (ibutils2), ibtopodiff and the matching infiniband-diags, libraries and ibsim from the
# public DOCA repository and unpacks them without root. Prints the directory to use as NVPREFIX and IBPREFIX.
#
# usage: NVPREFIX=$(tests/fetch-nvidia.sh [DIR])      default DIR: build/nvidia
# env:   DOCA_VERSION (default 3.4.1)
# The binaries are built for Ubuntu 24.04 x86_64 (glibc 2.38 or newer, libstdc++ 13 or newer); they run on newer
# distributions of that architecture as well. Needs curl, ar and tar, plus zstd or xz for the package compression.
set -eu
for tool in curl ar tar; do
    command -v "$tool" > /dev/null 2>&1 || { echo "fetch-nvidia.sh needs $tool" >&2; exit 1; }
done
DIR=${1:-$(dirname "$0")/../build/nvidia}
VERSION=${DOCA_VERSION:-3.4.1}
BASE=https://linux.mellanox.com/public/repo/doca/$VERSION/ubuntu24.04/x86_64/pool/
mkdir -p "$DIR/debs" "$DIR/root"
DIR=$(cd "$DIR" && pwd)

if [ ! -x "$DIR/root/usr/bin/ibdiagnet" ]; then
    curl -fsS --max-time 120 -o "$DIR/pool.html" "$BASE"
    for prefix in ibutils2 infiniband-diags libibumad3 libibmad5 libibnetdisc5 ibsim; do
        name=$(grep -oE "href=\"${prefix}_[^\"]*_amd64\.deb\"" "$DIR/pool.html" | head -1 | sed 's/href="//; s/"//')
        [ -n "$name" ] || { echo "package $prefix not found in $BASE" >&2; exit 1; }
        [ -s "$DIR/debs/$name" ] || curl -fsS --max-time 300 -o "$DIR/debs/$name" "$BASE$name"
        (cd "$DIR/debs" && rm -rf x && mkdir x && cd x && ar x "../$name"
         data=$(ls data.tar.* | head -1)
         case $data in
             *.zst) command -v zstd > /dev/null 2>&1 || { echo "fetch-nvidia.sh needs zstd" >&2; exit 1; } ;;
             *.xz) command -v xz > /dev/null 2>&1 || { echo "fetch-nvidia.sh needs xz" >&2; exit 1; } ;;
         esac
         tar -xf "$data" -C "$DIR/root")
    done
    rm -rf "$DIR/debs/x"
fi
echo "$DIR/root"
