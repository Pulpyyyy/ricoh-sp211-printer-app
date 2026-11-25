/* SPDX-License-Identifier: GPL-2.0-or-later */
/* sp211_usb - see sp211_usb.h */
#include "sp211_usb.h"

#include <ctype.h>
#include <string.h>

int sp211_usb_uri_serial(const char *device_uri, char *serial, size_t size) {
  const char *p = device_uri ? strstr(device_uri, "serial=") : NULL;
  size_t n = 0;
  if (!p || !size) return 0;
  p += 7;
  while (*p && *p != '&' && n + 1 < size) {
    if (p[0] == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
      char hex[3] = {p[1], p[2], 0};
      unsigned v = 0;
      for (int i = 0; i < 2; i++)
        v = v * 16 + (unsigned)(isdigit((unsigned char)hex[i]) ? hex[i] - '0'
                                                              : tolower((unsigned char)hex[i]) - 'a' + 10);
      serial[n++] = (char)v;
      p += 3;
    } else {
      serial[n++] = *p++;
    }
  }
  serial[n] = '\0';
  return n > 0;
}

#ifdef HAVE_LIBUSB

#include <libusb.h>
#include <time.h>

#define RICOH_VENDOR_ID 0x05ca
#define REQ_PREPARE 0x60
#define REQ_READ 0x61
#define CONTROL_TIMEOUT_MS 1500

static double monotonic_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void sleep_ms(long ms) {
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

/* Reads one register into reg[64]. Returns 1 when a valid reply arrived
 * within timeout seconds. */
static int read_register(libusb_device_handle *h, unsigned index, double timeout,
                         unsigned char *reg) {
  unsigned char echo[2];
  if (index < SP211_REG_SAFE_FIRST || index > SP211_REG_SAFE_LAST) return 0; /* never */

  libusb_control_transfer(h, 0xC0, REQ_PREPARE, (uint16_t)index, 0, echo, sizeof echo,
                          CONTROL_TIMEOUT_MS);
  double deadline = monotonic_seconds() + timeout;
  for (;;) {
    sleep_ms(150);
    int r = libusb_control_transfer(h, 0xC0, REQ_READ, (uint16_t)index, 0, reg,
                                    SP211_REGISTER_LEN, CONTROL_TIMEOUT_MS);
    if (r == SP211_REGISTER_LEN && !memcmp(reg, "GDIL", 4) && reg[4] == index) return 1;
    if (r == LIBUSB_ERROR_NO_DEVICE || monotonic_seconds() > deadline) return 0;
  }
}

static int has_printer_interface(libusb_device *dev) {
  struct libusb_config_descriptor *cfg;
  int found = 0;
  if (libusb_get_active_config_descriptor(dev, &cfg) != 0) return 0;
  for (int i = 0; i < cfg->bNumInterfaces && !found; i++)
    for (int a = 0; a < cfg->interface[i].num_altsetting && !found; a++)
      if (cfg->interface[i].altsetting[a].bInterfaceClass == LIBUSB_CLASS_PRINTER) found = 1;
  libusb_free_config_descriptor(cfg);
  return found;
}

/* Opens the Ricoh printer named by the URI's serial number, or the first
 * Ricoh printer if the URI has none. */
static libusb_device_handle *open_printer(libusb_context *ctx, const char *device_uri) {
  char wanted[128];
  int have_serial = sp211_usb_uri_serial(device_uri, wanted, sizeof wanted);
  libusb_device **list;
  libusb_device_handle *result = NULL;
  ssize_t count = libusb_get_device_list(ctx, &list);

  for (ssize_t i = 0; i < count && !result; i++) {
    struct libusb_device_descriptor desc;
    libusb_device_handle *h;
    if (libusb_get_device_descriptor(list[i], &desc) != 0) continue;
    if (desc.idVendor != RICOH_VENDOR_ID || !has_printer_interface(list[i])) continue;
    if (libusb_open(list[i], &h) != 0) continue;
    if (have_serial) {
      unsigned char serial[128] = {0};
      if (!desc.iSerialNumber ||
          libusb_get_string_descriptor_ascii(h, desc.iSerialNumber, serial, sizeof serial) <= 0 ||
          strcmp((char *)serial, wanted)) {
        libusb_close(h);
        continue;
      }
    }
    result = h;
  }
  if (count >= 0) libusb_free_device_list(list, 1);
  return result;
}

int sp211_usb_read_counters(const char *device_uri, int want_misfeeds, sp211_counters *out) {
  libusb_context *ctx;
  unsigned char reg[SP211_REGISTER_LEN];

  sp211_counters_init(out);
  if (libusb_init(&ctx) != 0) return 0;
  libusb_device_handle *h = open_printer(ctx, device_uri);
  if (h) {
    if (read_register(h, SP211_REG_COUNTERS, 1.0, reg))
      sp211_decode_register1(reg, sizeof reg, out);
    if (!out->have_pages && read_register(h, SP211_REG_COUNTERS_ALT, 1.0, reg))
      sp211_decode_register3(reg, sizeof reg, out);
    if (out->have_pages && want_misfeeds && read_register(h, SP211_REG_MISFEEDS, 6.0, reg))
      sp211_decode_misfeeds(reg, sizeof reg, out);
    libusb_close(h);
  }
  libusb_exit(ctx);
  return out->have_pages;
}

#else /* !HAVE_LIBUSB */

int sp211_usb_read_counters(const char *device_uri, int want_misfeeds, sp211_counters *out) {
  (void)device_uri;
  (void)want_misfeeds;
  sp211_counters_init(out);
  return 0;
}

#endif
