# Packaging ibnetlens

ibnetlens has no runtime dependencies beyond libc. Packages are built from the same source tree with the same
`Makefile`.

## Make targets

| target | result |
|---|---|
| `make install` | binary and man page under `$(DESTDIR)$(PREFIX)` (`BINDIR`, `MANDIR` can be overridden) |
| `make install-doc` | README, changelog, license and the documents in `docs/` into `$(DOCDIR)` |
| `make dist` | `build/ibnetlens-<version>.tar.gz` from the tracked files |
| `make deb` | `build/pkg/ibnetlens_<version>_<arch>.deb` through `dpkg-buildpackage` |
| `make rpm` | `build/rpm/RPMS/<arch>/ibnetlens-<version>-1.<dist>.<arch>.rpm` through `rpmbuild -ba packaging/ibnetlens.spec` |
| `make static` | statically linked binary |

The version comes from `IBNETLENS_VERSION` in `src/ibnetlens.h`. When it changes, update `debian/changelog` and
`packaging/ibnetlens.spec` (`Version`, `%changelog`); `packaging/test-package.sh` fails if the package version
differs from the source.

## Debian and Ubuntu

Needs `build-essential debhelper fakeroot`.

```
make deb
dpkg-deb -I build/pkg/ibnetlens_1.0.1_amd64.deb
sudo dpkg -i build/pkg/ibnetlens_1.0.1_amd64.deb
```

`debian/` is a native source package (`3.0 (native)`). The build runs `make test` (`override_dh_auto_test`),
with hardening flags enabled. `debian/tests/smoke` is an autopkgtest. The license is shipped as
`debian/copyright`; `LICENSE` is not installed twice.

## Fedora

Needs `gcc make rpm-build` (and `cpio` for the package test).

```
make rpm
rpm -qpi build/rpm/RPMS/x86_64/ibnetlens-1.0.1-1.*.x86_64.rpm
sudo dnf install build/rpm/RPMS/x86_64/ibnetlens-1.0.1-1.*.rpm
```

`packaging/ibnetlens.spec` builds with the distribution's `%optflags` and `%build_ldflags`, runs `make test` in
`%check`, and installs the license with `%license`.

## Fedora COPR

COPR can build and host the RPMs. The points below come from the COPR user documentation
(https://docs.copr.fedorainfracloud.org/user_documentation.html); the repository side was tested locally.

- Only people with a Fedora Account System account can create projects and builds.
- Create the project in the web UI, or with `copr-cli create ibnetlens --chroot fedora-rawhide-x86_64` and more `--chroot`
  options. ibnetlens was built and tested on Fedora 44 only (x86-64 and aarch64).
- Add the package with the SCM source type: Clone URL of the repository, Committish `v<version>`, Spec File
  `packaging/ibnetlens.spec`, SRPM build method `make srpm`. COPR then runs
  `make -f .copr/Makefile srpm outdir=... spec=...`; `.copr/Makefile` builds the source tarball with `make dist` and the
  source RPM with `rpmbuild -bs`.
- For automatic builds, put the webhook URL from the project's integrations page into the repository's webhook
  settings with content type `application/json`, and select tag push events to build on tags.
- Users then run `dnf copr enable <owner>/ibnetlens` and `dnf install ibnetlens`.
- COPR signs the repositories it generates; the ibnetlens release key is not involved. Old builds are deleted after
  14 days, one build per package is kept.
- `packaging/test-copr.sh` repeats in a clean Fedora container what COPR will do: it builds the source RPM through
  `.copr/Makefile` and rebuilds the binary RPM from it.

## Testing a package

```
packaging/test-package.sh --build              # build the native package for this system and test it
packaging/test-package.sh --deb FILE           # or --rpm FILE, on an existing package
sudo packaging/test-package.sh --install --build   # also install and remove it
```

It checks package metadata, contents, that only libc is linked, that the man page renders without groff
warnings, and runs the full golden suite against the binary taken out of the package.

## Clean-room builds in containers

```
packaging/docker-test.sh                       # ubuntu:24.04 and fedora:latest
packaging/docker-test.sh debian:12          # other images may work; only Ubuntu 24.04 and Fedora 44 have been tested
PLATFORM=linux/arm64 packaging/docker-test.sh ubuntu:24.04 fedora:latest   # needs qemu binfmt, see docs/TESTING.md
```

For each image the script starts a container, installs build and test dependencies (including `ibsim`,
`infiniband-diags` and `slurmctld` where the distribution has them), builds the native package, installs and
removes it, then runs `tests/selftest.py` against the installed binary. Package files and per-image result
directories with `report.html` land in `build/containers/` (named `<image>-<arch>`). The platform defaults to this
machine's architecture and is always passed to Docker explicitly: a tag such as `ubuntu:24.04` refers to whichever
architecture was pulled last, which would otherwise make an "x86" run silently use an ARM image.

## Signing a release

`packaging/sign-release.sh KEY_FINGERPRINT` gathers the release files into `dist/` and signs them: an embedded
signature for each RPM (`rpmsign`, run in a Fedora container), a detached `.asc` for every file, and a signed
`SHA256SUMS`. It then verifies everything with a throw-away keyring and with `rpm -K` in a clean container. The public
key is `packaging/ibnetlens-release.asc`. The release key has no passphrase so that releases can be scripted; add one
with `gpg --edit-key <fingerprint> passwd` if the key lives on a shared machine, and keep the revocation certificate
that gpg stored in `~/.gnupg/openpgp-revocs.d/`.

## Release checklist

1. Update the version in `src/ibnetlens.h`, `man/ibnetlens.1` (`.TH`), `debian/changelog`, `packaging/ibnetlens.spec`, `CHANGELOG.md`.
2. `make test asan tsan` and `make fuzz FUZZ_SECONDS=1800`.
3. `python3 tests/selftest.py --bench full --sanitizers --samples` on each supported machine type.
4. `packaging/docker-test.sh` for the supported distributions.
5. `packaging/sign-release.sh <fingerprint>`, then create the signed tag (`git tag -s vX.Y.Z`) and attach the files from
   `dist/` to the release.
