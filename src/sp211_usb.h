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

#include <stddef.h>

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

/* Asks the printer to print one of its own pages: SP211_PAGE_TEST (the
 * "Test Page" pattern) or SP211_PAGE_MAINTENANCE (counters, misfeeds,
 * firmware). Sent as an @SDC_SET command with a vendor control request,
 * outside the print data stream; printing starts about 10 s later. Returns 1
 * if the printer took the request. */
#define SP211_PAGE_TEST "GDITESTPAGE"
#define SP211_PAGE_MAINTENANCE "GDIMAINTENANCE"
int sp211_usb_print_internal_page(const char *device_uri, const char *page);

/* Builds the request data for sp211_usb_print_internal_page() into buf.
 * Returns its length, 0 if it does not fit or page is not one of the above.
 * Exposed for the tests. */
size_t sp211_usb_internal_page_request(const char *page, unsigned char *buf, size_t size);

/* Extracts the serial number from a USB device URI. Returns 1 and fills
 * serial (NUL terminated) if the URI has one. Exposed for the tests. */
int sp211_usb_uri_serial(const char *device_uri, char *serial, size_t size);

#endif
