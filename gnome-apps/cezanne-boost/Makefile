# cezanne-boost: touch boost (uclamp.min) for the shell
PREFIX ?= /usr
CFLAGS ?= -O2
CFLAGS += -std=gnu11 -Wall

all: cezanne-boost

cezanne-boost: cezanne-boost.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ cezanne-boost.c

install: all
	install -Dm755 cezanne-boost $(DESTDIR)$(PREFIX)/bin/cezanne-boost
	install -Dm644 cezanne-boost.service $(DESTDIR)$(PREFIX)/lib/systemd/system/cezanne-boost.service

clean:
	rm -f cezanne-boost

.PHONY: all install clean
