CC       ?= cc
CFLAGS   ?= -O2
THREADS  := -pthread
WARN     := -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes
CPPFLAGS += -D_POSIX_C_SOURCE=200809L
PREFIX   ?= /usr/local
BINDIR   ?= $(PREFIX)/bin
MANDIR   ?= $(PREFIX)/share/man
DOCDIR   ?= $(PREFIX)/share/doc/ibnetlens
VERSION  := $(shell sed -n 's/.*IBNETLENS_VERSION "\(.*\)"/\1/p' src/ibnetlens.h)

SRC     := $(wildcard src/*.c)
LIBSRC  := $(filter-out src/main.c,$(SRC))
OBJ     := $(SRC:src/%.c=build/%.o)
SANFLAGS := -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined

all: ibnetlens

ibnetlens: $(OBJ)
	$(CC) $(THREADS) $(LDFLAGS) -o $@ $^

build/%.o: src/%.c $(wildcard src/*.h) | build
	$(CC) $(CPPFLAGS) $(THREADS) $(WARN) $(CFLAGS) -c -o $@ $<

build:
	mkdir -p build

build/fattree: tests/gen/fattree.c | build
	$(CC) $(WARN) $(CFLAGS) -o $@ $<

build/test_core: tests/unit/test_core.c $(LIBSRC) $(wildcard src/*.h) | build
	$(CC) $(CPPFLAGS) $(THREADS) $(WARN) $(CFLAGS) -o $@ tests/unit/test_core.c $(LIBSRC)

test: ibnetlens build/fattree build/test_core
	tests/run.sh ./ibnetlens build/fattree
	build/test_core

check: ibnetlens build/fattree build/test_core
	tests/selftest.py --no-build

# Builds and tests without the SIMD code, as on CPUs that are not x86-64.
test-scalar: build/fattree | build
	$(CC) $(CPPFLAGS) -DIBNETLENS_NO_SIMD $(THREADS) $(WARN) $(CFLAGS) -o build/ibnetlens_scalar $(SRC)
	$(CC) $(CPPFLAGS) -DIBNETLENS_NO_SIMD $(THREADS) $(WARN) $(CFLAGS) -o build/test_core_scalar tests/unit/test_core.c $(LIBSRC)
	tests/run.sh build/ibnetlens_scalar build/fattree
	build/test_core_scalar
	build/ibnetlens_scalar info --stats -q tests/golden/ft-small.topo 2>&1 | grep -q 'simd=0'

asan: | build
	$(CC) $(CPPFLAGS) $(THREADS) $(WARN) $(SANFLAGS) -o build/ibnetlens_asan $(SRC)
	$(CC) $(CPPFLAGS) $(THREADS) $(WARN) $(SANFLAGS) -o build/test_core_asan tests/unit/test_core.c $(LIBSRC)
	$(CC) $(WARN) $(SANFLAGS) -o build/fattree_asan tests/gen/fattree.c
	tests/run.sh build/ibnetlens_asan build/fattree_asan
	build/test_core_asan

tsan: | build
	$(CC) $(CPPFLAGS) $(THREADS) $(WARN) -g -O1 -fsanitize=thread -o build/ibnetlens_tsan $(SRC)
	$(CC) $(WARN) -g -O1 -fsanitize=thread -o build/fattree_tsan tests/gen/fattree.c
	setarch $$(uname -m) -R tests/run.sh build/ibnetlens_tsan build/fattree_tsan

fuzz: | build
	clang $(CPPFLAGS) $(THREADS) -g -O1 -fsanitize=fuzzer,address,undefined -o build/parse_fuzz tests/fuzz/parse_fuzz.c $(LIBSRC)
	mkdir -p build/corpus && cp tests/golden/*.topo tests/golden/*.net tests/fuzz/regress/* build/corpus/
	# leak checking stays on in `make asan`; inside the fuzz loop LeakSanitizer slowed a threaded target by more than two orders of magnitude
	ASAN_OPTIONS=detect_leaks=0 build/parse_fuzz build/corpus -max_total_time=$(or $(FUZZ_SECONDS),60) -max_len=8192

samples: ibnetlens
	tests/fetch-samples.sh ./ibnetlens

bench: ibnetlens build/fattree
	tests/bench.sh ./ibnetlens build/fattree

static:
	$(MAKE) clean
	$(MAKE) CFLAGS="-O2" LDFLAGS="-static" ibnetlens

install: ibnetlens
	install -D -m 755 ibnetlens $(DESTDIR)$(BINDIR)/ibnetlens
	install -D -m 644 man/ibnetlens.1 $(DESTDIR)$(MANDIR)/man1/ibnetlens.1

install-doc:
	install -d $(DESTDIR)$(DOCDIR)
	install -m 644 README.md CHANGELOG.md LICENSE docs/DEVELOPER.md docs/TESTING.md docs/PACKAGING.md $(DESTDIR)$(DOCDIR)/

# Source tarball from the files tracked by git (working-tree contents); without git, from the
# source directories.
dist:
	mkdir -p build
	{ git ls-files --cached --others --exclude-standard 2>/dev/null || find src tests docs man bench debian packaging .github \
	    -type f -not -path 'bench/.*' -not -path 'debian/ibnetlens/*' -not -path 'debian/.debhelper/*' ; \
	  ls Makefile README.md CHANGELOG.md LICENSE .gitignore; } | sort -u \
	  | tar --transform 's,^,ibnetlens-$(VERSION)/,' -czf build/ibnetlens-$(VERSION).tar.gz -T -
	@echo build/ibnetlens-$(VERSION).tar.gz

deb:
	dpkg-buildpackage -us -uc -b
	mkdir -p build/pkg
	mv ../ibnetlens_$(VERSION)_*.deb ../ibnetlens_$(VERSION)_*.buildinfo ../ibnetlens_$(VERSION)_*.changes build/pkg/
	rm -f ../ibnetlens-dbgsym_$(VERSION)_*.ddeb
	@ls build/pkg/*.deb

rpm: dist
	mkdir -p build/rpm
	rpmbuild -ba packaging/ibnetlens.spec --define "_topdir $(CURDIR)/build/rpm" --define "_sourcedir $(CURDIR)/build"
	@ls build/rpm/RPMS/*/*.rpm

clean:
	rm -rf build ibnetlens

.PHONY: all test test-scalar check asan tsan fuzz samples bench static install install-doc dist deb rpm clean
