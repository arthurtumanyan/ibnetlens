#!/bin/sh
# Runs inside a container started by packaging/docker-test.sh: installs build and test dependencies,
# builds the native package, tests it, installs it and runs the full self-test against the installed binary.
# /src is the read-only source tree, /out receives the results.
set -u
. /etc/os-release
TAG=${TAG:-$ID-$VERSION_ID}
mkdir -p /work /out
cp -a /src/. /work/
cd /work || exit 2
rm -rf build results bench/.work debian/ibnetlens

export DEBIAN_FRONTEND=noninteractive
case "$ID" in
    ubuntu|debian)
        apt-get update -qq > /dev/null
        apt-get install -y -qq build-essential debhelper fakeroot python3 groff-base curl binutils xz-utils zstd \
            ibsim-utils infiniband-diags slurmctld pciutils > /dev/null 2>&1 \
            || apt-get install -y -qq build-essential debhelper fakeroot python3 groff-base curl binutils xz-utils zstd > /dev/null
        ;;
    fedora|rhel|centos|rocky|almalinux)
        dnf install -y -q --skip-unavailable gcc make rpm-build python3 groff-base cpio curl binutils xz zstd \
            ibsim infiniband-diags slurm-slurmctld slurm-auth-none pciutils > /dev/null 2>&1 \
            || dnf install -y -q gcc make rpm-build python3 groff-base cpio curl binutils xz zstd > /dev/null
        ;;
esac

# NVIDIA's ibdiagnet and ibtopodiff exist for x86-64 only
if [ "$(uname -m)" = x86_64 ]; then
    NVPREFIX=$(tests/fetch-nvidia.sh /tmp/nvidia | tail -1) || echo "NVIDIA packages could not be fetched"
    export NVPREFIX
fi

status=0
echo "== $TAG: build and test the package"
packaging/test-package.sh --install --build | tee /out/package.txt || status=1

echo "== $TAG: self-test against the installed binary"
make build/fattree build/test_core > /dev/null 2>&1
BIN=$(command -v ibnetlens)
if [ -z "$BIN" ]; then
    # the package test removes ibnetlens again; reinstall it for the self-test
    pkg=$(ls build/pkg/*.deb build/rpm/RPMS/*/*.rpm 2> /dev/null | head -1)
    case "$pkg" in
        *.deb) dpkg -i "$pkg" > /dev/null 2>&1 ;;
        *.rpm) rpm -i "$pkg" > /dev/null 2>&1 ;;
    esac
    BIN=$(command -v ibnetlens)
fi
python3 tests/selftest.py --no-build --bin "$BIN" --tag "$TAG" --out "/out/$TAG" --bench quick || status=1
cp /out/package.txt "/out/$TAG/package.txt" 2> /dev/null
mkdir -p /out/packages
cp build/pkg/*.deb build/rpm/RPMS/*/*.rpm /out/packages/ 2> /dev/null
# hand the files back to the user who started the container
[ -n "${HOST_UID:-}" ] && chown -R "$HOST_UID:${HOST_GID:-$HOST_UID}" /out
exit $status
