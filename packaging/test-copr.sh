#!/bin/sh
# Checks what COPR will do with this repository, in a clean Fedora container: build the source RPM through
# .copr/Makefile (the "make srpm" method), then rebuild the binary RPM from it (that runs `make test` in %check).
#
# usage: packaging/test-copr.sh      (needs docker and network; uses the working tree, committed or not)
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
docker pull -q --platform linux/amd64 fedora:latest > /dev/null
docker run --rm --platform linux/amd64 -v "$ROOT":/src:ro fedora:latest sh -c '
    dnf install -y -q rpm-build gcc make git tar gzip python3 > /dev/null 2>&1
    cp -a /src /repo
    git config --global --add safe.directory "*"
    cd /repo
    make -f .copr/Makefile srpm outdir=/tmp/srpm > /dev/null 2>&1 && ls /tmp/srpm/*.src.rpm > /dev/null \
        && echo "ok   source RPM built through .copr/Makefile: $(ls /tmp/srpm)" || { echo "FAIL source RPM"; exit 1; }
    rpmbuild --rebuild /tmp/srpm/*.src.rpm > /tmp/rebuild.log 2>&1 \
        && echo "ok   binary RPM rebuilt from the source RPM (includes make test): $(ls /root/rpmbuild/RPMS/*/ | grep -v debug | head -1)" \
        || { echo "FAIL rebuild"; tail -20 /tmp/rebuild.log; exit 1; }'
