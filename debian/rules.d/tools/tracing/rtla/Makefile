include $(top_rulesdir)/Makefile.inc

all:
# Out-of-tree build is not supported, so copy source to the build dir
	echo '$(UPSTREAMVERSION)' >VERSION
	rsync -a $(top_srcdir)/tools/tracing/rtla/ .
	rsync -a $(top_srcdir)/Documentation/tools/rtla/ Documentation/
	$(MAKE) EXTRA_CFLAGS='$(CFLAGS) $(CPPFLAGS)' EXTRA_LDFLAGS='$(LDFLAGS)'

install:
	$(MAKE) install
ifneq (,$(filter nodoc,$(DEB_BUILD_PROFILES)))
	rm -rf $(DESTDIR)/usr/share/man
endif

clean:
	rm -rf *
