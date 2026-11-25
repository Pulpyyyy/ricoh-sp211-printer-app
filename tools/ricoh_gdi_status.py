#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Status reporting for Ricoh GDI ("GDIC/GDIR") USB printers.

Verified on a Ricoh SP 211 (USB 05ca:0450, firmware V1.06). See
docs/GDI_STATUS_PROTOCOL.md for how the packet layout was established.

Two ways to use this file (no third-party dependencies):

1. Command line, while the printer is not printing:

       ricoh_gdi_status.py            # toner, page counter, paper, job
       ricoh_gdi_status.py --json     # machine readable

2. CUPS filter wrapper. Install a copy (or symlink) named ``ricohgdiwrap`` in
   the CUPS filter directory and point the PPD at it:

       *cupsFilter: "application/vnd.cups-raster 100 ricohgdiwrap"
       *ricohGdiRealFilter: "rastertolilo"

   The wrapper runs the real filter unchanged and, in parallel, decodes the
   64-byte GDIR packets the printer sends on the USB back-channel. It reports
   toner level and paper errors to CUPS (ATTR:/STATE: messages).
"""

import json
import os
import select
import subprocess
import sys
import time

GDIR_LEN = 64
GDIR_MAGIC = b"GDIR"
GDIC_QUERY = b"GDIC GDIR       "  # 16 bytes, what the Windows driver sends

RICOH_VID = 0x05CA
SP211_PID = 0x0450
EP_OUT, EP_IN, IFACE = 0x03, 0x83, 0

TONER_LOW_PERCENT = 10


# --------------------------------------------------------------------------
# Packet decoding
# --------------------------------------------------------------------------

def _le16(p, o):
    return p[o] | (p[o + 1] << 8)


def _le32(p, o):
    return p[o] | (p[o + 1] << 8) | (p[o + 2] << 16) | (p[o + 3] << 24)


def decode_gdir(packet):
    """Decode one 64-byte GDIR packet. Returns a dict, or None if invalid."""
    if len(packet) < GDIR_LEN or packet[:4] != GDIR_MAGIC:
        return None
    p = packet[:GDIR_LEN]
    toner = p[0x34]
    return {
        # Remaining black toner in percent; None if the value is out of range.
        "toner_black_percent": toner if toner <= 100 else None,
        # Set when the tray is pulled out or a sheet could not be fed.
        # An empty tray that is pushed in is NOT reported until a job tries
        # to feed from it.
        "paper_out": bool(p[0x2B] & 0x20),
        # 1 while the printer holds a job, back to 0 once it is printed.
        # Stays 1 for a job the printer refuses to start (e.g. unknown
        # paper size).
        "jobs_pending": _le16(p, 0x22),
        # Pages finished in the job currently printing.
        "job_pages_done": _le32(p, 0x10),
        "free_image_memory": _le32(p, 0x1C),
        # 0 normally, 7 in factory mode. The firmware writes nothing else
        # here, so printer errors cannot be read from it.
        "status_code": p[0x24],
        "raw": p.hex(),
    }


def split_gdir(buf):
    """Extract complete GDIR packets from a byte buffer.

    Returns (packets, remainder). Bytes before a GDIR marker are dropped.
    """
    packets = []
    while True:
        i = buf.find(GDIR_MAGIC)
        if i < 0:
            # keep a possible partial marker at the end
            return packets, buf[-(len(GDIR_MAGIC) - 1):]
        if len(buf) - i < GDIR_LEN:
            return packets, buf[i:]
        packets.append(buf[i:i + GDIR_LEN])
        buf = buf[i + GDIR_LEN:]


# --------------------------------------------------------------------------
# Direct USB query (command-line mode)
# --------------------------------------------------------------------------

class UsbError(Exception):
    pass


REGISTER_MAGIC = b"GDIL"
REQ_PREPARE, REQ_READ, REQ_WRITE = 0x60, 0x61, 0x62   # vendor control requests

# Values of the TESTPAGE setting that print one of the printer's own pages.
# Both were printed on an SP 211; "SELFTEST", used for other models, was not
# accepted.
INTERNAL_PAGES = {"test": "GDITESTPAGE", "maintenance": "GDIMAINTENANCE"}


def decode_registers(reg1, reg3=None):
    """Decode the vendor "GDIL" registers 1 and 3 (64 bytes each).

    Returns a dict; values are None when a register is missing or invalid.
    """
    out = {"page_counter": None, "toner_black_percent": None, "firmware": None}

    def valid(reg, index):
        return bool(reg) and len(reg) >= 64 and reg[:4] == REGISTER_MAGIC and reg[4] == index

    if valid(reg1, 1):
        out["page_counter"] = _le32(reg1, 0x2C)
        if reg1[5] <= 100:
            out["toner_black_percent"] = reg1[5]
        fw = bytes(reg1[0x18:0x20]).split(b"\0")[0]
        if fw and all(32 <= c < 127 for c in fw):
            out["firmware"] = fw.decode()
    if out["page_counter"] is None and valid(reg3, 3):
        out["page_counter"] = _le32(reg3, 0x3C)
    return out


REG_MISFEEDS = 0x27
REG_JOBLOG_FIRST, REG_JOBLOG_LAST = 0x10, 0x23
# Reading register 0x2B makes the SP 211 controller reboot. Never read
# registers outside this range without checking the firmware first.
SAFE_REGISTERS = range(0x01, 0x2B)


def _valid_register(reg, index):
    return bool(reg) and len(reg) >= 64 and reg[:4] == REGISTER_MAGIC and reg[4] == index


def _text(raw):
    raw = bytes(raw).split(b"\0")[0]
    return raw.decode("latin-1") if all(32 <= c < 127 for c in raw) else ""


def decode_misfeeds(reg):
    """Decode register 0x27 (paper misfeed counters)."""
    if not _valid_register(reg, REG_MISFEEDS):
        return {"misfeed_total": None, "misfeed_tray1": None}
    return {"misfeed_total": _le32(reg, 0x18), "misfeed_tray1": _le32(reg, 0x1C)}


def decode_job(reg, index):
    """Decode one job-log register (0x10..0x23). Returns None for an empty slot."""
    if not _valid_register(reg, index) or not any(reg[5:]):
        return None
    return {"title": _text(reg[0x06:0x24]), "user": _text(reg[0x24:0x38]),
            "pages": _le16(reg, 0x38), "result_code": _le16(reg, 0x3A)}


def query_usb(vid=RICOH_VID, pid=SP211_PID, attempts=4, jobs=False, claim=True,
              misfeeds=True):
    """Read the printer over USB.

    Returns (gdir_packet_or_None, registers_dict, note). The registers are
    read with control transfers and do not need the printer interface; the
    GDIR packet does, and is None when the interface is busy.
    """
    import ctypes as C
    import ctypes.util

    name = ctypes.util.find_library("usb-1.0") or "libusb-1.0.so.0"
    try:
        lib = C.CDLL(name)
    except OSError as e:
        raise UsbError("libusb-1.0 not found (%s)" % e)

    lib.libusb_open_device_with_vid_pid.restype = C.c_void_p
    lib.libusb_open_device_with_vid_pid.argtypes = [C.c_void_p, C.c_uint16, C.c_uint16]
    for f in ("libusb_claim_interface", "libusb_release_interface"):
        getattr(lib, f).argtypes = [C.c_void_p, C.c_int]
    lib.libusb_bulk_transfer.argtypes = [C.c_void_p, C.c_ubyte, C.c_void_p, C.c_int,
                                         C.POINTER(C.c_int), C.c_uint]
    lib.libusb_control_transfer.argtypes = [C.c_void_p, C.c_uint8, C.c_uint8, C.c_uint16,
                                            C.c_uint16, C.c_void_p, C.c_uint16, C.c_uint]
    lib.libusb_close.argtypes = [C.c_void_p]
    lib.libusb_exit.argtypes = [C.c_void_p]

    ctx = C.c_void_p()
    if lib.libusb_init(C.byref(ctx)) != 0:
        raise UsbError("libusb_init failed")
    try:
        h = lib.libusb_open_device_with_vid_pid(ctx, vid, pid)
        if not h:
            raise UsbError("printer %04x:%04x not found or not accessible "
                           "(powered on? run as root?)" % (vid, pid))
        try:
            def read_register(index, timeout=1.0):
                assert index in SAFE_REGISTERS
                two = C.create_string_buffer(2)
                lib.libusb_control_transfer(h, 0xC0, REQ_PREPARE, index, 0, two, 2, 1500)
                # The printer builds the reply asynchronously; until it is
                # ready the read returns zeros (or the previous register).
                deadline = time.time() + timeout
                while True:
                    time.sleep(0.15)
                    buf = C.create_string_buffer(64)
                    r = lib.libusb_control_transfer(h, 0xC0, REQ_READ, index, 0, buf, 64, 1500)
                    if r == 64 and _valid_register(buf.raw, index):
                        return buf.raw
                    if time.time() > deadline:
                        return None

            registers = decode_registers(read_register(1), read_register(3))
            # the misfeed register queries the print engine: about 3 s
            if misfeeds:
                registers.update(decode_misfeeds(read_register(REG_MISFEEDS, timeout=6.0)))
            if jobs:
                log = []
                for index in range(REG_JOBLOG_FIRST, REG_JOBLOG_LAST + 1):
                    job = decode_job(read_register(index), index)
                    if job is None:
                        break
                    log.append(job)
                registers["jobs"] = log

            if not claim:
                return None, registers, None
            r = lib.libusb_claim_interface(h, IFACE)
            if r != 0:
                return None, registers, ("printer interface busy (in use by CUPS?): "
                                         "paper and job state not read")
            try:
                buf = C.create_string_buffer(4096)
                n = C.c_int()
                data = b""
                for _ in range(attempts):
                    lib.libusb_bulk_transfer(h, EP_OUT, C.c_char_p(GDIC_QUERY),
                                             len(GDIC_QUERY), C.byref(n), 3000)
                    deadline = time.time() + 1.5
                    while time.time() < deadline:
                        n.value = 0
                        lib.libusb_bulk_transfer(h, EP_IN, buf, 4096, C.byref(n), 400)
                        if n.value:
                            data += buf.raw[:n.value]
                        elif data:
                            break
                    packets, _ = split_gdir(data)
                    if packets:
                        return packets[-1], registers, None
                return None, registers, "no GDIR reply from the printer"
            finally:
                lib.libusb_release_interface(h, IFACE)
        finally:
            lib.libusb_close(h)
    finally:
        lib.libusb_exit(ctx)


def sdc_command(name, value):
    """Build an @SDC_SET command the way Ricoh's Smart Organizing Monitor does."""
    text = "@SDC_SET%02d%s%02d%s" % (len(name), name, len(value), value)
    # The first three bytes mirror the Windows "vendor set command" buffer
    # (request, value high, value low); the firmware skips them.
    return b"\x62\xff\x00" + text.encode("ascii")


