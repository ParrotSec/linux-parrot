include $(top_rulesdir)/Makefile.inc

DEB_HOST_MULTIARCH := $(shell dpkg-architecture -qDEB_HOST_MULTIARCH)

# Catch use of missing kernel APIs early
CFLAGS += -Werror=implicit-function-declaration

MAKE_BPF := +$(MAKE) -C $(top_srcdir)/$(OUTDIR) O=$(CURDIR)
MAKE_BPF += prefix=/usr
MAKE_BPF += libdir_relative=lib/$(DEB_HOST_MULTIARCH)
MAKE_BPF += V=$(KBUILD_VERBOSE)

all:
	$(MAKE_BPF)

install:
	$(MAKE_BPF) install install_headers
