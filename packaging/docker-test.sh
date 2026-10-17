#!/bin/sh
# Builds and tests the native package in clean containers.
#
# usage: [PLATFORM=linux/arm64] packaging/docker-test.sh [IMAGE ...]      default: ubuntu:24.04 fedora:latest
# PLATFORM defaults to the architecture of this machine. Another one needs qemu user emulation registered with
# binfmt (for example: docker run --privileged --rm tonistiigi/binfmt --install arm64). The platform is always
# passed explicitly because a tag such as ubuntu:24.04 refers to whichever architecture was pulled last.
# Results (package files, per-image test results and report.html) go to build/containers/.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/build/containers
mkdir -p "$OUT"
if [ -z "${PLATFORM:-}" ]; then
    case $(uname -m) in
        aarch64|arm64) PLATFORM=linux/arm64 ;;
        *) PLATFORM=linux/amd64 ;;
    esac
fi
[ $# -gt 0 ] || set -- ubuntu:24.04 fedora:latest

status=0
for image in "$@"; do
    tag=$(printf '%s' "$image" | tr ':/' '--')-${PLATFORM#linux/}
    echo "=== $image"
    docker pull -q --platform "$PLATFORM" "$image" > /dev/null || status=1
    docker run --rm --platform "$PLATFORM" -e TAG="$tag" -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" -v "$ROOT":/src:ro -v "$OUT":/out "$image" sh /src/packaging/container-job.sh \
        || status=1
done

# one report that overlays all images (needs python3 on the host)
dirs=$(for d in "$OUT"/*/; do [ -f "$d/tests.json" ] && printf '%s ' "$d"; done)
if [ -n "$dirs" ] && command -v python3 > /dev/null 2>&1; then
    python3 "$ROOT/bench/report.py" $dirs -o "$OUT/compare.html"
fi
exit $status