def send_sdc_command(name, value, vid=RICOH_VID, pid=SP211_PID):
    """Send one @SDC_SET command through vendor control request 0x62."""
    import ctypes as C
    import ctypes.util

    libname = ctypes.util.find_library("usb-1.0") or "libusb-1.0.so.0"
    try:
        lib = C.CDLL(libname)
    except OSError as e:
        raise UsbError("libusb-1.0 not found (%s)" % e)
    lib.libusb_open_device_with_vid_pid.restype = C.c_void_p
    lib.libusb_open_device_with_vid_pid.argtypes = [C.c_void_p, C.c_uint16, C.c_uint16]
    lib.libusb_control_transfer.argtypes = [C.c_void_p, C.c_uint8, C.c_uint8, C.c_uint16,
                                            C.c_uint16, C.c_void_p, C.c_uint16, C.c_uint]
    lib.libusb_close.argtypes = [C.c_void_p]
    lib.libusb_exit.argtypes = [C.c_void_p]

    data = sdc_command(name, value)
    ctx = C.c_void_p()
    if lib.libusb_init(C.byref(ctx)) != 0:
        raise UsbError("libusb_init failed")
    try:
        h = lib.libusb_open_device_with_vid_pid(ctx, vid, pid)
        if not h:
            raise UsbError("printer %04x:%04x not found or not accessible "
                           "(powered on? run as root?)" % (vid, pid))
        try:
            buf = C.create_string_buffer(data, len(data))
            r = lib.libusb_control_transfer(h, 0x41, REQ_WRITE, 0xFF00, 0, buf, len(data), 3000)
            if r != len(data):
                raise UsbError("printer refused the command (libusb result %d)" % r)
        finally:
            lib.libusb_close(h)
    finally:
        lib.libusb_exit(ctx)


