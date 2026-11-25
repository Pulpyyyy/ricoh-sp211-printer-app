#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Unit tests for ricoh_gdi_status, using packets captured from a real SP 211."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import ricoh_gdi_status as g

# Idle, paper present, toner 75 %
IDLE = bytes.fromhex(
    "47444952000000000000000000000000000000000000000000000000043a1307"
    "00000000000000100000005009090000010100004b0000000060000000000000")
# Job held by the printer, tray empty (red LED on the printer)
PAPER_OUT_JOB = bytes.fromhex(
    "474449520000000000000000000000000000000000000000000000008c961207"
    "00000100000000100000007009090000010100004b0000000060000000000000")


class DecodeTest(unittest.TestCase):
    def test_idle(self):
        st = g.decode_gdir(IDLE)
        self.assertEqual(st["toner_black_percent"], 75)
        self.assertFalse(st["paper_out"])
        self.assertEqual(st["jobs_pending"], 0)
        self.assertEqual(st["free_image_memory"], 0x07133A04)
        self.assertEqual(st["status_code"], 0)

    def test_paper_out_with_job(self):
        st = g.decode_gdir(PAPER_OUT_JOB)
        self.assertTrue(st["paper_out"])
        self.assertEqual(st["jobs_pending"], 1)
        self.assertEqual(st["free_image_memory"], 0x07133A04 - 41848)

    def test_invalid(self):
        self.assertIsNone(g.decode_gdir(b"GDIR" + b"\0" * 10))
        self.assertIsNone(g.decode_gdir(b"XXXX" + b"\0" * 60))

    def test_toner_out_of_range(self):
        p = bytearray(IDLE)
        p[0x34] = 200
        self.assertIsNone(g.decode_gdir(bytes(p))["toner_black_percent"])


class SplitTest(unittest.TestCase):
    def test_split_with_garbage_and_partial(self):
        buf = b"junk" + IDLE + PAPER_OUT_JOB + IDLE[:30]
        packets, rest = g.split_gdir(buf)
        self.assertEqual(packets, [IDLE, PAPER_OUT_JOB])
        self.assertEqual(rest, IDLE[:30])
        packets, rest = g.split_gdir(rest + IDLE[30:])
        self.assertEqual(packets, [IDLE])

    def test_marker_split_across_reads(self):
        packets, rest = g.split_gdir(b"zzGD")
        self.assertEqual(packets, [])
        packets, rest = g.split_gdir(rest + IDLE[2:])
        self.assertEqual(packets, [IDLE])


# Registers 1 and 3 as read from the printer (page counter 184, toner 75 %)
REG1 = bytes.fromhex(
    "4744494c014b000000000000000000000000000000000000"
    "56312e30360000000000000056312e3036000000b8000000"
    "00010100010101000101000001008000")
REG3 = bytes.fromhex("4744494c03140000" + "00" * 52 + "b8000000")


class RegisterTest(unittest.TestCase):
    def test_register_1(self):
        r = g.decode_registers(REG1, REG3)
        self.assertEqual(r["page_counter"], 184)
        self.assertEqual(r["toner_black_percent"], 75)
        self.assertEqual(r["firmware"], "V1.06")

    def test_counter_falls_back_to_register_3(self):
        self.assertEqual(g.decode_registers(None, REG3)["page_counter"], 184)

    def test_rejects_wrong_index_or_magic(self):
        self.assertIsNone(g.decode_registers(REG3, None)["page_counter"])
        self.assertIsNone(g.decode_registers(b"XXXX" + REG1[4:], None)["page_counter"])

    def test_build_status_without_gdir(self):
        st = g.build_status(None, g.decode_registers(REG1, REG3), "busy")
        self.assertEqual(st["page_counter"], 184)
        self.assertEqual(st["toner_black_percent"], 75)
        self.assertIsNone(st["paper_out"])
        self.assertIn("unknown", g.format_text(st))

    def test_build_status_with_gdir(self):
        st = g.build_status(IDLE, g.decode_registers(REG1, REG3))
        self.assertFalse(st["paper_out"])
        self.assertIn("Page counter: 184", g.format_text(st))


# Register 0x27 and two job-log registers as read from the printer (the
# identifier inside register 0x27 is replaced by a dummy). The
# maintenance page printed at the same time showed 6 misfeeds, all on tray 1.
REG27 = bytes.fromhex(
    "4744494c27000000000000000000000000000000000000000600000006000000"
    "000000000000000100af41303030423030303030300002000000000000000000")
