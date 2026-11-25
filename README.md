# ricoh-sp211-printer-app

An IPP Everywhere printer for the Ricoh SP 211 on Linux, built on
[PAPPL](https://www.msweet.org/pappl/). Windows, macOS, iOS, Android and
Home Assistant print to it and read its status without any driver.

- **Prints without Ricoh's software**: pages are converted to the printer's
  own "lilo2" stream by the encoder of
  [Creative8428/ricoh-sp211-driver](https://github.com/Creative8428/ricoh-sp211-driver).
- **Reports the printer's state over IPP**: toner level (`printer-supply`,
  `marker-levels`), toner low, and the printer's own page counter
  (`printer-impressions-completed`).
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
sudo apt install build-essential libpappl-dev libusb-1.0-0-dev avahi-daemon
make
make test
sudo make install
sudo systemctl enable --now sp211-printer-app
sudo sp211-printer-app add -d SP211 -m ricoh_sp211 \
     -v "$(sp211-printer-app devices | grep -m1 -o 'usb://RICOH[^ ]*')"
```

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

### Home Assistant

Home Assistant's IPP integration finds the printer by itself, or can be
pointed at host `HOST`, port `631`, path `/ipp/print/SP211`. It shows the
printer state and the page counter ("impressions"). The toner level is
published too; its sensor has not been checked in Home Assistant yet, and
may need a reload of the integration after the first status read.

## Status

Tested on one printer: Ricoh SP 211, firmware V1.06, Debian 13, PAPPL 1.3.1.

| | State |
|---|---|
| Printing from Windows (IPP Class Driver, gzip over IPPS) | verified on paper |
| PWG raster and Apple raster (AirPrint) in, A4 and Letter | verified: same image bands as Ricoh's driver, byte for byte |
| Heavy page (gray, about 350 KB of printer data) | verified on paper |
| Paper size taken from the page received; copies | verified |
| Toner level and toner low | verified: 50 % |
| Page counter as `printer-impressions-completed` | verified |
| Offline when the printer cannot be reached | not tested |
| Out of paper | not yet: the printer only reports it during a job |
| 1200×600 dpi, manual duplex | not supported yet |
| Cover open, jams in progress | not available: the printer does not transmit them |

### Known limits

Data is written to the printer in pieces of 2 KB. PAPPL's papplDeviceWrite()
sends a zero-length USB transfer when given more than its 8 KB buffer while
the buffer is empty; the SP 211 then keeps the job without printing it and
refuses all further data until it is switched off and on. PAPPL fixed this
after 1.4.12 ([issue #434](https://github.com/michaelrsweet/pappl/issues/434));
no release contains the fix yet, and Debian 13 ships 1.3.1.

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

## License

GPL-2.0-or-later for the source files; the `sp211-printer-app` binary, linked
with PAPPL (Apache-2.0), is distributed under GPL-3.0-or-later. The encoder
comes from
[Creative8428/ricoh-sp211-driver](https://github.com/Creative8428/ricoh-sp211-driver);
see [NOTICE.md](NOTICE.md) for the origin and license of each part.