def build_status(packet, registers, note=None):
    """Merge the GDIR packet and the registers into one status dict."""
    st = decode_gdir(packet) if packet else None
    if st is None:
        st = {"toner_black_percent": None, "paper_out": None, "jobs_pending": None,
              "job_pages_done": None, "free_image_memory": None, "status_code": None,
              "raw": None}
    if st["toner_black_percent"] is None:
        st["toner_black_percent"] = registers["toner_black_percent"]
    st["page_counter"] = registers["page_counter"]
    st["firmware"] = registers["firmware"]
    st["misfeed_total"] = registers.get("misfeed_total")
    st["misfeed_tray1"] = registers.get("misfeed_tray1")
    if "jobs" in registers:
        st["jobs"] = registers["jobs"]
    if note:
        st["note"] = note
    return st


def format_text(st):
    def show(v, fmt="%s"):
        return fmt % v if v is not None else "unknown"

    paper = {None: "unknown", True: "OUT (tray removed or feed failed)", False: "ok"}[st["paper_out"]]
    lines = [
        "Black toner : " + show(st["toner_black_percent"], "%d %%"),
        "Page counter: " + show(st["page_counter"], "%d"),
        "Paper       : " + paper,
        "Jobs pending: " + show(st["jobs_pending"], "%d"),
        "Misfeeds    : " + (show(st.get("misfeed_total"), "%d") +
                            (" (tray 1: %d)" % st["misfeed_tray1"]
                             if st.get("misfeed_tray1") is not None else "")),
        "Firmware    : " + show(st["firmware"]),
    ]
    for n, job in enumerate(st.get("jobs", []), 1):
        lines.append("Job %-2d      : %-30s %3d page(s)%s" % (
            n, job["title"] or "(untitled)", job["pages"],
            "  user " + job["user"] if job["user"] else ""))
    if st["jobs_pending"] and st["paper_out"]:
        lines.append("Action      : load paper, then press the Start key on the printer")
    if st.get("note"):
        lines.append("Note        : " + st["note"])
    return "\n".join(lines)