REG10 = bytes.fromhex("4744494c100074342e747874" + "00" * 44 + "0100020000000000")
REG11 = bytes.fromhex("4744494c1100546573742050616765" + "00" * 41 + "0100020000000000")


class MisfeedTest(unittest.TestCase):
    def test_register_27(self):
        self.assertEqual(g.decode_misfeeds(REG27), {"misfeed_total": 6, "misfeed_tray1": 6})

    def test_missing_register(self):
        self.assertEqual(g.decode_misfeeds(None), {"misfeed_total": None, "misfeed_tray1": None})
        self.assertIsNone(g.decode_misfeeds(REG10)["misfeed_total"])


class JobLogTest(unittest.TestCase):
    def test_jobs(self):
        self.assertEqual(g.decode_job(REG10, 0x10),
                         {"title": "t4.txt", "user": "", "pages": 1, "result_code": 2})
        self.assertEqual(g.decode_job(REG11, 0x11)["title"], "Test Page")

    def test_empty_or_wrong_slot(self):
        empty = b"GDIL" + bytes([0x12]) + bytes(59)
        self.assertIsNone(g.decode_job(empty, 0x12))
        self.assertIsNone(g.decode_job(REG10, 0x11))

    def test_text_output(self):
        regs = g.decode_registers(REG1, REG3)
        regs.update(g.decode_misfeeds(REG27))
        regs["jobs"] = [g.decode_job(REG10, 0x10)]
        text = g.format_text(g.build_status(IDLE, regs))
        self.assertIn("Misfeeds    : 6 (tray 1: 6)", text)
        self.assertIn("t4.txt", text)

    def test_crashing_register_is_excluded(self):
        self.assertNotIn(0x2B, g.SAFE_REGISTERS)
        self.assertIn(g.REG_MISFEEDS, g.SAFE_REGISTERS)


class WatchTest(unittest.TestCase):
    def run_watch(self, readings):
        """readings: list of dicts, or an exception to raise for that round."""
        seen, asked = [], []
        it = iter(readings)

        def read(want_misfeeds):
            asked.append(want_misfeeds)
            r = next(it)
            if isinstance(r, Exception):
                raise r
            r = dict(r)
            if not want_misfeeds:
                r["misfeed_total"] = r["misfeed_tray1"] = None
            return r

        g.watch_changes(read, seen.append, 0, sleep=lambda s: None, rounds=len(readings))
        return seen, asked

    BASE = {"toner_black_percent": 75, "page_counter": 187, "firmware": "V1.06",
            "misfeed_total": 6, "misfeed_tray1": 6}

    def test_emits_once_when_nothing_changes(self):
        seen, asked = self.run_watch([self.BASE] * 4)
        self.assertEqual(len(seen), 1)
        self.assertEqual(seen[0]["page_counter"], 187)
        self.assertEqual(seen[0]["misfeed_total"], 6)
        self.assertEqual(asked, [True, False, False, False])

    def test_counter_change_emits_and_refreshes_misfeeds(self):
        after = dict(self.BASE, page_counter=188, misfeed_total=7, misfeed_tray1=7)
        seen, asked = self.run_watch([self.BASE, self.BASE, after, after, after])
        self.assertEqual([s["page_counter"] for s in seen], [187, 188, 188])
        self.assertEqual(seen[-1]["misfeed_total"], 7)
        self.assertEqual(asked, [True, False, False, True, False])

    def test_printer_off_then_back(self):
        seen, _ = self.run_watch([self.BASE, g.UsbError("gone"), g.UsbError("gone"), self.BASE])
        self.assertEqual([s["online"] for s in seen], [True, False, True])
        self.assertEqual(seen[1]["error"], "gone")
        self.assertEqual(seen[2]["misfeed_total"], 6)


class SdcCommandTest(unittest.TestCase):
    def test_matches_bytes_accepted_by_the_printer(self):
        self.assertEqual(g.sdc_command("TESTPAGE", "GDITESTPAGE"),
                         b"b\xff\x00@SDC_SET08TESTPAGE11GDITESTPAGE")
        self.assertEqual(g.sdc_command("TESTPAGE", g.INTERNAL_PAGES["maintenance"]),
                         b"b\xff\x00@SDC_SET08TESTPAGE14GDIMAINTENANCE")


if __name__ == "__main__":
    unittest.main()
