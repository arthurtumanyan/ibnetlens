Name:           ibnetlens
Version:        1.0.1
Release:        1%{?dist}
Summary:        Parse, validate and convert InfiniBand topology files
License:        MIT
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make

%description
ibnetlens reads the topology files written by ibnetdiscover, and the net files used
by ibsim, and converts them to JSON, CSV, Graphviz or a Slurm tree topology.conf.
It can also check a fabric for inconsistencies (duplicate GUIDs and LIDs, links
without a matching reverse entry, mismatched or degraded link speeds) and compare
two snapshots. Parsing is single pass and multi-threaded.

%prep
%autosetup

%build
%make_build CFLAGS="%{optflags}" LDFLAGS="%{build_ldflags}"

%install
%make_install PREFIX=%{_prefix}

%check
make test CFLAGS="%{optflags}"

%files
%license LICENSE
%doc README.md CHANGELOG.md docs/DEVELOPER.md docs/TESTING.md docs/PACKAGING.md
%{_bindir}/ibnetlens
%{_mandir}/man1/ibnetlens.1*

%changelog
* Sat Oct 17 2026 Arthur Tumanyan <arthurtumanyan@gmail.com> - 1.0.1-1
- Fix a possible 16-byte over-read in the SIMD hex parser (found by fuzzing).
- Documentation corrected after an audit of its claims.

* Sat Oct 17 2026 Arthur Tumanyan <arthurtumanyan@gmail.com> - 1.0.0-1
- Rewrite of topo_parser: multi-threaded parser, ibsim net file support,
  json/csv/dot/slurm/ibnd output, check and diff commands.