WATCH_KEYS = ("toner_black_percent", "page_counter", "misfeed_total", "misfeed_tray1",
              "firmware", "online")


def watch_changes(read, emit, interval, sleep=time.sleep, rounds=None):
    """Poll the printer and call emit(status) only when something changed.

    read(want_misfeeds) returns a status dict or raises UsbError. Only the
    registers are read (no printer interface), so CUPS jobs are not disturbed.
    The misfeed register queries the print engine, so it is read at start and
    then only after the page counter moved.
    """
    last = None
    misfeeds = {"misfeed_total": None, "misfeed_tray1": None}
    want_misfeeds = True
    n = 0
    while rounds is None or n < rounds:
        n += 1
        try:
            st = read(want_misfeeds)
            st["online"] = True
            if want_misfeeds:
                misfeeds = {k: st.get(k) for k in misfeeds}
            else:
                st.update(misfeeds)
        except UsbError as e:
            st = dict.fromkeys(WATCH_KEYS)
            st["online"] = False
            st["error"] = str(e)
        view = {k: st.get(k) for k in WATCH_KEYS}
        if not view["online"]:
            want_misfeeds = True    # read them again when the printer is back
        else:
            want_misfeeds = (last is not None and last["online"]
                             and view["page_counter"] != last["page_counter"])
        if view != last:
            emit(dict(view, **({"error": st["error"]} if "error" in st else {})))
            last = view
        sleep(interval)


