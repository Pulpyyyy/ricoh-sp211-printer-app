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

#include "sp211_config.h"
#include "sp211_json.h"
#include "sp211_power.h"
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
  CHECK(!strcmp(c.firmware, "V1.06"));
  unsigned char odd[64];
  memcpy(odd, reg1, 64);
  odd[0x19] = 0x01;                                    /* not text: ignored */
  sp211_counters alt0;
  sp211_counters_init(&alt0);
  CHECK(sp211_decode_register1(odd, 64, &alt0) && alt0.firmware[0] == '\0');
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

static void test_internal_page(void) {
  unsigned char buf[64];
  size_t n = sp211_usb_internal_page_request(SP211_PAGE_TEST, buf, sizeof buf);
  /* the request Ricoh's tool sends, captured (docs/STATUS_PROTOCOL.md) */
  static const char want[] = "\x62\xff\x00@SDC_SET08TESTPAGE11GDITESTPAGE";
  CHECK(n == sizeof want - 1 && !memcmp(buf, want, n));
  n = sp211_usb_internal_page_request(SP211_PAGE_MAINTENANCE, buf, sizeof buf);
  CHECK(n == 3 + strlen("@SDC_SET08TESTPAGE14GDIMAINTENANCE") &&
        !memcmp(buf + 3, "@SDC_SET08TESTPAGE14GDIMAINTENANCE", n - 3));
  CHECK(sp211_usb_internal_page_request("SELFTEST", buf, sizeof buf) == 0); /* only these two */
  CHECK(sp211_usb_internal_page_request(NULL, buf, sizeof buf) == 0);
  CHECK(sp211_usb_internal_page_request(SP211_PAGE_TEST, buf, 10) == 0);    /* too small */
}

static void test_json(void) {
  sp211_buf b = {0};
  CHECK(!strcmp(sp211_buf_text(&b), ""));
  sp211_buf_cat(&b, "{\"name\":");
  sp211_buf_json_string(&b, "a\"b\\c\n\x01\xc3\xa9");
  sp211_buf_printf(&b, ",\"n\":%d,\"x\":", 42);
  sp211_buf_json_string(&b, NULL);
  sp211_buf_cat(&b, "}");
  CHECK(!b.failed);
  CHECK(!strcmp(sp211_buf_text(&b),
               "{\"name\":\"a\\\"b\\\\c\\n\\u0001\xc3\xa9\",\"n\":42,\"x\":null}"));
  for (int i = 0; i < 1000; i++) sp211_buf_cat(&b, "0123456789"); /* grows */
  CHECK(!b.failed && b.len == strlen(sp211_buf_text(&b)) && b.len > 10000);
  sp211_buf_free(&b);
  CHECK(b.s == NULL && b.len == 0);
}

static void test_power(void) {
  sp211_power p;
  time_t t = 1000000;
  sp211_power_init(&p);
  CHECK(p.auto_on && p.auto_off && p.delay_min == 15 && !p.linked && !p.on);
  CHECK(sp211_power_clamp_delay(0) == 1 && sp211_power_clamp_delay(500) == 240 &&
        sp211_power_clamp_delay(30) == 30);

  /* not linked: the switch only mirrors the printer, nothing is decided */
  CHECK(sp211_power_step(&p, true, false, t) && p.on);
  CHECK(sp211_power_step(&p, false, true, t + 1) && !p.on);    /* no auto on */
  CHECK(!sp211_power_step(&p, false, true, t + 2) && !p.on);
  CHECK(sp211_power_off_at(&p, false) == 0);

  /* Home Assistant links the plug: it is off */
  sp211_power_set(&p, false, t);
  CHECK(p.linked && !p.on);
  /* idle and off: stays off */
  CHECK(!sp211_power_step(&p, false, false, t + 10) && !p.on);
  /* a job arrives: auto on */
  CHECK(sp211_power_step(&p, false, true, t + 20) && p.on);
  CHECK(sp211_power_off_at(&p, true) == 0);                    /* never while busy */
  /* the printer boots and prints; still busy */
  CHECK(!sp211_power_step(&p, true, true, t + 60) && p.on);
  /* the job ends: idle period starts, off 15 min later */
  CHECK(!sp211_power_step(&p, true, false, t + 100) && p.on);
  CHECK(sp211_power_off_at(&p, false) == t + 100 + 15 * 60);
  CHECK(!sp211_power_step(&p, true, false, t + 100 + 15 * 60 - 1) && p.on);
  CHECK(sp211_power_step(&p, true, false, t + 100 + 15 * 60) && !p.on);
  CHECK(sp211_power_off_at(&p, false) == 0);
  /* the printer has not lost power yet: off stays off (the plug decides) */
  CHECK(!sp211_power_step(&p, true, false, t + 100 + 15 * 60 + 5) && !p.on);

  /* switched on by hand: a new idle period, not an immediate auto off */
  sp211_power_set(&p, true, t + 5000);
  CHECK(!sp211_power_step(&p, true, false, t + 5001) && p.on);
  CHECK(sp211_power_off_at(&p, false) == t + 5000 + 15 * 60);

  /* a job during the idle period restarts it */
  CHECK(!sp211_power_step(&p, true, true, t + 5100) && p.on);
  CHECK(!sp211_power_step(&p, true, false, t + 5200) && p.on);
  CHECK(sp211_power_off_at(&p, false) == t + 5200 + 15 * 60);

  /* auto off disabled: stays on */
  p.auto_off = false;
  CHECK(!sp211_power_step(&p, true, false, t + 99999) && p.on);
  CHECK(sp211_power_off_at(&p, false) == 0);

  /* auto on disabled: a job waits for someone to switch the printer on */
  sp211_power_set(&p, false, t + 100000);
  p.auto_on = false;
  CHECK(!sp211_power_step(&p, false, true, t + 100001) && !p.on);
}

