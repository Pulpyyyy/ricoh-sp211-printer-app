/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Unit tests for sp211_status. No printer needed: the packets below were
 * captured from a Ricoh SP 211 (firmware V1.06).
 *
 *   make test-status
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

static void test_reporter(void) {
  char text[2048] = {0};
  FILE *f = fmemopen(text, sizeof text - 1, "w");
  sp211_reporter r;
  sp211_status st;
  sp211_reporter_init(&r, f);

  sp211_decode_gdir(IDLE, 64, &st);
  sp211_report(&r, &st);
  CHECK(strstr(text, "marker-levels=75") != NULL);
  CHECK(strstr(text, "STATE: -toner-low-warning") != NULL);
  CHECK(strstr(text, "STATE: -media-empty-error") != NULL);
  size_t first = strlen(text);

  sp211_report(&r, &st);                             /* nothing changed */
  CHECK(strlen(text) == first);

  sp211_decode_gdir(PAPER_OUT, 64, &st);
  sp211_report(&r, &st);
  CHECK(strstr(text, "STATE: +media-empty-error") != NULL);
  CHECK(strstr(text + first, "ATTR:") == NULL);      /* toner unchanged */

  st.toner_percent = 8;
  sp211_report(&r, &st);
  CHECK(strstr(text, "marker-levels=8") != NULL);
  CHECK(strstr(text, "STATE: +toner-low-warning") != NULL);

  size_t before = strlen(text);
  st.toner_percent = -1;                             /* unknown: say nothing */
  sp211_report(&r, &st);
  CHECK(strlen(text) == before);
  fclose(f);
}

static void test_keep_waiting(void) {
  sp211_status idle, busy, nopaper;
  sp211_decode_gdir(IDLE, 64, &idle);
  busy = idle;
  busy.jobs_pending = 1;
  sp211_decode_gdir(PAPER_OUT, 64, &nopaper);

  CHECK(sp211_keep_waiting(5, 1, &idle, 0, 0));         /* printer not started yet */
  CHECK(sp211_keep_waiting(5, 1, NULL, 0, 0));
  CHECK(!sp211_keep_waiting(25, 1, &idle, 0, 0));       /* never started */
  CHECK(sp211_keep_waiting(120, 1, &busy, 1, 0));       /* printing */
  CHECK(!sp211_keep_waiting(120, 1, &idle, 1, 0));      /* finished */
  CHECK(sp211_keep_waiting(600, 1, &nopaper, 1, 0));    /* waiting for paper */
  CHECK(!sp211_keep_waiting(30, 16, &busy, 1, 0));      /* printer went silent */
  CHECK(!sp211_keep_waiting(SP211_WAIT_MAX_SECONDS + 1, 1, &nopaper, 1, 0));
  CHECK(sp211_keep_waiting(3600, 3600, NULL, 0, 1));     /* printer switched off */
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

  char text[96];
  CHECK(sp211_counters_message(&c, text, sizeof text) > 0);
  CHECK(!strcmp(text, "page-counter=184;misfeeds=6;misfeeds-tray1=6"));
  CHECK(strchr(text, ' ') == NULL);
  sp211_counters only_pages;
  sp211_counters_init(&only_pages);
  only_pages.have_pages = 1;
  only_pages.page_counter = 5;
  sp211_counters_message(&only_pages, text, sizeof text);
  CHECK(!strcmp(text, "page-counter=5"));
  sp211_counters none;
  sp211_counters_init(&none);
  CHECK(sp211_counters_message(&none, text, sizeof text) == 0 && text[0] == '\0');

  sp211_counters after = c;
  after.page_counter = 187;
  CHECK(sp211_pages_printed(&c, &after) == 3);
  CHECK(sp211_pages_printed(&c, &c) == 0);
  CHECK(sp211_pages_printed(&after, &c) == 0);          /* counter went back */
  CHECK(sp211_pages_printed(&none, &after) == 0);
  CHECK(sp211_pages_printed(NULL, &after) == 0);
}

static void test_reporter_counters(void) {
  char text[2048] = {0};
  FILE *f = fmemopen(text, sizeof text - 1, "w");
  sp211_reporter r;
  sp211_reporter_init(&r, f);
  unsigned char reg1[64], reg27[64];
  from_hex(REG1_HEX, reg1, 64);
  from_hex(REG27_HEX, reg27, 64);
  sp211_counters c;
  sp211_counters_init(&c);
  sp211_decode_register1(reg1, 64, &c);

  sp211_report_counters(&r, &c);                        /* toner comes from the register */
  CHECK(strstr(text, "marker-levels=75 marker-message=page-counter=184\n") != NULL);
  size_t first = strlen(text);
  sp211_report_counters(&r, &c);                        /* unchanged: nothing */
  CHECK(strlen(text) == first);

  sp211_status st;
  sp211_decode_gdir(IDLE, 64, &st);
  sp211_report(&r, &st);                                /* same toner: no new ATTR */
  CHECK(strstr(text + first, "ATTR:") == NULL);

  sp211_decode_misfeeds(reg27, 64, &c);
  c.page_counter = 185;
  sp211_report_counters(&r, &c);
  CHECK(strstr(text, "marker-message=page-counter=185;misfeeds=6;misfeeds-tray1=6\n") != NULL);

  sp211_report_pages(&r, 0);
  CHECK(strstr(text, "PAGE:") == NULL);
  sp211_report_pages(&r, 2);
  CHECK(strstr(text, "PAGE: total 2\n") != NULL);
  sp211_report_counters(&r, NULL);                      /* tolerated */
  fclose(f);

  /* without any toner value nothing can be reported */
  char text2[256] = {0};
  FILE *g = fmemopen(text2, sizeof text2 - 1, "w");
  sp211_reporter r2;
  sp211_reporter_init(&r2, g);
  sp211_counters none;
  sp211_counters_init(&none);
  none.have_pages = 1;
  sp211_report_counters(&r2, &none);
  CHECK(text2[0] == '\0');
  fclose(g);
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
  test_reporter();
  test_keep_waiting();
  test_counters();
  test_reporter_counters();
  test_uri_serial();
  printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