def cli_main(argv):
    import argparse
    ap = argparse.ArgumentParser(description="Read status of a Ricoh GDI USB printer (SP 211).")
    ap.add_argument("--json", action="store_true", help="JSON output")
    ap.add_argument("--vid", type=lambda s: int(s, 16), default=RICOH_VID, help="USB vendor id (hex)")
    ap.add_argument("--pid", type=lambda s: int(s, 16), default=SP211_PID, help="USB product id (hex)")
    ap.add_argument("--registers-only", action="store_true",
                    help="do not take the printer interface: toner, counter and misfeeds "
                         "only, no paper or job state. Use this for periodic polling so "
                         "a CUPS job starting at the same moment is not disturbed")
    ap.add_argument("--watch", type=float, metavar="SECONDS", nargs="?", const=30.0,
                    help="keep running and print one JSON line each time toner, page "
                         "counter or misfeeds change (default: check every 30 s). "
                         "Reads registers only, so printing is not disturbed")
    ap.add_argument("--jobs", action="store_true",
                    help="also list the printer's job log (jobs since power-on)")
    ap.add_argument("--test-page", action="store_true",
                    help="print the printer's built-in test pattern (uses one sheet)")
    ap.add_argument("--maintenance-page", action="store_true",
                    help="print the maintenance page: counters, misfeeds, firmware "
                         "versions, supplies (uses one sheet)")
    args = ap.parse_args(argv)
    page = INTERNAL_PAGES["test"] if args.test_page else (
        INTERNAL_PAGES["maintenance"] if args.maintenance_page else None)
    if page:
        try:
            send_sdc_command("TESTPAGE", page, args.vid, args.pid)
        except UsbError as e:
            sys.stderr.write("error: %s\n" % e)
            return 2
        print("Page requested; printing starts after about 10 seconds.")
        return 0
    if args.watch is not None:
        def read(want_misfeeds):
            return build_status(*query_usb(args.vid, args.pid, claim=False,
                                           misfeeds=want_misfeeds))

        def emit(view):
            view["time"] = time.strftime("%Y-%m-%dT%H:%M:%S")
            print(json.dumps(view))
            sys.stdout.flush()

        try:
            watch_changes(read, emit, max(args.watch, 2.0))
        except KeyboardInterrupt:
            pass
        return 0
    try:
        st = build_status(*query_usb(args.vid, args.pid, jobs=args.jobs,
                                     claim=not args.registers_only))
    except UsbError as e:
        if args.json:
            print(json.dumps({"error": str(e)}))
        else:
            sys.stderr.write("error: %s\n" % e)
        return 2
    print(json.dumps(st) if args.json else format_text(st))
    return 0


# --------------------------------------------------------------------------
# CUPS filter wrapper mode
# --------------------------------------------------------------------------

BACKCHANNEL_FD = 3          # CUPS gives filters the back-channel on fd 3
LINGER_OK_SECONDS = 20      # how long to wait for the printer to start the job
LINGER_SILENCE_SECONDS = 15  # stop waiting if the printer stops talking


def counters_message(registers):
    """Text for the CUPS marker-message attribute, e.g.
    "page-counter=187;misfeeds=6;misfeeds-tray1=6". Empty if nothing is known.

    CUPS has no attribute for a lifetime page counter; marker-message is the
    only free-text field a filter can set, so the counters travel there in a
    key=value form that is easy to parse.
    """
    parts = []
    for key, label in (("page_counter", "page-counter"), ("misfeed_total", "misfeeds"),
                       ("misfeed_tray1", "misfeeds-tray1")):
        value = registers.get(key) if registers else None
        if value is not None:
            parts.append("%s=%d" % (label, value))
    return ";".join(parts)


def pages_printed(counter_before, counter_after):
    """Pages physically printed during a job, from the printer's own counter."""
    if counter_before is None or counter_after is None:
        return None
    delta = counter_after - counter_before
    return delta if 0 < delta <= 10000 else None


