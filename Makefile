# SPDX-License-Identifier: GPL-2.0-or-later
#
# IPP Everywhere Printer Application for Ricoh SP 211 class GDI printers.
#
#   make            build sp211-printer-app (and the stream checker)
#   make test       unit tests, no printer needed
#   sudo make install
#
# Needs PAPPL (Debian/Ubuntu: libpappl-dev) and libusb-1.0 (libusb-1.0-0-dev).
# PAPPL is under Apache-2.0, so the binary is distributed under
# GPL-3.0-or-later (see NOTICE.md).

JBIG    = third_party/jbigkit
ENCODER = src/lilo2.c $(JBIG)/jbig.c $(JBIG)/jbig_ar.c
STATUS  = src/sp211_status.c src/sp211_usb.c
HEADERS = src/lilo2.h src/dither_matrix.h src/sp211_status.h src/sp211_usb.h

CFLAGS  ?= -O2 -Wall -Wextra

PAPPL_CFLAGS = $(shell pkg-config --cflags pappl 2>/dev/null)
PAPPL_LIBS   = $(shell pkg-config --libs pappl 2>/dev/null || echo -lpappl -lcups)
USB_CFLAGS   = -DHAVE_LIBUSB $(shell pkg-config --cflags libusb-1.0 2>/dev/null)
USB_LIBS     = $(shell pkg-config --libs libusb-1.0 2>/dev/null || echo -lusb-1.0)

all: build/sp211-printer-app build/lilo2check

build/sp211-printer-app: src/sp211-printer-app.c $(ENCODER) $(STATUS) $(HEADERS)
	mkdir -p build
	$(CC) $(CFLAGS) -Isrc -I$(JBIG) $(PAPPL_CFLAGS) $(USB_CFLAGS) -o $@ src/sp211-printer-app.c \
	    $(ENCODER) $(STATUS) $(PAPPL_LIBS) $(USB_LIBS) -lpthread

# Decodes a printer stream and reports what is on each page.
build/lilo2check: tools/lilo2check.c $(JBIG)/jbig.c $(JBIG)/jbig_ar.c
	mkdir -p build
	$(CC) $(CFLAGS) -I$(JBIG) -o $@ tools/lilo2check.c $(JBIG)/jbig.c $(JBIG)/jbig_ar.c

# Unit tests: no printer, no PAPPL, no libusb.
build/test_status: tests/test_status.c $(STATUS) src/sp211_status.h src/sp211_usb.h
	mkdir -p build
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_status.c $(STATUS)

test: build/test_status
	./build/test_status
	python3 tests/test_gdi_status.py

# Installs the Printer Application and its systemd service (not started).
PREFIX ?= /usr/local
SYSTEMD_UNITDIR ?= /etc/systemd/system
install: build/sp211-printer-app
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(SYSTEMD_UNITDIR)
	install -m 755 build/sp211-printer-app $(DESTDIR)$(PREFIX)/bin/sp211-printer-app
	sed 's|@BINDIR@|$(PREFIX)/bin|' packaging/sp211-printer-app.service \
	    > $(DESTDIR)$(SYSTEMD_UNITDIR)/sp211-printer-app.service
	chmod 644 $(DESTDIR)$(SYSTEMD_UNITDIR)/sp211-printer-app.service

clean:
	rm -rf build

.PHONY: all test install clean
