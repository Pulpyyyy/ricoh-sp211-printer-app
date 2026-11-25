/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sp211_usb - read the printer's counters over USB.
 *
 * The Ricoh SP 211 class printers answer USB vendor control requests on the
 * default endpoint, independently of the print data stream:
 *
 *   bmRequestType 0xC0, bRequest 0x60, wValue = register   "prepare"
 *   bmRequestType 0xC0, bRequest 0x61, wValue = register   "read", 64 bytes
 *
 * The reply is built asynchronously: until it is ready the read returns
 * zeros, so it is polled. Control requests do not need the printer
 * interface, which the print job holds while it prints.
 *
 * Built without libusb (HAVE_LIBUSB undefined), the function below returns 0
 * and the driver simply reports no counters.
 */
#ifndef SP211_USB_H
#define SP211_USB_H

#include "sp211_status.h"

/* Reads the page counter and toner level (and, if want_misfeeds, the misfeed
 * counters, which takes about 3 s more).
 *
 * device_uri: the printer's USB device URI ("usb://RICOH/SP%20211?serial=XXXX") used
 * to pick the right printer when several are connected; may be NULL.
 *
 * Returns 1 if the page counter was read, 0 if the printer could not be
 * reached (switched off, unplugged, no permission, no libusb). Never blocks
 * for more than a few seconds. */
int sp211_usb_read_counters(const char *device_uri, int want_misfeeds, sp211_counters *out);

/* Extracts the serial number from a USB device URI. Returns 1 and fills
 * serial (NUL terminated) if the URI has one. Exposed for the tests. */
int sp211_usb_uri_serial(const char *device_uri, char *serial, size_t size);

#endif
