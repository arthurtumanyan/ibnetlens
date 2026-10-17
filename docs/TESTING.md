# Testing ibnetlens

## One command on any machine

```
git clone <repository> && cd ibnetlens
python3 tests/selftest.py --tag my-machine
```

This builds the program (`make`, using `$CC` if set), runs every suite that can run on the machine, and writes a
result directory `results/<tag>-<timestamp>/` containing:

| file | content |
|---|---|
| `machine.json` | CPU, core count, memory, OS, kernel, compiler, ibnetlens version and commit, SIMD path, GPU (informational) |
| `tests.json` | every check with suite, name and status (`ok`, `FAIL`, `skip`) |
| `bench.json` | benchmark data (only with `--bench`) |
| `report.html` | self-contained report: machine table, test results, charts |

Open `report.html` in a browser. The report needs nothing but Python's standard library to build and a browser
to view; the charts are inline SVG.

Useful options:

| option | meaning |
|---|---|
| `--bench quick` / `--bench full` | also run the benchmark (quick: seconds, full: about a minute) |
| `--sanitizers` | also run `make asan` and `make tsan` |
| `--samples` | download and test public sample dumps (needs network) |
| `--large` | also cross-check fabrics of 16k to 61k nodes against the real `ibnetdiscover` and ibdiagnet (several minutes) |
| `--tag NAME` | label shown in the report (default: the host name); the host name is not stored unless `--with-hostname` |
| `--bin PATH` | test an installed binary instead of the freshly built one |
| `--no-build` | do not run `make` |
| `--out DIR` | write results elsewhere |

Exit status is 0 when nothing failed.

`make check` does the same for the already-built tree.

## Comparing machines

Run the self-test with `--bench full` on each machine, copy the result directories to one place, and build a
combined report:

```
python3 bench/report.py results/machine-a-... results/machine-b-... -o compare.html --svg-dir compare-charts
```

The charts then overlay the machines (colour per machine, line style per tool).

## What the suites check

| suite | what it proves | needs |
|---|---|---|
| golden and behaviour (`tests/run.sh`) | output of every format equals reviewed golden files; `ibnd` output re-parses to the same fabric; threaded and single-threaded runs are identical; strict mode; progress and stats options; exit codes; diff and check expectations | `sh`, `cmp`, `awk` |
| unit (`tests/unit/test_core.c`) | GUID table, hostlist compression, parser edge cases (CRLF, overflow, ibsim names), SIMD and scalar hex parsers agree | C compiler |
| real ibnetdiscover (`tests/xcheck-ibnetdiscover.sh`) | the real `ibnetdiscover`, `iblinkinfo` and `ibnetdiscover --diff` from infiniband-diags, run on fabrics simulated by `ibsim`, agree with ibnetlens: strict parse of the real output, node and port counts, identical link sets, diff results | `ibsim`, `infiniband-diags` |
| NVIDIA ibdiagnet and ibtopodiff (`tests/xcheck-nvidia.sh`) | NVIDIA's `ibdiagnet` discovers a simulated fabric and ibnetlens reads the topology file it writes (extended port-label layout) with equal nodes and links; `ibtopodiff` and `ibnetlens diff` agree on removed hosts and on identical files; `ibtopodiff` accepts ibnetlens's `ibnd` output | `ibutils2` from NVIDIA DOCA (`tests/fetch-nvidia.sh`), `ibsim` |
| real slurmctld (`tests/xcheck-slurm.sh`) | a real `slurmctld` loads the generated `topology.conf` with the tree plugin and builds the switch levels without errors | `slurmctld` |
| public sample dumps (`tests/fetch-samples.sh`) | dumps published by other projects parse in strict mode and round-trip | network |
| AddressSanitizer / ThreadSanitizer | memory and data-race errors in the full golden suite | `--sanitizers`; clang or gcc with sanitizer runtimes |

