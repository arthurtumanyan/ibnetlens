#!/bin/sh
# Tests a built .deb or .rpm without installing it (unless --install is given as root):
# metadata, contents, linked libraries, the packaged binary against the golden test suite.
#
# usage: test-package.sh [--install] (--deb FILE | --rpm FILE | --build)
#   --build   builds the native package for this system first (deb or rpm) and tests it
# Prints "ok", "FAIL" or "skip" lines like tests/run.sh.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
INSTALL=0
KIND=
FILE=
while [ $# -gt 0 ]; do
    case $1 in
        --install) INSTALL=1 ;;
        --deb) KIND=deb; FILE=$2; shift ;;
        --rpm) KIND=rpm; FILE=$2; shift ;;
        --build) KIND=build ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
[ -n "$KIND" ] || { echo "usage: $0 [--install] (--deb FILE | --rpm FILE | --build)" >&2; exit 2; }

VERSION=$(sed -n 's/.*IBNETLENS_VERSION "\(.*\)"/\1/p' "$ROOT/src/ibnetlens.h")
fail=0
ok()   { printf 'ok   %s\n' "$1"; }
bad()  { printf 'FAIL %s\n' "$1"; fail=1; }
skip() { printf 'skip %s\n' "$1"; }

if [ "$KIND" = build ]; then
    if command -v dpkg-buildpackage > /dev/null 2>&1; then
        (cd "$ROOT" && make deb > /dev/null 2>&1) && FILE=$(ls "$ROOT"/build/pkg/ibnetlens_*.deb | head -1) && KIND=deb \
            || { bad "make deb"; exit 1; }
    elif command -v rpmbuild > /dev/null 2>&1; then
        (cd "$ROOT" && make rpm > /dev/null 2>&1) && FILE=$(ls "$ROOT"/build/rpm/RPMS/*/ibnetlens-*.rpm | head -1) && KIND=rpm \
            || { bad "make rpm"; exit 1; }
    else
        skip "package build (neither dpkg-buildpackage nor rpmbuild found)"
        exit 0
    fi
    ok "built $(basename "$FILE")"
fi

[ -f "$FILE" ] || { bad "package file $FILE"; exit 1; }
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# metadata and extraction
if [ "$KIND" = deb ]; then
    [ "$(dpkg-deb -f "$FILE" Package)" = ibnetlens ] && ok "deb package name" || bad "deb package name"
    [ "$(dpkg-deb -f "$FILE" Version)" = "$VERSION" ] && ok "deb version $VERSION" || bad "deb version ($(dpkg-deb -f "$FILE" Version), source $VERSION)"
    dpkg-deb -f "$FILE" Depends | grep -q libc6 && ok "deb depends on libc6" || bad "deb depends on libc6"
    dpkg-deb -x "$FILE" "$TMP/root"
    LICENSE_PATH=usr/share/doc/ibnetlens/copyright
else
    if command -v rpm > /dev/null 2>&1; then
        [ "$(rpm -qp --qf '%{NAME}' "$FILE")" = ibnetlens ] && ok "rpm package name" || bad "rpm package name"
        [ "$(rpm -qp --qf '%{VERSION}' "$FILE")" = "$VERSION" ] && ok "rpm version $VERSION" || bad "rpm version"
        [ "$(rpm -qp --qf '%{LICENSE}' "$FILE")" = MIT ] && ok "rpm license MIT" || bad "rpm license"
    else
        skip "rpm metadata (rpm not found)"
    fi
    mkdir "$TMP/root"
    (cd "$TMP/root" && rpm2cpio "$FILE" | cpio -idm 2> /dev/null) || bad "extract rpm"
    LICENSE_PATH=usr/share/licenses/ibnetlens/LICENSE
fi
R=$TMP/root
BIN=$R/usr/bin/ibnetlens

[ -x "$BIN" ] && ok "binary installed to /usr/bin" || { bad "binary installed to /usr/bin"; exit 1; }
ls "$R"/usr/share/man/man1/ibnetlens.1* > /dev/null 2>&1 && ok "man page installed" || bad "man page installed"
[ -f "$R/$LICENSE_PATH" ] && ok "license file installed" || bad "license file installed"
ls "$R"/usr/share/doc/ibnetlens/README.md* > /dev/null 2>&1 && ok "documentation installed" || bad "documentation installed"

if command -v ldd > /dev/null 2>&1; then
    extra=$(ldd "$BIN" 2> /dev/null | grep -v -E 'linux-vdso|libc\.so|ld-linux|libm\.so|libpthread|statically' | grep -c .)
    [ "$extra" -eq 0 ] && ok "links only libc" || bad "links $extra unexpected libraries"
fi
[ "$("$BIN" --version)" = "ibnetlens $VERSION" ] && ok "packaged binary reports $VERSION" || bad "packaged binary version"

MANFILE=$(ls "$R"/usr/share/man/man1/ibnetlens.1* | head -1)
case $MANFILE in
    *.gz) text=$(gzip -dc "$MANFILE") ;;
    *) text=$(cat "$MANFILE") ;;
esac
printf '%s\n' "$text" | grep -q '^.TH IBNETLENS 1' && ok "man page header" || bad "man page header"
if command -v groff > /dev/null 2>&1; then
    printf '%s\n' "$text" | groff -man -Tascii -ww -z 2>&1 | grep -q . && bad "man page has groff warnings" || ok "man page renders without groff warnings"
fi

# the packaged binary against the full golden suite
GEN=$ROOT/build/fattree
[ -x "$GEN" ] || (cd "$ROOT" && make build/fattree > /dev/null 2>&1)
if [ -x "$GEN" ]; then
    out=$("$ROOT/tests/run.sh" "$BIN" "$GEN" 2>&1)
    nfail=$(printf '%s\n' "$out" | grep -c '^FAIL')
    nok=$(printf '%s\n' "$out" | grep -c '^ok')
    [ "$nfail" -eq 0 ] && ok "golden suite with the packaged binary ($nok checks)" || { bad "golden suite with the packaged binary"; printf '%s\n' "$out" | grep '^FAIL'; }
else
    skip "golden suite (could not build the fixture generator)"
fi

# real install and removal, only when running as root and asked to
if [ "$INSTALL" -eq 1 ]; then
    if [ "$(id -u)" -ne 0 ]; then
        skip "install and remove (not root)"
    elif [ "$KIND" = deb ]; then
        dpkg -i "$FILE" > /dev/null 2>&1 && [ -x /usr/bin/ibnetlens ] && ok "dpkg -i installs it" || bad "dpkg -i installs it"
        [ "$(ibnetlens --version)" = "ibnetlens $VERSION" ] && ok "installed binary runs" || bad "installed binary runs"
        dpkg -r ibnetlens > /dev/null 2>&1 && [ ! -e /usr/bin/ibnetlens ] && ok "dpkg -r removes it" || bad "dpkg -r removes it"
    else
        rpm -i "$FILE" > /dev/null 2>&1 || dnf install -y -q "$FILE" > /dev/null 2>&1
        [ -x /usr/bin/ibnetlens ] && ok "rpm installs it" || bad "rpm installs it"
        [ "$(ibnetlens --version)" = "ibnetlens $VERSION" ] && ok "installed binary runs" || bad "installed binary runs"
        rpm -e ibnetlens > /dev/null 2>&1 && [ ! -e /usr/bin/ibnetlens ] && ok "rpm -e removes it" || bad "rpm -e removes it"
    fi
fi
exit $fail