class CupsReporter:
    """Turns decoded status into CUPS ATTR:/STATE: messages, on change only."""

    def __init__(self, out=sys.stderr):
        self.out = out
        self.toner = None
        self.message = ""
        self.sent = None
        self.paper_out = None
        self.toner_low = None

    def _emit(self, line):
        self.out.write(line + "\n")
        self.out.flush()

    def _supplies(self):
        if self.toner is None or (self.toner, self.message) == self.sent:
            return
        self.sent = (self.toner, self.message)
        line = ("ATTR: marker-colors=#000000 marker-names='Black Toner' "
                "marker-types=toner marker-low-levels=%d marker-high-levels=100 "
                "marker-levels=%d" % (TONER_LOW_PERCENT, self.toner))
        if self.message:
            line += " marker-message=%s" % self.message
        self._emit(line)
        low = self.toner <= TONER_LOW_PERCENT
        if low != self.toner_low:
            self.toner_low = low
            self._emit("STATE: %stoner-low-warning" % ("+" if low else "-"))

    def update(self, st):
        """Status decoded from a GDIR packet."""
        if st["toner_black_percent"] is not None:
            self.toner = st["toner_black_percent"]
        self._supplies()
        if st["paper_out"] != self.paper_out:
            self.paper_out = st["paper_out"]
            if self.paper_out:
                self._emit("STATE: +media-empty-error")
                self._emit("INFO: Out of paper - load paper, then press Start on the printer")
            else:
                self._emit("STATE: -media-empty-error")
                self._emit("INFO: Printing")

    def update_registers(self, registers):
        """Counters (and toner, if not known yet) read from the registers."""
        if not registers:
            return
        if self.toner is None and registers.get("toner_black_percent") is not None:
            self.toner = registers["toner_black_percent"]
        message = counters_message(registers)
        if message:
            self.message = message
        self._supplies()

    def pages(self, count):
        """Job accounting: tells CUPS how many pages really came out."""
        if count:
            self._emit("PAGE: total %d" % count)


def read_ppd(path):
    """Minimal PPD scan: wrapped filter name, option defaults, page sizes."""
    info = {"filter": "rastertolilo", "ui": [], "defaults": {}, "pagesizes": set()}
    if not path:
        return info
    try:
        with open(path, "r", errors="replace") as f:
            for line in f:
                if line.startswith("*ricohGdiRealFilter:"):
                    name = line.split(":", 1)[1].strip().strip('"')
                    if name and "/" not in name:
                        info["filter"] = name
                elif line.startswith("*OpenUI *"):
                    key = line[len("*OpenUI *"):].split("/")[0].split(":")[0].strip()
                    if key and key not in info["ui"]:
                        info["ui"].append(key)
                elif line.startswith("*Default"):
                    key, _, val = line[len("*Default"):].partition(":")
                    info["defaults"][key.strip()] = val.strip()
                elif line.startswith("*PageSize "):
                    info["pagesizes"].add(line[len("*PageSize "):].split("/")[0].split(":")[0].strip())
    except OSError:
        pass
    return info


def complete_options(options, ppd):
    """Add PPD defaults for options the job does not set.

    Ricoh's rastertolilo fetches the PPD with cupsGetPPD(), which fails on
    current CUPS; it then sends an unknown paper size and no tray, and the
    printer holds the job until Start is pressed. Passing the defaults
    explicitly avoids that.
    """
    present = set()
    media = None
    for tok in options.split():
        key, sep, val = tok.partition("=")
        present.add(key)
        if key == "media" and sep:
            media = val
    extra = []
    if "PageSize" not in present and media:
        for part in media.split(","):
            if part in ppd["pagesizes"]:
                extra.append("PageSize=" + part)
                present.add("PageSize")
                break
    for key in ppd["ui"]:
        val = ppd["defaults"].get(key)
        if key not in present and val and val != "Unknown":
            extra.append("%s=%s" % (key, val))
    return " ".join([options] + extra).strip()


def backchannel_usable():
    try:
        os.fstat(BACKCHANNEL_FD)
        return True
    except OSError:
        return False


def pump_backchannel(reporter, buf, timeout):
    """Read what is available on the back-channel. Returns (buf, last_status, got_data)."""
    last = None
    got = False
    r, _, _ = select.select([BACKCHANNEL_FD], [], [], timeout)
    if r:
        try:
            chunk = os.read(BACKCHANNEL_FD, 4096)
        except OSError:
            chunk = b""
        if chunk:
            got = True
            packets, buf = split_gdir(buf + chunk)
            for p in packets:
                last = decode_gdir(p)
                if last:
                    reporter.update(last)
    return buf, last, got


