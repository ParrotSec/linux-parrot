include $(top_rulesdir)/Makefile.inc

MAKE_BPFTOOL := +$(MAKE) -C $(top_srcdir)/$(OUTDIR) O=$(CURDIR)
MAKE_BPFTOOL += prefix=/usr
MAKE_BPFTOOL += mandir=/usr/share/man
MAKE_BPFTOOL += V=$(KBUILD_VERBOSE)
MAKE_BPFTOOL += ARCH=$(KERNEL_ARCH)
MAKE_BPFTOOL += EXTRA_CFLAGS='$(CFLAGS) $(CPPFLAGS)'
MAKE_BPFTOOL += EXTRA_LDFLAGS='$(LDFLAGS)'

# dynamically linking with libbfd is not allowed in Debian
MAKE_BPFTOOL += feature-libbfd=0 feature-libbfd-liberty=0 feature-libbfd-liberty-z=0

all:
	$(MAKE_BPFTOOL)
ifeq (,$(filter nodoc,$(DEB_BUILD_PROFILES)))
	$(MAKE_BPFTOOL) doc
endif
# Check that bpftool wasn't linked with libbfd
	type ldd
	! ldd $(CURDIR)/bpftool | grep -E '\blibbfd'

install:
	$(MAKE_BPFTOOL) install
ifeq (,$(filter nodoc,$(DEB_BUILD_PROFILES)))
	$(MAKE_BPFTOOL) doc-install
# doc-install installs a bpf-helpers.7 that conflicts
# with the one distributed by the manpages package.
	rm $(DESTDIR)/usr/share/man/man7/bpf-helpers.7
	rmdir $(DESTDIR)/usr/share/man/man7/
endif
