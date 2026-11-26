/* SPDX-License-Identifier: GPL-2.0-or-later */
/* sp211_usbdev - see sp211_usbdev.h */
#include "sp211_usbdev.h"

#ifndef HAVE_LIBUSB

void sp211_usbdev_register(void) {}

#else

#include <errno.h>
#include <libusb.h>
#include <pappl/pappl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ID_TIMEOUT_MS 5000

typedef struct {
  libusb_context *ctx;
  libusb_device_handle *handle;
  int conf_index;  /* configuration index, for GET_DEVICE_ID */
  int iface;       /* interface number */
  int altset;
  unsigned char write_endp, read_endp; /* endpoint addresses; read_endp 0 = none */
  char device_id[1024];
} usbdev_t;

/* A found printer interface. */
typedef struct {
  int conf_index, conf_value, iface, altset, num_altsetting;
  unsigned char write_endp, read_endp;
} iface_t;

/* Finds the printer interface PAPPL would use: class 7, subclass 1,
 * protocol 1 or 2 (the highest), with a bulk OUT endpoint. */
static int find_interface(libusb_device *dev, const struct libusb_device_descriptor *desc,
                          iface_t *out) {
  int found = 0;
  memset(out, 0, sizeof *out);
  for (int c = 0; c < desc->bNumConfigurations && !found; c++) {
    struct libusb_config_descriptor *cfg;
    if (libusb_get_config_descriptor(dev, (uint8_t)c, &cfg) < 0) continue;
    for (int i = 0; i < cfg->bNumInterfaces && !found; i++) {
      const struct libusb_interface *itf = &cfg->interface[i];
      int best = 0;
      for (int a = 0; a < itf->num_altsetting; a++) {
        const struct libusb_interface_descriptor *alt = &itf->altsetting[a];
        if (alt->bInterfaceClass != LIBUSB_CLASS_PRINTER || alt->bInterfaceSubClass != 1) continue;
        if (alt->bInterfaceProtocol != 1 && alt->bInterfaceProtocol != 2) continue;
        if (alt->bInterfaceProtocol < best) continue;
        unsigned char w = 0, r = 0;
        for (int e = 0; e < alt->bNumEndpoints; e++) {
          const struct libusb_endpoint_descriptor *ep = &alt->endpoint[e];
          if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK) continue;
          if (ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) r = ep->bEndpointAddress;
          else w = ep->bEndpointAddress;
        }
        if (!w) continue;
        best = alt->bInterfaceProtocol;
        out->conf_index = c;
        out->conf_value = cfg->bConfigurationValue;
        out->iface = alt->bInterfaceNumber;
        out->altset = alt->bAlternateSetting;
        out->num_altsetting = itf->num_altsetting;
        out->write_endp = w;
        out->read_endp = best > 1 ? r : 0;
      }
      if (best) found = 1;
    }
    libusb_free_config_descriptor(cfg);
  }
  return found;
}

/* IEEE 1284 device ID (GET_DEVICE_ID class request), as PAPPL reads it. */
static int get_device_id(libusb_device_handle *h, int conf_index, int iface, int altset,
                         char *buf, size_t size) {
  unsigned char data[1024];
  int n = libusb_control_transfer(
      h, LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_ENDPOINT_IN | LIBUSB_RECIPIENT_INTERFACE, 0,
      (uint16_t)conf_index, (uint16_t)((iface << 8) | altset), data, sizeof data, ID_TIMEOUT_MS);
  buf[0] = '\0';
  if (n < 2) return n < 0 ? n : 0;
  /* big-endian length including the two bytes; some printers send it
   * little-endian */
  size_t len = ((size_t)data[0] << 8) | data[1];
  if (len > (size_t)n || len < 14) len = ((size_t)data[1] << 8) | data[0];
  if (len > (size_t)n) len = (size_t)n;
  if (len < 2) return 0;
  len -= 2;
  if (len >= size) len = size - 1;
  memcpy(buf, data + 2, len);
  buf[len] = '\0';
  return 1;
}

static void usb_string(libusb_device_handle *h, uint8_t index, char *buf, size_t size) {
  buf[0] = '\0';
  if (index && libusb_get_string_descriptor_ascii(h, index, (unsigned char *)buf, (int)size) < 0)
    buf[0] = '\0';
}

