/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Unit tests for sp211_status. No printer needed: the packets below were
 * captured from a Ricoh SP 211 (firmware V1.06).
 *
 *   make test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sp211_status.h"
#include "sp211_usb.h"

static int failures, checks;
#define CHECK(cond)                                                     \
  do {                                                                  \
    checks++;                                                           \
    if (!(cond)) {                                                      \
      failures++;                                                       \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
    }                                                                   \
  } while (0)

static void from_hex(const char *hex, unsigned char *out, size_t n) {
  for (size_t i = 0; i < n; i++) {
    unsigned v;
    sscanf(hex + 2 * i, "%2x", &v);
    out[i] = (unsigned char)v;
  }
}

/* idle, paper present, toner 75 % */
static const char IDLE_HEX[] =
    "47444952000000000000000000000000000000000000000000000000043a1307"
    "00000000000000100000005009090000010100004b0000000060000000000000";
/* job held by the printer, tray empty (red LED) */
static const char PAPER_OUT_HEX[] =
    "474449520000000000000000000000000000000000000000000000008c961207"
    "00000100000000100000007009090000010100004b0000000060000000000000";

static unsigned char IDLE[64], PAPER_OUT[64];

static void test_decode(void) {
  sp211_status st;
  CHECK(sp211_decode_gdir(IDLE, 64, &st));
  CHECK(st.toner_percent == 75);
  CHECK(!st.paper_out);
  CHECK(st.jobs_pending == 0);
  CHECK(st.free_memory == 0x07133a04);
  CHECK(st.status_code == 0);

  CHECK(sp211_decode_gdir(PAPER_OUT, 64, &st));
  CHECK(st.paper_out);
  CHECK(st.jobs_pending == 1);
  CHECK(st.free_memory == 0x07133a04 - 41848);

  CHECK(!sp211_decode_gdir(IDLE, 63, &st));          /* too short */
  CHECK(!sp211_decode_gdir(NULL, 64, &st));
  unsigned char bad[64];
  memcpy(bad, IDLE, 64);
  bad[0] = 'X';
  CHECK(!sp211_decode_gdir(bad, 64, &st));           /* wrong marker */
  memcpy(bad, IDLE, 64);
  bad[0x34] = 200;
  CHECK(sp211_decode_gdir(bad, 64, &st) && st.toner_percent == -1);
}

typedef struct { int n; sp211_status last; } seen_t;
static void on_status(const sp211_status *st, void *arg) {
  seen_t *s = arg;
  s->n++;
  s->last = *st;
}

static void test_stream(void) {
  sp211_stream s = {0};
  seen_t seen = {0};
  unsigned char buf[4 + 64 + 64 + 30];
  memcpy(buf, "junk", 4);
  memcpy(buf + 4, IDLE, 64);
  memcpy(buf + 68, PAPER_OUT, 64);
  memcpy(buf + 132, IDLE, 30);                       /* partial third packet */
  CHECK(sp211_stream_feed(&s, buf, sizeof buf, on_status, &seen) == 2);
  CHECK(seen.n == 2 && seen.last.paper_out);
  CHECK(sp211_stream_feed(&s, IDLE + 30, 34, on_status, &seen) == 1);
  CHECK(seen.n == 3 && !seen.last.paper_out);

  /* one byte at a time, marker split across reads, stray 'G' before it */
  sp211_stream t = {0};
  seen_t one = {0};
  int found = sp211_stream_feed(&t, (const unsigned char *)"zzGG", 4, on_status, &one);
  for (int i = 1; i < 64; i++) found += sp211_stream_feed(&t, IDLE + i, 1, on_status, &one);
  CHECK(found == 1 && one.n == 1 && one.last.toner_percent == 75);

  /* NULL callback is allowed */
  sp211_stream u = {0};
  CHECK(sp211_stream_feed(&u, IDLE, 64, NULL, NULL) == 1);
}

/* Registers read from the printer: counter 184, toner 75 %, 6 misfeeds (the
 * maintenance page printed at the time showed the same values). The
 * identifier inside register 0x27 is replaced by a dummy. */
static const char REG1_HEX[] =
    "4744494c014b000000000000000000000000000000000000"
    "56312e30360000000000000056312e3036000000b8000000"
    "00010100010101000101000001008000";
static const char REG27_HEX[] =
    "4744494c27000000000000000000000000000000000000000600000006000000"
    "000000000000000100af41303030423030303030300002000000000000000000";

static void test_counters(void) {
  unsigned char reg1[64], reg27[64], reg3[64] = {'G', 'D', 'I', 'L', 3};
  from_hex(REG1_HEX, reg1, 64);
  from_hex(REG27_HEX, reg27, 64);
  reg3[0x3c] = 0xb8;

  sp211_counters c;
  sp211_counters_init(&c);
  CHECK(!c.have_pages && c.toner_percent == -1);
  CHECK(sp211_decode_register1(reg1, 64, &c));
  CHECK(c.have_pages && c.page_counter == 184 && c.toner_percent == 75);
  CHECK(sp211_decode_misfeeds(reg27, 64, &c));
  CHECK(c.have_misfeeds && c.misfeed_total == 6 && c.misfeed_tray1 == 6);

  sp211_counters alt;
  sp211_counters_init(&alt);
  CHECK(sp211_decode_register3(reg3, 64, &alt) && alt.page_counter == 184);
  CHECK(!sp211_decode_register1(reg3, 64, &alt));      /* wrong register */
  CHECK(!sp211_decode_register1(NULL, 64, &alt));
  CHECK(!sp211_decode_register1(reg1, 63, &alt));
  CHECK(!sp211_decode_misfeeds(reg1, 64, &alt));
}

static void test_uri_serial(void) {
  char serial[32];
  CHECK(sp211_usb_uri_serial("usb://RICOH/SP%20211?serial=XY1Z234A", serial, sizeof serial));
  CHECK(!strcmp(serial, "XY1Z234A"));
  CHECK(sp211_usb_uri_serial("usb://RICOH/SP%20211?serial=AB%20C&interface=1", serial, sizeof serial));
  CHECK(!strcmp(serial, "AB C"));
  CHECK(!sp211_usb_uri_serial("usb://RICOH/SP%20211", serial, sizeof serial));
  CHECK(!sp211_usb_uri_serial(NULL, serial, sizeof serial));
  CHECK(sp211_usb_uri_serial("usb://x?serial=1234567890", serial, 5) && !strcmp(serial, "1234"));
  /* built without libusb here: the reader reports "not reachable" */
  sp211_counters c;
  c.have_pages = 1;
  CHECK(sp211_usb_read_counters(NULL, 1, &c) == 0 || c.have_pages);
}

int main(void) {
  from_hex(IDLE_HEX, IDLE, 64);
  from_hex(PAPER_OUT_HEX, PAPER_OUT, 64);
  test_decode();
  test_stream();
  test_counters();
  test_uri_serial();
  printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
