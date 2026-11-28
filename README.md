# ricoh-sp211-printer-app

An IPP Everywhere printer for the Ricoh SP 211 on Linux, built on
[PAPPL](https://www.msweet.org/pappl/). Windows, macOS, iOS, Android and
Home Assistant print to it and read its status without any driver.

- **Prints without Ricoh's software**: pages are converted to the printer's
  own "lilo2" stream by the encoder of
  [Creative8428/ricoh-sp211-driver](https://github.com/Creative8428/ricoh-sp211-driver).
- **Reports the printer's state over IPP**: toner level (`printer-supply`,
  `marker-levels`), toner low and empty, the printer's own page counter
  (`printer-impressions-completed`), its firmware version, and "stopped"
  while the printer cannot be reached.
- **Publishes the printer over MQTT** (optional), for any MQTT client, with
  Home Assistant discovery: jobs, counters, misfeeds, test pages, and a
  power state that a smart plug can follow, to switch the printer on when a
  document arrives and off when it has been idle.
- **Runs as a service** on a small Linux machine with the printer on USB, for
  example a Proxmox LXC container.

Other printers of the same family (SP 210, SP 213, SP 220, SP 221) use the
same print format and may work; only the SP 211 has been tested.

CUPS is not needed. It is going this way itself: CUPS 2.4 warns that printer
drivers such as PPD files "are deprecated and will stop working in a future
version of CUPS", in favour of printer applications like this one.

## Install

### From the package (Debian 13, amd64 or arm64)

Download `sp211-printer-app_VERSION_ARCH.deb` from the
[releases](https://github.com/Pulpyyyy/ricoh-sp211-printer-app/releases)
(or, for the latest commit, from the artifacts of the "Package" workflow),
then, with the printer on USB:

```
sudo apt install ./sp211-printer-app_*.deb
sudo sp211-printer-app add -d SP211 -m ricoh_sp211 \
     -v "$(sp211-printer-app devices | grep -m1 -o 'usb://RICOH[^ ]*')"
```

`apt` installs the libraries it needs (PAPPL, libusb) and the package starts
the service. The packages are built by GitHub Actions in a Debian 13
container, with the tests run before packaging (`packaging/build-deb.sh`).

### From source

On Debian 13 (or Ubuntu), with the printer on USB:

```
sudo apt install build-essential libpappl-dev libusb-1.0-0-dev libmosquitto-dev avahi-daemon
make
make test
sudo make install
sudo systemctl enable --now sp211-printer-app
sudo sp211-printer-app add -d SP211 -m ricoh_sp211 \
     -v "$(sp211-printer-app devices | grep -m1 -o 'usb://RICOH[^ ]*')"
```

The application announces its version over IPP and MQTT: 0.2 by default, or
the one given with `make VERSION=x.y.z` (after `make clean` if it was already
built). The packages announce their own version.

The service listens on the IPP port, 631, and announces the printer on the
network (DNS-SD), so clients find "SP211" by themselves. Its data (queue,
settings) is kept in `/var/lib/sp211-printer-app`, and its web interface is
at `http://HOST:631/`. Only one program at a time may drive the printer over
USB: do not run CUPS with a queue for the same printer on that machine.

To try it next to CUPS, on another port:

```
make
build/sp211-printer-app server -o server-port=8632 -o spool-directory=DIR
build/sp211-printer-app add -d SP211 -m ricoh_sp211 -v DEVICE-URI
```

### In a Proxmox LXC container

The printer's USB device must be passed to the container. In
`/etc/pve/lxc/ID.conf` on the Proxmox host:

```
lxc.cgroup2.devices.allow: c 189:* rwm
lxc.mount.entry: /dev/bus/usb dev/bus/usb none bind,optional,create=dir
```

189 is the device number of USB devices. In an unprivileged container the
device files also need permissions that the container's root can use. This
LXC setup has **not been tested yet** (the tests ran in an existing container
that already had the printer).

### MQTT

Set the broker in `/etc/default/sp211-printer-app` (`MQTT_HOST`, and
`MQTT_USERNAME`, `MQTT_PASSWORD` if it asks for a login) and restart the
service. The printer's state is then published on
`sp211-printer-app/NAME/state` (JSON), commands are taken on
`sp211-printer-app/NAME/set/...`, and any MQTT client can use them: Home
Assistant, Node-RED, openHAB, Jeedom, a script. Plain MQTT or MQTT over
TLS. [docs/MQTT.md](docs/MQTT.md) describes every setting, topic, field and
command.

### Home Assistant

Home Assistant's IPP integration finds the printer by itself, or can be
pointed at host `HOST`, port `631`, path `/ipp/print/SP211`. It shows:

- the printer state: idle, printing, or stopped while the printer is
  switched off or unplugged; the state sensor's `state_reason` attribute
  holds `toner-low`, `toner-empty` or `offline`;
- the toner level ("Black Toner", percent);
- the page counter ("impressions"), the printer's own lifetime total;
- the printer's firmware first in the device's firmware field, followed by
  the versions of this application and of its libraries (PAPPL adds those).

That is all the IPP integration reads. For more, enable MQTT (above): with
MQTT discovery, the printer then also appears by itself as a device of the
MQTT integration, with:

- the current job, the jobs waiting, and the last job with its time and
  result;
- the page counter and the paper misfeeds;
- buttons for the printer's test and maintenance pages;
- a power switch, with auto power on when a document arrives and auto power
  off after a delay; the blueprint
  [`homeassistant/blueprints/sp211_power.yaml`](homeassistant/blueprints/sp211_power.yaml)
  links it to any plug in two fields.

See [docs/MQTT.md](docs/MQTT.md). The CUPS card shows all of it on a
dashboard.

## Status

Tested on one printer: Ricoh SP 211, firmware V1.06, Debian 13, PAPPL 1.3.1.

| | State |
|---|---|
| Printing from Windows (IPP Class Driver, gzip over IPPS) | verified on paper |
| PWG raster and Apple raster (AirPrint) in, A4 and Letter | verified: same image bands as Ricoh's driver, byte for byte |
| Heavy page (gray, about 350 KB of printer data) | verified on paper |
| Paper size taken from the page received; copies | verified |
| Toner level and toner low | verified: 50 % |
| Toner empty (`toner-empty` at 0 %) | not tested |
| Firmware version in `printer-firmware-string-version` | verified: V1.06 first; read at start-up |
| Queue stopped while the printer cannot be reached, resumed when it answers | verified: stopped about 30 s after the printer went off, resumed when it answered again |
| Page counter as `printer-impressions-completed` | verified |
| MQTT: device, jobs, counters | verified: device and entities in Home Assistant, last job, page counter, misfeeds |
| MQTT: test and maintenance pages | verified: both printed on request, printer on; waiting for a printer that is off, and switching it on: implemented, not tested on the printer yet |
| MQTT: auto power on and off through the blueprint | verified: auto power off after the delay; a job sent from Windows with the printer off switched it on and printed, 20 s after it was sent |
| Out of paper | not yet: the printer only reports it during a job |
| 1200×600 dpi, manual duplex | not supported yet |
| Cover open, jams in progress | not available: the printer does not transmit them |

### Known limits

The application has its own USB access (`src/sp211_usbdev.c`) in place of
PAPPL's, for the `usb://` device URIs, which it builds the same way. PAPPL's
keeps one libusb context for the life of the process and learns about
devices that come and go from udev; in a container those events never
arrive, so after the printer was switched off and on (it then comes back
under a new USB device number) a job waiting for it retried forever. The
application's USB access looks the devices up again at every open. It is the
only device type it offers: no network or file devices.

Data is written to the printer in pieces of 2 KB, and the USB access never
sends a zero-length transfer. PAPPL's papplDeviceWrite() asks for one when
given more than its 8 KB buffer while the buffer is empty; the SP 211 then
keeps the job without printing it and refuses all further data until it is
switched off and on. PAPPL fixed this after 1.4.12
([issue #434](https://github.com/michaelrsweet/pappl/issues/434)); no
release contains the fix yet, and Debian 13 ships 1.3.1.

`printer-impressions-completed` follows the printer's own counter upwards
only: PAPPL counts the pages of a job that failed, and its API cannot lower
the value. It is then ahead of the printer by those pages.

PAPPL 1.3.1 (Debian 13) drops the last digit of numbers in its debug log,
including in its own messages; printing is not affected.

## Tools

- `tools/ricoh_gdi_status.py`: reads the printer directly (toner, counters,
  misfeeds, paper state, the printer's job log), and can ask it to print its
  test and maintenance pages. `--json` for scripts, `--watch` to follow
  changes.
- `build/lilo2check FILE`: decodes a print stream and reports, for each
  page, its size and the share of black dots.

## Documentation

- [docs/STATUS_PROTOCOL.md](docs/STATUS_PROTOCOL.md): the status packet, the
  counter registers, the printer's job log, and what is known not to work,
  including the zero-length transfer above.
- [docs/MQTT.md](docs/MQTT.md): MQTT settings, topics, state fields and
  commands, the plug, and Home Assistant (discovery, entities, blueprint).

## License

GPL-2.0-or-later for the source files; the `sp211-printer-app` binary, linked
with PAPPL (Apache-2.0), is distributed under GPL-3.0-or-later. The encoder
comes from
[Creative8428/ricoh-sp211-driver](https://github.com/Creative8428/ricoh-sp211-driver);
see [NOTICE.md](NOTICE.md) for the origin and license of each part.