/* The device URI and description, as PAPPL builds them. */
static void make_uri(libusb_device_handle *h, const struct libusb_device_descriptor *desc,
                     const iface_t *itf, char *device_id, size_t id_size, char *uri,
                     size_t uri_size, char *info, size_t info_size) {
  char smake[256], smodel[256], sserial[256], fallback[32];
  cups_option_t *pairs = NULL;
  int n;
  const char *make, *model, *serial;

  usb_string(h, desc->iManufacturer, smake, sizeof smake);
  usb_string(h, desc->iProduct, smodel, sizeof smodel);
  usb_string(h, desc->iSerialNumber, sserial, sizeof sserial);
  if (!device_id[0])
    snprintf(device_id, id_size, "MFG:%s;MDL:%s;SN:%s;", smake[0] ? smake : "Unknown",
             smodel[0] ? smodel : "Product", sserial);

  n = papplDeviceParseID(device_id, &pairs);
  if ((make = cupsGetOption("MANUFACTURER", n, pairs)) == NULL &&
      (make = cupsGetOption("MFG", n, pairs)) == NULL)
    make = smake[0] ? smake : "Unknown";
  if ((model = cupsGetOption("MODEL", n, pairs)) == NULL &&
      (model = cupsGetOption("MDL", n, pairs)) == NULL)
    model = smodel[0] ? smodel : "Product";
  if ((serial = cupsGetOption("SERIALNUMBER", n, pairs)) == NULL &&
      (serial = cupsGetOption("SERN", n, pairs)) == NULL &&
      (serial = cupsGetOption("SN", n, pairs)) == NULL && sserial[0])
    serial = sserial;
  if (!serial || !*serial) {
    snprintf(fallback, sizeof fallback, "%d.%d", itf->conf_index, itf->iface);
    serial = fallback;
  }
  httpAssembleURIf(HTTP_URI_CODING_ALL, uri, (int)uri_size, "usb", NULL, make, 0, "/%s?serial=%s",
                   model, serial);
  snprintf(info, info_size, "%s %s (USB)", make, model);
  cupsFreeOptions(n, pairs);
}

typedef bool (*found_cb_t)(libusb_device_handle *h, const iface_t *itf, const char *info,
                           const char *uri, const char *device_id, void *data);

/* Walks the USB printers with a new libusb context; calls cb with each one
 * opened. When cb returns true the device stays open and is returned in
 * *ctx / *handle; otherwise everything is closed. */
static bool walk(found_cb_t cb, void *data, pappl_deverror_cb_t err_cb, void *err_data,
                 libusb_context **out_ctx, libusb_device_handle **out_handle, iface_t *out_itf,
                 char *out_id, size_t id_size) {
  libusb_context *ctx;
  libusb_device **list;
  bool found = false;
  int err;

  if ((err = libusb_init(&ctx)) != 0) {
    if (err_cb) {
      char msg[256];
      snprintf(msg, sizeof msg, "Unable to initialize USB access: %s", libusb_strerror(err));
      err_cb(msg, err_data);
    }
    return false;
  }
  ssize_t count = libusb_get_device_list(ctx, &list);
  for (ssize_t i = 0; i < count && !found; i++) {
    struct libusb_device_descriptor desc;
    iface_t itf;
    libusb_device_handle *h;
    char device_id[1024], uri[1024], info[256];

    if (libusb_get_device_descriptor(list[i], &desc) < 0 || desc.idVendor == 0x05ac) continue;
    if (!find_interface(list[i], &desc, &itf)) continue;
    if (libusb_open(list[i], &h) < 0) continue;
    if (get_device_id(h, itf.conf_index, itf.iface, itf.altset, device_id, sizeof device_id) < 0)
      device_id[0] = '\0';
    make_uri(h, &desc, &itf, device_id, sizeof device_id, uri, sizeof uri, info, sizeof info);
    if (cb(h, &itf, info, uri, device_id, data)) {
      found = true;
      if (out_handle) {
        *out_handle = h;
        *out_itf = itf;
        papplCopyString(out_id, device_id, id_size);
        continue;
      }
    }
    libusb_close(h);
  }
  if (count >= 0) libusb_free_device_list(list, 1);
  if (found && out_handle) *out_ctx = ctx;
  else libusb_exit(ctx);
  return found;
}

/* ---- list ---------------------------------------------------------------- */

typedef struct {
  pappl_device_cb_t cb;
  void *data;
} list_t;

static bool list_one(libusb_device_handle *h, const iface_t *itf, const char *info, const char *uri,
                     const char *device_id, void *data) {
  (void)h;
  (void)itf;
  list_t *l = data;
  return l->cb(info, uri, device_id, l->data);
}

static bool usb_list(pappl_device_cb_t cb, void *data, pappl_deverror_cb_t err_cb, void *err_data) {
  list_t l = {cb, data};
  return walk(list_one, &l, err_cb, err_data, NULL, NULL, NULL, NULL, 0);
}

/* ---- open, close, read, write, id ----------------------------------------- */

typedef struct {
  const char *uri;
  pappl_device_t *device;
} open_t;

/* The serial= value of a URI, or "". */
static void uri_serial(const char *uri, char *out, size_t size) {
  const char *p = strstr(uri, "serial=");
  size_t n = 0;
  if (p)
    for (p += 7; *p && *p != '&' && n + 1 < size; p++) out[n++] = *p;
  out[n] = '\0';
}

