# sfm - suckless file manager
# See LICENSE file for copyright and license details.

.POSIX:

include config.mk

SRC = sfm.c
OBJ = $(SRC:.c=.o)

all: sfm

config.h:
	cp config.def.h config.h

.c.o:
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ): config.h config.mk

sfm: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS)

clean:
	rm -f sfm $(OBJ)

dist: clean
	rm -rf sfm-$(VERSION)
	mkdir -p sfm-$(VERSION)
	cp -R Makefile config.mk config.def.h sfm.c LICENSE sfm-$(VERSION)
	tar -cf sfm-$(VERSION).tar sfm-$(VERSION)
	gzip -f sfm-$(VERSION).tar
	rm -rf sfm-$(VERSION)

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f sfm $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/sfm

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/sfm

.PHONY: all clean dist install uninstall
