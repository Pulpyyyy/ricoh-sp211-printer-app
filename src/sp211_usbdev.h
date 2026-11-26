/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sp211_usbdev - the "usb" device scheme of this application, in place of
 * PAPPL's own.
 *
 * PAPPL's USB code keeps one libusb context for the whole life of the
 * process, and relies on udev events to learn about devices that come and
 * go. In a container (LXC) those events never arrive: once the printer has
 * been switched off and on again, it reappears under a new device number
 * that PAPPL never sees, and a job waiting for it retries forever. This
 * scheme opens a new libusb context for every list and every open, so it
 * always sees the devices as they are now.
 *
 * Device URIs are built exactly as PAPPL builds them
 * (usb://MAKE/MODEL?serial=SERIAL, from the IEEE 1284 device ID and the USB
 * strings), so printers added before keep working. It never sends a
 * zero-length transfer (see docs/STATUS_PROTOCOL.md).
 */
#ifndef SP211_USBDEV_H
#define SP211_USBDEV_H

/* Registers the scheme. Must run before anything lists or opens a device:
 * PAPPL keeps the first scheme registered under a name, and registers its
 * own schemes only when none exists yet (so "file", "socket" and "dnssd"
 * devices are not available in this application, which drives USB printers
 * only). Does nothing when built without libusb. */
void sp211_usbdev_register(void);

#endif