static bool open_match(libusb_device_handle *h, const iface_t *itf, const char *info, const char *uri,
                       const char *device_id, void *data) {
  (void)device_id;
  (void)info;
  open_t *o = data;
  if (strcmp(uri, o->uri)) {
    /* same printer if the serial number is the same, whatever the 1284 ID
     * said this time (it can be incomplete right after power-on) */
    char a[128], b[128];
    uri_serial(uri, a, sizeof a);
    uri_serial(o->uri, b, sizeof b);
    if (!a[0] || strcmp(a, b)) return false;
  }

  int err;
  libusb_set_auto_detach_kernel_driver(h, 1); /* the usblp driver, if loaded */
  int current = 0;
  if (libusb_get_configuration(h, &current) < 0) current = 0;
  if (current != itf->conf_value && (err = libusb_set_configuration(h, itf->conf_value)) < 0) {
    papplDeviceError(o->device, "Unable to set USB configuration: %s", libusb_strerror(err));
    return false;
  }
  if ((err = libusb_claim_interface(h, itf->iface)) < 0) {
    papplDeviceError(o->device, "Unable to claim USB interface: %s", libusb_strerror(err));
    return false;
  }
  if (itf->num_altsetting > 1 && (err = libusb_set_interface_alt_setting(h, itf->iface, itf->altset)) < 0) {
    papplDeviceError(o->device, "Unable to set alternate USB interface: %s", libusb_strerror(err));
    libusb_release_interface(h, itf->iface);
    return false;
  }
  return true;
}

static bool usb_open(pappl_device_t *device, const char *device_uri, const char *name) {
  (void)name;
  usbdev_t *u = calloc(1, sizeof *u);
  iface_t itf;
  open_t o = {device_uri, device};
  if (!u) {
    papplDeviceError(device, "Unable to allocate memory for USB device: %s", strerror(errno));
    return false;
  }
  if (!walk(open_match, &o, NULL, NULL, &u->ctx, &u->handle, &itf, u->device_id, sizeof u->device_id)) {
    free(u);
    return false; /* not connected, or switched off: PAPPL retries */
  }
  u->conf_index = itf.conf_index;
  u->iface = itf.iface;
  u->altset = itf.altset;
  u->write_endp = itf.write_endp;
  u->read_endp = itf.read_endp;
  papplDeviceSetData(device, u);
  return true;
}

static void usb_close(pappl_device_t *device) {
  usbdev_t *u = papplDeviceGetData(device);
  if (!u) return;
  libusb_release_interface(u->handle, u->iface);
  libusb_close(u->handle);
  libusb_exit(u->ctx);
  free(u);
}

static ssize_t usb_write(pappl_device_t *device, const void *buffer, size_t bytes) {
  usbdev_t *u = papplDeviceGetData(device);
  size_t done = 0;
  if (!u) return -1;
  /* never a zero-length transfer: it wedges the SP 211 */
  while (done < bytes) {
    int sent = 0;
    int chunk = bytes - done > 65536 ? 65536 : (int)(bytes - done);
    int err = libusb_bulk_transfer(u->handle, u->write_endp, (unsigned char *)buffer + done, chunk,
                                   &sent, 0);
    if (err < 0 && err != LIBUSB_ERROR_TIMEOUT) {
      papplDeviceError(device, "Unable to send print data: %s", libusb_strerror(err));
      return done ? (ssize_t)done : -1;
    }
    done += (size_t)sent;
  }
  return (ssize_t)done;
}

static ssize_t usb_read(pappl_device_t *device, void *buffer, size_t bytes) {
  usbdev_t *u = papplDeviceGetData(device);
  int got = 0;
  if (!u || !u->read_endp) return -1;
  int err = libusb_bulk_transfer(u->handle, u->read_endp, buffer, (int)bytes, &got, 1000);
  if (err < 0 && err != LIBUSB_ERROR_TIMEOUT) {
    papplDeviceError(device, "Unable to read from the printer: %s", libusb_strerror(err));
    return -1;
  }
  return got;
}

static char *usb_id(pappl_device_t *device, char *buffer, size_t bufsize) {
  usbdev_t *u = papplDeviceGetData(device);
  if (!u) return NULL;
  if (get_device_id(u->handle, u->conf_index, u->iface, u->altset, buffer, bufsize) <= 0 || !buffer[0])
    papplCopyString(buffer, u->device_id, bufsize);
  return buffer;
}

void sp211_usbdev_register(void) {
  papplDeviceAddScheme("usb", PAPPL_DEVTYPE_USB, usb_list, usb_open, usb_close, usb_read, usb_write,
                       NULL, usb_id);
}

#endif /* HAVE_LIBUSB */
