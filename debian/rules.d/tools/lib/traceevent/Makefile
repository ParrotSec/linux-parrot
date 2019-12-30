include $(top_rulesdir)/Makefile.inc

DEB_HOST_MULTIARCH := $(shell dpkg-architecture -qDEB_HOST_MULTIARCH)

# Catch use of missing kernel APIs early
CFLAGS += -Werror=implicit-function-declaration

# Don't let the kernel version override traceevent's version
undefine VERSION
PREFIX=/usr

MAKE_TRACEEVENT := +$(MAKE) -C $(top_srcdir)/$(OUTDIR) O=$(CURDIR)
MAKE_TRACEEVENT += prefix=$(PREFIX)
MAKE_TRACEEVENT += libdir_relative=lib/$(DEB_HOST_MULTIARCH)
MAKE_TRACEEVENT += pkgconfig_dir=$(PREFIX)/lib/$(DEB_HOST_MULTIARCH)/pkgconfig
MAKE_TRACEEVENT += V=$(KBUILD_VERBOSE)
MAKE_TRACEEVENT += ARCH=$(KERNEL_ARCH)

all:
	$(MAKE_TRACEEVENT)

install:
	$(MAKE_TRACEEVENT) install