def read_registers_quietly(misfeeds=False):
    """Registers for the filter: None when the printer cannot be read (off,
    no permission...). Never raises: status must not break printing."""
    try:
        return query_usb(claim=False, misfeeds=misfeeds)[1]
    except Exception:
        return None


JOB_MAX_SECONDS = 1800
POWER_PROBE_SECONDS = 5


def keep_waiting(elapsed, silence, status, seen_active, waiting_for_power=False):
    """After the data is sent: should the filter stay alive?

    Staying alive keeps the CUPS job "processing" until the printer is really
    done, so that a paper error stays visible and the page counter read at
    the end is final.
    """
    if waiting_for_power:
        return True                     # printer is off: CUPS is holding the job
    paper_out = bool(status and status["paper_out"])
    pending = bool(status and status["jobs_pending"])
    if silence > LINGER_SILENCE_SECONDS:
        return False                    # printer stopped talking (off, unplugged)
    if elapsed > JOB_MAX_SECONDS:
        return False
    if paper_out or pending:
        return True                     # job held or printing
    if seen_active:
        return False                    # was printing, now finished
    return elapsed <= LINGER_OK_SECONDS  # give the printer time to start


def filter_main(argv):
    if len(argv) not in (6, 7):
        sys.stderr.write("ERROR: usage: %s job user title copies options [file]\n" % argv[0])
        return 1
    ppd = read_ppd(os.environ.get("PPD"))
    name = ppd["filter"]
    args = list(argv[1:])
    args[4] = complete_options(args[4], ppd)
    serverbin = os.environ.get("CUPS_SERVERBIN", "/usr/lib/cups")
    path = os.path.join(serverbin, "filter", name)

    reporter = CupsReporter()
    # Counter before the job. None if the printer is still off; it is then
    # read as soon as the printer starts answering.
    before = read_registers_quietly()
    reporter.update_registers(before)

    try:
        child = subprocess.Popen([path] + args, executable=path)
    except OSError as e:
        sys.stderr.write("ERROR: cannot run filter %s: %s\n" % (path, e))
        return 1

    if not backchannel_usable():
        return child.wait()

    buf = b""
    status = None
    seen_active = False

    def pump(timeout):
        nonlocal buf, status, before, seen_active
        buf, last, got = pump_backchannel(reporter, buf, timeout)
        if got and before is None:
            before = read_registers_quietly()
            reporter.update_registers(before)
        status = last or status
        if status and status["jobs_pending"]:
            seen_active = True
        return got

    while child.poll() is None:
        pump(0.5)
    rc = child.returncode
    if rc != 0:
        return rc

    # If the printer has never answered, it is switched off and the CUPS
    # backend is waiting for it. Wait with it (the filter is stopped by CUPS
    # if the job is cancelled), then start timing once it is on.
    heard = status is not None
    start = last_data = time.time()
    next_probe = 0.0
    while True:
        if pump(1.0):
            last_data = time.time()
            if not heard:
                heard = True
                start = last_data
        now = time.time()
        waiting_for_power = False
        if not heard and before is None:
            if now >= next_probe:
                next_probe = now + POWER_PROBE_SECONDS
                before = read_registers_quietly()
                reporter.update_registers(before)
            if before is None:
                waiting_for_power = True
            else:
                start = last_data = now     # printer just came on
        if not keep_waiting(now - start, now - last_data, status, seen_active,
                            waiting_for_power):
            break

    after = read_registers_quietly(misfeeds=True)
    reporter.update_registers(after)
    if before and after:
        reporter.pages(pages_printed(before.get("page_counter"), after.get("page_counter")))
    return 0


def main():
    if os.path.basename(sys.argv[0]).startswith("ricohgdiwrap"):
        return filter_main(sys.argv)
    return cli_main(sys.argv[1:])


if __name__ == "__main__":
    sys.exit(main())