static void test_broker(void) {
  sp211_broker b;
  const char *err;

  CHECK(sp211_parse_broker(NULL, NULL, NULL, &b, &err) == 0 && !err);
  CHECK(sp211_parse_broker("  ", NULL, NULL, &b, &err) == 0 && !err);

  CHECK(sp211_parse_broker("192.168.1.52", NULL, NULL, &b, &err) == 1);
  CHECK(!strcmp(b.host, "192.168.1.52") && b.port == 1883 && !b.tls);
  CHECK(sp211_parse_broker(" broker.lan:1884 ", NULL, NULL, &b, &err) == 1);
  CHECK(!strcmp(b.host, "broker.lan") && b.port == 1884 && !b.tls);
  CHECK(sp211_parse_broker("192.168.1.52", " 1885 ", NULL, &b, &err) == 1 && b.port == 1885);
  CHECK(sp211_parse_broker("192.168.1.52:1884", "1885", NULL, &b, &err) == 1 && b.port == 1885);
  CHECK(sp211_parse_broker("192.168.1.52", "", NULL, &b, &err) == 1 && b.port == 1883);

  /* schemes */
  CHECK(sp211_parse_broker("mqtts://ha.lan", NULL, NULL, &b, &err) == 1);
  CHECK(!strcmp(b.host, "ha.lan") && b.port == 8883 && b.tls);
  CHECK(sp211_parse_broker("MQTTS://ha.lan:9883/", NULL, NULL, &b, &err) == 1);
  CHECK(!strcmp(b.host, "ha.lan") && b.port == 9883 && b.tls);
  CHECK(sp211_parse_broker("mqtt://ha.lan", NULL, NULL, &b, &err) == 1 && b.port == 1883 && !b.tls);
  CHECK(sp211_parse_broker("ha.lan", NULL, "yes", &b, &err) == 1 && b.port == 8883 && b.tls);
  CHECK(sp211_parse_broker("mqtts://ha.lan", NULL, "no", &b, &err) == 1 && b.port == 1883 && !b.tls);
  CHECK(sp211_parse_broker("ha.lan", "8884", "true", &b, &err) == 1 && b.port == 8884 && b.tls);

  /* IPv6 */
  CHECK(sp211_parse_broker("[fd00::5]:1884", NULL, NULL, &b, &err) == 1);
  CHECK(!strcmp(b.host, "fd00::5") && b.port == 1884);
  CHECK(sp211_parse_broker("mqtts://[fd00::5]", NULL, NULL, &b, &err) == 1);
  CHECK(!strcmp(b.host, "fd00::5") && b.port == 8883 && b.tls);
  CHECK(sp211_parse_broker("fd00::5", NULL, NULL, &b, &err) == 1);
  CHECK(!strcmp(b.host, "fd00::5") && b.port == 1883);

  /* errors name the variable */
  CHECK(sp211_parse_broker("ha.lan:port", NULL, NULL, &b, &err) == -1 && !strcmp(err, "MQTT_HOST"));
  CHECK(sp211_parse_broker("ha.lan:0", NULL, NULL, &b, &err) == -1 && !strcmp(err, "MQTT_HOST"));
  CHECK(sp211_parse_broker("ha.lan:", NULL, NULL, &b, &err) == -1 && !strcmp(err, "MQTT_HOST"));
  CHECK(sp211_parse_broker(":1883", NULL, NULL, &b, &err) == -1 && !strcmp(err, "MQTT_HOST"));
  CHECK(sp211_parse_broker("http://ha.lan", NULL, NULL, &b, &err) == -1 && !strcmp(err, "MQTT_HOST"));
  CHECK(sp211_parse_broker("mqtt://", NULL, NULL, &b, &err) == -1 && !strcmp(err, "MQTT_HOST"));
  CHECK(sp211_parse_broker("[fd00::5", NULL, NULL, &b, &err) == -1 && !strcmp(err, "MQTT_HOST"));
  CHECK(sp211_parse_broker("ha.lan", "65536", NULL, &b, &err) == -1 && !strcmp(err, "MQTT_PORT"));
  CHECK(sp211_parse_broker("ha.lan", "18 83", NULL, &b, &err) == -1 && !strcmp(err, "MQTT_PORT"));
  CHECK(sp211_parse_broker("ha.lan", NULL, "maybe", &b, &err) == -1 && !strcmp(err, "MQTT_TLS"));

  CHECK(sp211_parse_bool("Yes", 0) == 1 && sp211_parse_bool("OFF", 1) == 0);
  CHECK(sp211_parse_bool(NULL, 1) == 1 && sp211_parse_bool(" ", 0) == 0);
  CHECK(sp211_parse_bool("2", 0) == -1);
}

int main(void) {
  from_hex(IDLE_HEX, IDLE, 64);
  from_hex(PAPER_OUT_HEX, PAPER_OUT, 64);
  test_decode();
  test_stream();
  test_counters();
  test_uri_serial();
  test_internal_page();
  test_json();
  test_power();
  test_broker();
  printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
