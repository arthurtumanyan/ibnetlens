# Changelog

## 1.0.1

- Fix a possible 16-byte over-read in the SIMD hex parser. The limit that bounds the vector load was a thread-local
  pointer that kept its value after a parse; a later parse of a smaller buffer with several threads could pass the
  bounds check wrongly. Found by the CI fuzzer, covered by a regression test and a fuzz corpus file. Reachable only
  when one process parses more than one input and the second is a small file read with an explicit `-j N`.
- README and documentation corrected after an audit of every claim (measurements refreshed, statements about other
  tools and about ibsim narrowed to what was verified).

## 1.0.0

Rewrite of topo_parser.

- Single-pass parser over mmap'd input; no strtok/sscanf, bounds-checked integer parsing.
- Keeps LIDs, LMC, link speed and width, descriptions, vendor and device IDs and routers (port labels of ibdiagnet files are dropped).
- Output formats: json, csv, csv-nodes, dot, slurm, ibnd, legacy (byte-compatible with topo_parser 0.x).
- `check`, `diff` and `info` commands.
- Reads the ibsim net-file dialect (free-form node IDs, Hca nodes, w= options).
- Parallel parsing and link resolution (`-j`), SSSE3 GUID parsing, prefetching.
- Slurm output: leaf detection by host/uplink balance, synthetic leaf for spines that serve hosts.
- Progress bar on terminals (`--progress`, `--no-progress`), `--stats`.
- Portable self-test (`tests/selftest.py`) with HTML report, checks against real ibnetdiscover and slurmctld.
- Debian and RPM packaging.
- Parallel parsing of ibsim net files; GUID assignment and duplicate IDs are settled after the merge.
- Reads the extended layout written by NVIDIA's ibdiagnet (port labels in parentheses).
- Cross-checks against NVIDIA ibdiagnet, ibtopodiff and infiniband-diags, and against fabrics of up to 61k nodes.
- MIT license; no third-party code.
- 54 MB / 1.1M-line dump: 23 s -> 0.03 s.
