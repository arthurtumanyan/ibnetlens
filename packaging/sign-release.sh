#!/bin/sh
# Collects the release files in dist/ and signs them:
#   - every .rpm gets an embedded RPM signature (rpmsign, run in a Fedora container so no RPM tools are needed here)
#   - every file (packages, source tarball) gets a detached armored signature (.asc)
#   - SHA256SUMS lists all files and is signed too (SHA256SUMS.asc)
# Then everything is verified: gpg for the detached signatures, rpm -K in a clean Fedora container for the RPMs.
#
# usage: packaging/sign-release.sh KEY_FINGERPRINT
# Needs gpg with the secret key (no passphrase prompt), docker, the packages in build/containers/packages/
# (packaging/docker-test.sh) and the source tarball (make dist).
set -eu
FPR=${1:?usage: sign-release.sh KEY_FINGERPRINT}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DIST=$ROOT/dist
PUB=$ROOT/packaging/ibnetlens-release.asc
VERSION=$(sed -n 's/.*IBNETLENS_VERSION "\(.*\)"/\1/p' "$ROOT/src/ibnetlens.h")

rm -rf "$DIST"
mkdir -p "$DIST"
cp "$ROOT"/build/containers/packages/ibnetlens_"$VERSION"_*.deb "$DIST"/
cp "$ROOT"/build/containers/packages/ibnetlens-"$VERSION"-*.rpm "$DIST"/
rm -f "$DIST"/ibnetlens-debuginfo-* "$DIST"/ibnetlens-debugsource-*
(cd "$ROOT" && make dist > /dev/null && cp build/ibnetlens-"$VERSION".tar.gz "$DIST"/)

# embedded RPM signatures
KEYFILE=$(mktemp)
trap 'rm -f "$KEYFILE"' EXIT
chmod 600 "$KEYFILE"
gpg --batch --pinentry-mode loopback --passphrase '' --export-secret-keys --armor "$FPR" > "$KEYFILE"
docker pull -q --platform linux/amd64 fedora:latest > /dev/null
docker run --rm --platform linux/amd64 -v "$DIST":/dist -v "$KEYFILE":/key.asc:ro -v "$PUB":/pub.asc:ro \
    -e FPR="$FPR" -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" fedora:latest sh -c '
    dnf install -y -q rpm-sign gnupg2 > /dev/null 2>&1
    gpg --batch --import /key.asc 2> /dev/null
    for f in /dist/*.rpm; do
        rpmsign --define "_gpg_name $FPR" --define "__gpg_sign_cmd_extra_args --pinentry-mode loopback --passphrase \"\"" --addsign "$f" > /dev/null
    done
    chown "$HOST_UID:$HOST_GID" /dist/*.rpm'

# detached signatures and checksums
for f in "$DIST"/*; do
    gpg --batch --yes --pinentry-mode loopback --passphrase '' --local-user "$FPR" --armor --detach-sign --output "$f.asc" "$f"
done
(cd "$DIST" && sha256sum ibnetlens* > SHA256SUMS)
gpg --batch --yes --pinentry-mode loopback --passphrase '' --local-user "$FPR" --armor --detach-sign \
    --output "$DIST/SHA256SUMS.asc" "$DIST/SHA256SUMS"
cp "$PUB" "$DIST/"

# verification with a throw-away keyring, as a user would do it
GNUPGHOME=$(mktemp -d)
export GNUPGHOME
gpg --batch --import "$PUB" 2> /dev/null
status=0
for f in "$DIST"/*.asc; do
    case $f in
        *ibnetlens-release.asc) continue ;; # the public key itself
        *SHA256SUMS.asc) target=$DIST/SHA256SUMS ;;
        *) target=${f%.asc} ;;
    esac
    if gpg --batch --verify "$f" "$target" > /dev/null 2>&1; then echo "ok   gpg signature of $(basename "$target")"
    else echo "FAIL gpg signature of $(basename "$target")"; status=1; fi
done
(cd "$DIST" && sha256sum -c SHA256SUMS > /dev/null 2>&1) && echo "ok   SHA256SUMS" || { echo "FAIL SHA256SUMS"; status=1; }
docker run --rm --platform linux/amd64 -v "$DIST":/dist:ro -v "$PUB":/pub.asc:ro fedora:latest sh -c '
    rpm --import /pub.asc
    for f in /dist/*.rpm; do
        if rpm -K "$f" 2>&1 | grep -q "digests signatures OK"; then echo "ok   rpm -K $(basename "$f")"
        else echo "FAIL rpm -K $(basename "$f")"; rpm -K "$f"; exit 1; fi
    done' || status=1
rm -rf "$GNUPGHOME"
ls -l "$DIST"
exit $status