Suites whose tools are missing report `skip`, not failure. The `slurmctld` check also needs Slurm's `auth/none`
plugin; where the distribution does not ship it (Fedora at the time of writing) that suite reports `skip`.

NVIDIA's `ibdiagnet`, `ibtopodiff` and a matching `ibnetdiscover` are not in the distributions' repositories; they come
from NVIDIA's public DOCA repository. `tests/fetch-nvidia.sh` downloads and unpacks them without root and prints the
directory to use:

```
export NVPREFIX=$(tests/fetch-nvidia.sh)       # unpacks into build/nvidia/root
export IBPREFIX=$NVPREFIX                       # use NVIDIA's ibnetdiscover/iblinkinfo/ibsim for the other suite too
python3 tests/selftest.py --tag my-machine
```

The binaries target x86-64 and glibc 2.38 or newer. NVIDIA UFM is a licensed platform and is not covered.

Installing the other optional tools:

```
# Debian, Ubuntu
sudo apt install ibsim-utils infiniband-diags slurmctld
# Fedora (other RPM distributions: package names may differ)
sudo dnf install ibsim infiniband-diags slurm-slurmctld
```

Without root, unpack the packages into a directory and point the environment at it:
`IBPREFIX=/path/to/root SLURMPREFIX=/path/to/root python3 tests/selftest.py`.

## Other architectures

The whole suite runs in containers for another architecture when qemu user emulation is registered:

```
docker run --privileged --rm tonistiigi/binfmt --install arm64     # once per boot
PLATFORM=linux/arm64 packaging/docker-test.sh ubuntu:24.04 fedora:latest
```

On those CPUs `simd` shows `no (scalar)` in the report. `make test-scalar` runs the same tests on x86 with the SIMD code
left out. Emulated runs are slow; their timings say nothing about real ARM hardware.

## Individual commands

```
make test                      # golden + unit tests
make asan                      # same under AddressSanitizer and UBSan
make tsan                      # same under ThreadSanitizer
make fuzz FUZZ_SECONDS=120     # libFuzzer (needs clang)
make test-scalar               # tests without the SIMD code (as on non-x86 CPUs)
make samples                   # public sample dumps
tests/xcheck-nvidia.sh ./ibnetlens build/fattree
make bench                     # quick comparison with a plain Python parser
tests/xcheck-ibnetdiscover.sh ./ibnetlens build/fattree
tests/xcheck-slurm.sh ./ibnetlens tests/golden/ft-medium.topo
```

## Benchmarks

```
python3 bench/run.py --out results/mine [--quick]
bench/fetch-analogs.sh        # optional: download and port the ibtopology script for comparison
python3 bench/audit.py FILE.topo ...   # how many hosts end up reachable in a generated topology.conf
```

The benchmark generates two-level fat-trees with `tests/gen/fattree.c` (32 hosts per leaf) from about 300 to
135,000 nodes, times ibnetlens with one thread and with automatic threading, the Python baseline in
`bench/ref_parse.py`, and ibtopology when present. Timing uses wall-clock time of the whole process; memory
is ibnetlens's own `--stats` figure (`VmHWM`) and GNU time for the Python programs. Run the benchmark on an idle
machine and with the power profile you intend to measure; results from laptops vary with thermal state and
with performance and efficiency cores.

## Adding or updating golden files

Golden outputs were reviewed by hand when created. When a change legitimately alters an output, regenerate the
file, read the diff, and commit it together with the code change. Never regenerate goldens to make a failing
test pass without understanding the difference.

## Fuzzing

`tests/fuzz/parse_fuzz.c` feeds arbitrary bytes through the parser (with 3 threads), resolution, the checks,
every writer and the diff. `make fuzz` seeds it from `tests/golden/`. LeakSanitizer is switched off inside the fuzz loop (in our runs it slowed a threaded target by more than two orders of magnitude); leaks are
checked by `make asan` and by valgrind. A release should survive at least 30 minutes without a finding.
