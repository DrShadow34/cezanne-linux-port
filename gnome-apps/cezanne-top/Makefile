# cezanne-top: libcezanne-top.so (used by Mission Center's magpie) + CLI
PREFIX ?= /usr
LIBDIR ?= $(PREFIX)/lib
VERSION = 0.1
CFLAGS ?= -O2
CFLAGS += -std=gnu11 -Wall -Wno-format-truncation -fPIC

all: libcezanne-top.so.0 cezanne-top cezanne-top.pc

libcezanne-top.so.0: ctop.c ctop.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -shared -Wl,-soname,$@ -o $@ ctop.c

cezanne-top: cezanne-top.c ctop.h libcezanne-top.so.0
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ cezanne-top.c libcezanne-top.so.0

cezanne-top.pc: Makefile
	printf 'prefix=%s\nlibdir=%s\nincludedir=%s/include\n\nName: cezanne-top\nDescription: GPU and media engine usage for SoCs\nVersion: %s\nLibs: -L$${libdir} -lcezanne-top\nCflags: -I$${includedir}\n' \
		'$(PREFIX)' '$(LIBDIR)' '$(PREFIX)' '$(VERSION)' > $@

install: all
	install -Dm755 cezanne-top $(DESTDIR)$(PREFIX)/bin/cezanne-top
	install -Dm755 libcezanne-top.so.0 $(DESTDIR)$(LIBDIR)/libcezanne-top.so.0
	ln -sf libcezanne-top.so.0 $(DESTDIR)$(LIBDIR)/libcezanne-top.so
	install -Dm644 ctop.h $(DESTDIR)$(PREFIX)/include/ctop.h
	install -Dm644 cezanne-top.pc $(DESTDIR)$(LIBDIR)/pkgconfig/cezanne-top.pc

clean:
	rm -f libcezanne-top.so.0 cezanne-top cezanne-top.pc

.PHONY: all install clean
