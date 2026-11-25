# ricoh-sp211-printer-app

A CUPS driver for the Ricoh SP 211 laser printer on Linux, that also tells
CUPS what the printer says about itself.

- **Prints without Ricoh's software**: a native filter, `rastertosp211`.
- **Reports to CUPS at every job**: toner level, out-of-paper, the printer's
  lifetime page counter and misfeed counters, and the number of pages that
  really came out.
- **Copes with a printer that is switched off between jobs**: CUPS holds the
  job and the driver waits with it.

Other printers of the same family (SP 210, SP 213, SP 220, SP 221) use the
same print format and may work; only the SP 211 has been tested.

## Status

Tested on one printer: Ricoh SP 211, firmware V1.06, Debian 13, CUPS 2.4.

| | State |
|---|---|
| Printing through the native filter | verified on paper |
| Paper size and tray taken from the queue defaults | verified on paper |
| Toner level in CUPS | verified |
| Page counter, misfeeds and pages per job, read by the C filter | verified in a dry run (replayed status, real counter reads); **not yet with a real job** |
| Out-of-paper reported during a job | replayed packets only |
| Printer switched off when the job is submitted | verified with an earlier Python version of this code; **not yet with the C filter** |
| Cover open, jams in progress | not available: the printer does not transmit them |

## Install

```
sudo apt install build-essential libcups2-dev libusb-1.0-0-dev
make
make test
sudo make install
sudo lpadmin -p SP211 -E -v "$(lpinfo -v | grep -i 'usb://RICOH' | cut -d' ' -f2)" \
     -P /usr/share/cups/model/ricoh-sp211.ppd
```

Without `libusb-1.0-0-dev` the driver still builds and prints; it then does
not report the page and misfeed counters.

## What CUPS shows

| Information | Where |
|---|---|
| toner level | `marker-levels`, and `toner-low-warning` at 10 % or less |
| out of paper | `media-empty-error` |
| page counter and misfeeds | `marker-message`, for example `page-counter=193;misfeeds=6;misfeeds-tray1=6` |
| pages printed by a job | job accounting (`job-impressions-completed`) |

`marker-message` is free text: CUPS has no attribute for a lifetime counter,
and it is the only free field a filter can set. Values are refreshed when a
job runs; CUPS keeps the last ones while the printer is off.

## Printer Application (in progress)

`sp211-printer-app` is an IPP Everywhere printer of its own, built on
[PAPPL](https://www.msweet.org/pappl/): Windows, macOS, iOS, Android and
Home Assistant can talk to it without any driver, and CUPS can use it as a
driverless printer. It is the direction CUPS takes: CUPS 2.4 warns that
printer drivers such as the PPD above "are deprecated and will stop working
in a future version of CUPS".

To try it next to CUPS, on another port:

```
sudo apt install build-essential libpappl-dev
make printer-app
build/sp211-printer-app server -o server-port=8632 -o spool-directory=DIR
build/sp211-printer-app add -d SP211 -m ricoh_sp211 -v DEVICE-URI
```

`build/sp211-printer-app devices` lists the printers found on USB.

### On a machine of its own (Debian 13, for example a Proxmox LXC)

The application then replaces CUPS for this printer and takes the IPP port,
631. Only one program at a time may drive the printer over USB.

```
sudo apt install build-essential libpappl-dev avahi-daemon
make printer-app
sudo make install-printer-app
sudo systemctl enable --now sp211-printer-app
sudo sp211-printer-app add -d SP211 -m ricoh_sp211 \
     -v "$(sp211-printer-app devices | grep -m1 -o 'usb://RICOH[^ ]*')"
```

Its data (queue, settings) is kept in `/var/lib/sp211-printer-app`, and its
web interface is at `http://HOST:631/`.

In a Proxmox LXC container, the printer's USB device must be passed to the
container. In `/etc/pve/lxc/ID.conf` on the Proxmox host:

```
lxc.cgroup2.devices.allow: c 189:* rwm
lxc.mount.entry: /dev/bus/usb dev/bus/usb none bind,optional,create=dir
```

189 is the device number of USB devices. In an unprivileged container the
device files also need permissions that the container's root can use. The
service and this LXC setup have **not been tested yet**.

| | State |
|---|---|
| PWG raster and Apple raster (AirPrint) in, A4 and Letter | same image bands as the CUPS filter, byte for byte, written to a file |
| Paper size taken from the page received | verified, written to a file |
| Copies | passed to the printer, written to a file |
| Printing on the printer | verified: one A4 page sent from the server, counter 194 -> 195 |
| Printing from Windows (IPP Class Driver, gzip over IPPS) | verified on paper |
| Heavy page (gray, about 350 KB of printer data) | verified on paper, no zero-length USB transfer |
| Toner level (`printer-supply`, `marker-levels`) and toner-low | verified on the printer: 50 % |
| Page counter as `printer-impressions-completed` | verified: 195, the printer's own counter |
| Offline when the printer cannot be reached | not tested |
| Out of paper | not yet: the printer only reports it during a job |
| 1200×600 dpi, manual duplex | not supported yet |

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

- `build/lilo2check FILE`: decodes a print stream and reports, for each
  page, its size and the share of black dots. Use it to check a stream
  before sending it to a printer: a wrong page (for example an inverted,
  almost black one) can leave the printer refusing all data until it is
  switched off and on.
- `tools/ricoh_gdi_status.py`: reads the printer directly (toner, counters,
  paper state), and can ask it to print its test and maintenance pages.

## Documentation

- [docs/STATUS_PROTOCOL.md](docs/STATUS_PROTOCOL.md): the status packet, the
  register channel, and what is known not to work. It was written for an
  earlier Python version of this code and still describes its commands; the
  protocol itself is unchanged.

## License

GPL-2.0-or-later. The print stream encoder and the filter come from
[Creative8428/ricoh-sp211-driver](https://github.com/Creative8428/ricoh-sp211-driver);
see [NOTICE.md](NOTICE.md) for the origin and license of each part.
