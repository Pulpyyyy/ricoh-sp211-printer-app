# SPDX-License-Identifier: GPL-2.0-or-later
#
# IPP Everywhere Printer Application for Ricoh SP 211 class GDI printers.
#
#   make            build sp211-printer-app (and the stream checker);
#                   VERSION=x.y.z sets the version it announces
#   make test       unit tests, no printer needed
#   sudo make install
#
# Needs PAPPL (Debian/Ubuntu: libpappl-dev) and libusb-1.0 (libusb-1.0-0-dev);
# libmosquitto (libmosquitto-dev) for MQTT, optional.
# PAPPL is under Apache-2.0, so the binary is distributed under
# GPL-3.0-or-later (see NOTICE.md).

JBIG    = third_party/jbigkit
ENCODER = src/lilo2.c $(JBIG)/jbig.c $(JBIG)/jbig_ar.c
STATUS  = src/sp211_status.c src/sp211_usb.c src/sp211_power.c src/sp211_json.c \
          src/sp211_config.c
HEADERS = src/lilo2.h src/dither_matrix.h src/sp211_status.h src/sp211_usb.h \
          src/sp211_power.h src/sp211_json.h src/sp211_app.h src/sp211_mqtt.h \
          src/sp211_config.h src/sp211_usbdev.h

CFLAGS  ?= -O2 -Wall -Wextra

# The version the application announces over IPP and MQTT. The package build
# passes its own (packaging/build-deb.sh). make does not see a change of
# VERSION alone: run "make clean" first.
VERSION ?= 0.2
VERSION_CFLAGS = -DSP211_APP_VERSION='"$(VERSION)"'

PAPPL_CFLAGS = $(shell pkg-config --cflags pappl 2>/dev/null)
PAPPL_LIBS   = $(shell pkg-config --libs pappl 2>/dev/null || echo -lpappl -lcups)
USB_CFLAGS   = -DHAVE_LIBUSB $(shell pkg-config --cflags libusb-1.0 2>/dev/null)
USB_LIBS     = $(shell pkg-config --libs libusb-1.0 2>/dev/null || echo -lusb-1.0)
# MQTT for Home Assistant (Debian/Ubuntu: libmosquitto-dev). Left out when the
# library is missing; MQTT=no leaves it out, MQTT=yes requires it.
MQTT ?= $(shell pkg-config --exists libmosquitto 2>/dev/null && echo yes || echo no)
ifeq ($(MQTT),yes)
MQTT_CFLAGS  = -DHAVE_MOSQUITTO $(shell pkg-config --cflags libmosquitto 2>/dev/null)
MQTT_LIBS    = $(shell pkg-config --libs libmosquitto 2>/dev/null || echo -lmosquitto)
endif

all: build/sp211-printer-app build/lilo2check

APP_SRC = src/sp211-printer-app.c src/sp211_mqtt.c src/sp211_usbdev.c

build/sp211-printer-app: $(APP_SRC) $(ENCODER) $(STATUS) $(HEADERS)
	mkdir -p build
	$(CC) $(CFLAGS) $(VERSION_CFLAGS) -Isrc -I$(JBIG) $(PAPPL_CFLAGS) $(USB_CFLAGS) \
	    $(MQTT_CFLAGS) -o $@ \
	    $(APP_SRC) $(ENCODER) $(STATUS) \
	    $(PAPPL_LIBS) $(USB_LIBS) $(MQTT_LIBS) -lpthread

# Decodes a printer stream and reports what is on each page.
build/lilo2check: tools/lilo2check.c $(JBIG)/jbig.c $(JBIG)/jbig_ar.c
	mkdir -p build
	$(CC) $(CFLAGS) -I$(JBIG) -o $@ tools/lilo2check.c $(JBIG)/jbig.c $(JBIG)/jbig_ar.c

# Unit tests: no printer, no PAPPL, no libusb.
build/test_status: tests/test_status.c $(STATUS) $(HEADERS)
	mkdir -p build
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_status.c $(STATUS)

test: build/test_status
	./build/test_status
	python3 tests/test_gdi_status.py

# Installs the Printer Application and its systemd service (not started).
PREFIX ?= /usr/local
SYSTEMD_UNITDIR ?= /etc/systemd/system
SYSCONFDIR ?= /etc
install: build/sp211-printer-app
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(SYSTEMD_UNITDIR)
	install -m 755 build/sp211-printer-app $(DESTDIR)$(PREFIX)/bin/sp211-printer-app
	install -d $(DESTDIR)$(SYSCONFDIR)/default
	[ -e $(DESTDIR)$(SYSCONFDIR)/default/sp211-printer-app ] || \
	    install -m 600 packaging/sp211-printer-app.default $(DESTDIR)$(SYSCONFDIR)/default/sp211-printer-app
	sed 's|@BINDIR@|$(PREFIX)/bin|' packaging/sp211-printer-app.service \
	    > $(DESTDIR)$(SYSTEMD_UNITDIR)/sp211-printer-app.service
	chmod 644 $(DESTDIR)$(SYSTEMD_UNITDIR)/sp211-printer-app.service

clean:
	rm -rf build

.PHONY: all test install clean
