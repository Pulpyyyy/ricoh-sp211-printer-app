# SPDX-License-Identifier: GPL-2.0-or-later
#
# CUPS driver for Ricoh SP 211 class GDI printers.
#
#   make            build the filter (and the stream checker)
#   make test       unit tests, no printer needed
#   sudo make install
#
# Needs the CUPS development files (Debian/Ubuntu: libcups2-dev).
# With libusb-1.0 (libusb-1.0-0-dev) the driver also reports the printer's
# page and misfeed counters; without it, it builds and prints all the same.

JBIG    = third_party/jbigkit
ENCODER = src/lilo2.c $(JBIG)/jbig.c $(JBIG)/jbig_ar.c
STATUS  = src/sp211_status.c src/sp211_usb.c
HEADERS = src/lilo2.h src/dither_matrix.h src/sp211_status.h src/sp211_usb.h

CFLAGS  ?= -O2 -Wall -Wextra
# CUPS marks the PPD API as deprecated; it is what a CUPS 2.x filter uses.
CFLAGS  += -Wno-deprecated-declarations
CPPFLAGS += -Isrc -I$(JBIG) $(shell cups-config --cflags 2>/dev/null)
LDLIBS  += $(shell cups-config --libs 2>/dev/null || echo -lcups)

ifeq ($(shell pkg-config --exists libusb-1.0 2>/dev/null && echo yes),yes)
USB_CPPFLAGS = -DHAVE_LIBUSB $(shell pkg-config --cflags libusb-1.0)
USB_LDLIBS   = $(shell pkg-config --libs libusb-1.0)
endif

CUPS_SERVERBIN ?= $(shell cups-config --serverbin 2>/dev/null || echo /usr/lib/cups)
CUPS_DATADIR   ?= $(shell cups-config --datadir 2>/dev/null || echo /usr/share/cups)

all: build/rastertosp211 build/lilo2check

build/rastertosp211: src/rastertosp211.c $(ENCODER) $(STATUS) $(HEADERS)
	mkdir -p build
	$(CC) $(CFLAGS) $(CPPFLAGS) $(USB_CPPFLAGS) -o $@ src/rastertosp211.c $(ENCODER) $(STATUS) $(LDLIBS) $(USB_LDLIBS)

# Decodes a printer stream and reports what is on each page.
build/lilo2check: tools/lilo2check.c $(JBIG)/jbig.c $(JBIG)/jbig_ar.c
	mkdir -p build
	$(CC) $(CFLAGS) -I$(JBIG) -o $@ tools/lilo2check.c $(JBIG)/jbig.c $(JBIG)/jbig_ar.c

# Unit tests: no printer, no CUPS, no libusb.
build/test_status: tests/test_status.c $(STATUS) src/sp211_status.h src/sp211_usb.h
	mkdir -p build
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_status.c $(STATUS)

test: build/test_status
	./build/test_status

# IPP Everywhere Printer Application, built on PAPPL (Debian/Ubuntu: libpappl-dev).
# Not part of "make all": PAPPL is optional for the CUPS driver above.
# PAPPL is under Apache-2.0, so this binary is distributed under GPL-3.0-or-later.
PAPPL_CFLAGS = $(shell pkg-config --cflags pappl 2>/dev/null)
PAPPL_LIBS   = $(shell pkg-config --libs pappl 2>/dev/null || echo -lpappl -lcups)
build/sp211-printer-app: src/sp211-printer-app.c $(ENCODER) $(STATUS) $(HEADERS)
	mkdir -p build
	$(CC) $(CFLAGS) -Isrc -I$(JBIG) $(PAPPL_CFLAGS) $(USB_CPPFLAGS) -o $@ src/sp211-printer-app.c \
	    $(ENCODER) $(STATUS) $(PAPPL_LIBS) $(USB_LDLIBS) -lpthread

printer-app: build/sp211-printer-app

# Installs the Printer Application and its systemd service (not started).
PREFIX ?= /usr/local
SYSTEMD_UNITDIR ?= /etc/systemd/system
install-printer-app: build/sp211-printer-app
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(SYSTEMD_UNITDIR)
	install -m 755 build/sp211-printer-app $(DESTDIR)$(PREFIX)/bin/sp211-printer-app
	sed 's|@BINDIR@|$(PREFIX)/bin|' packaging/sp211-printer-app.service \
	    > $(DESTDIR)$(SYSTEMD_UNITDIR)/sp211-printer-app.service
	chmod 644 $(DESTDIR)$(SYSTEMD_UNITDIR)/sp211-printer-app.service

# The PPD shipped here names the filter by its macOS path; installed PPDs
# refer to it by name, which CUPS resolves in its filter directory.
install: build/rastertosp211
	install -d $(DESTDIR)$(CUPS_SERVERBIN)/filter $(DESTDIR)$(CUPS_DATADIR)/model
	install -m 755 build/rastertosp211 $(DESTDIR)$(CUPS_SERVERBIN)/filter/rastertosp211
	sed 's|/Library/Printers/SP211Native/Filters/rastertosp211|rastertosp211|' \
	    ppd/RICOH-SP-211-native.ppd > $(DESTDIR)$(CUPS_DATADIR)/model/ricoh-sp211.ppd
	chmod 644 $(DESTDIR)$(CUPS_DATADIR)/model/ricoh-sp211.ppd

clean:
	rm -rf build

.PHONY: all test install clean printer-app install-printer-app
