DATA = \
	Kbuild \
	Makefile \

SUBDIRS = \
	scripts \
	tools

DEB_HOST_ARCH := $(shell dpkg-architecture -qDEB_HOST_ARCH)
ifneq ($(filter powerpc powerpcspe ppc64,$(DEB_HOST_ARCH)),)
	SUBDIRS += arch/powerpc/boot
endif

include $(top_rulesdir)/Makefile.inc

# Build userland headers first
unexport VERSION
all-local:
	mkdir -p headers-tools
	$(MAKE) -C $(top_srcdir) ARCH=$(KERNEL_ARCH) O=$(CURDIR)/headers-tools \
		INSTALL_HDR_PATH=$(CURDIR) headers_install
all-recursive: all-local
