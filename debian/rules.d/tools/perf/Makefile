include $(top_rulesdir)/Makefile.inc

ifeq ($(filter nopython,$(DEB_BUILD_PROFILES)),)

ifneq ($(filter alpha arm arm64 mips parisc s390 sh sparc,$(KERNEL_ARCH)),)
HAVE_PERF := 1
else ifeq ($(KERNEL_ARCH),powerpc)
  # Pure 32-bit toolchains currently don't work
  ifneq ($(shell dpkg-architecture -qDEB_HOST_ARCH),powerpcspe)
    HAVE_PERF := 1
  endif
else ifeq ($(KERNEL_ARCH),x86)
  # x32 isn't supported yet
  ifneq ($(shell dpkg-architecture -qDEB_HOST_ARCH),x32)
    HAVE_PERF := 1
  endif
endif

MAKE_PERF := $(MAKE) O=$(CURDIR) prefix=/usr V=1 ARCH=$(KERNEL_ARCH) EXTRA_WARNINGS=-Wno-error EXTRA_CFLAGS='$(CFLAGS) $(CPPFLAGS)' LDFLAGS='$(LDFLAGS)'

# Disable Gtk UI until it's more usable
MAKE_PERF += NO_GTK2=1

# Include version in all directory names
MAKE_PERF += perfexecdir=lib/perf_$(VERSION)-core plugindir=/usr/lib/traceevent_$(VERSION)/plugins STRACE_GROUPS_DIR=share/perf_$(VERSION)-core/strace/groups tipdir=share/doc/linux-perf-$(VERSION)

# perf can link against libbfd if available, but the result is
# undistributable as they are licenced under GPL v2 and v3+
# respectively.  Override detection of libbfd and insist that
# cplus_demangle() can be found in libiberty (LGPL v2.1+).
MAKE_PERF += feature-libbfd=0 HAVE_CPLUS_DEMANGLE_SUPPORT=1

# perf can link against libcrypto if available, but the result is
# undistributable as GPL v2 and OpenSSL are not compatible without
# an explicit exception.  Override detection of libcrypto.
MAKE_PERF += NO_LIBCRYPTO=1

# Currently babeltrace support for `perf data' is not automatically detected.
MAKE_PERF += LIBBABELTRACE=1

# Build with asciidoctor, not asciidoc
MAKE_PERF += USE_ASCIIDOCTOR=1

# Build with Python 3, not Python 2
MAKE_PERF += PYTHON=/usr/bin/python3

all:
ifdef HAVE_PERF
# perf changes some default directories depending on whether DESTDIR is
# set.  We must define it even when building to avoid a rebuild when we
# run 'make install'.
	+$(MAKE_PERF) -C $(top_srcdir)/tools/perf -f Makefile.perf all VERSION=$(VERSION) DESTDIR=dummy
ifeq (,$(filter nodoc,$(DEB_BUILD_PROFILES)))
	+$(MAKE_PERF) -C $(top_srcdir)/tools/perf/Documentation man VERSION=$(VERSION)
endif
# Check that perf didn't get linked against libbfd or libcrypto
	type ldd
	! ldd $(CURDIR)/perf | grep -E '\blib(bfd|crypto)'
# Check that it includes cplus_demangle from libiberty
	grep cplus_demangle $(CURDIR)/perf
endif

install:
ifdef HAVE_PERF
	+$(MAKE_PERF) -C $(top_srcdir)/tools/perf -f Makefile.perf install VERSION=$(VERSION)
# Don't install a 'trace' alias yet:
# - We need a wrapper for it anyway, so there's little point adding a
#   versioned link
# - It doesn't work out-of-the-box as non-root (it depends on debugfs),
#   so it's less widely useful than strace
# - 'perf trace' doesn't take much more typing
	rm -f $(DESTDIR)/usr/bin/trace_$(VERSION)
endif
	mkdir -p $(DESTDIR)/usr/share/bash-completion/
	mv $(DESTDIR)/etc/bash_completion.d \
		$(DESTDIR)/usr/share/bash-completion/completions
	rmdir --ignore-fail-on-non-empty $(DESTDIR)/etc
# Check for unversioned files that are likely to result in file conflicts
	! find $(DESTDIR) -name '*perf*' \! -path '*[_-]$(VERSION)*' | grep .

endif # !nopython
