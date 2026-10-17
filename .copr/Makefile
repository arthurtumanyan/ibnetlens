# Fedora COPR builds the source RPM with this target (SRPM build method "make srpm", see docs/PACKAGING.md).
outdir ?= $(CURDIR)/build/srpm

srpm:
	mkdir -p $(outdir)
	$(MAKE) dist
	rpmbuild -bs packaging/ibnetlens.spec --define "_topdir $(CURDIR)/build/rpm" \
	    --define "_sourcedir $(CURDIR)/build" --define "_srcrpmdir $(outdir)"

.PHONY: srpm
