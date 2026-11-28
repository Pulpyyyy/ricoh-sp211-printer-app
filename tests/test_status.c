/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Unit tests of the parts that need no PAPPL. No printer needed: the packets
 * below were captured from a Ricoh SP 211 (firmware V1.06).
 *
 *   make test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sp211_config.h"
#include "sp211_json.h"
#include "sp211_power.h"
#include "sp211_state.h"
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

/* The JSON string written for text. */
static int json_is(const char *text, const char *want) {
  sp211_buf b = {0};
  sp211_buf_json_string(&b, text);
  int ok = !b.failed && !strcmp(sp211_buf_text(&b), want) && b.len == strlen(want);
  if (!ok) fprintf(stderr, "got %s, want %s\n", sp211_buf_text(&b), want);
  sp211_buf_free(&b);
  return ok;
}

static void test_json_strings(void) {
  CHECK(json_is("", "\"\""));
  CHECK(json_is("invoice.pdf", "\"invoice.pdf\""));
  CHECK(json_is("\"", "\"\\\"\""));
  CHECK(json_is("\\", "\"\\\\\""));
  CHECK(json_is("C:\\a \"b\".pdf", "\"C:\\\\a \\\"b\\\".pdf\""));
  CHECK(json_is("\\n", "\"\\\\n\""));               /* two characters, not a newline */
  CHECK(json_is("a/b 'c'", "\"a/b 'c'\""));          /* nothing to escape */
  CHECK(json_is("\n\r\t", "\"\\n\\r\\t\""));
  CHECK(json_is("\b\f\x1b\x1f", "\"\\u0008\\u000c\\u001b\\u001f\""));
  CHECK(json_is(" \x7f", "\" \x7f\""));              /* not control characters in JSON */

  /* every control character is escaped */
  for (int c = 1; c < 0x20; c++) {
    char in[2] = {(char)c, '\0'};
    sp211_buf b = {0};
    sp211_buf_json_string(&b, in);
    const unsigned char *p = (const unsigned char *)sp211_buf_text(&b);
    while (*p >= 0x20) p++;
    CHECK(!*p && b.len >= 4 && sp211_buf_text(&b)[1] == '\\');
    sp211_buf_free(&b);
  }

  /* UTF-8 of 2, 3 and 4 bytes is kept, and so are invalid bytes */
  CHECK(json_is("\xc3\xa9t\xc3\xa9 \xe2\x82\xac \xf0\x9f\x96\xa8",
                "\"\xc3\xa9t\xc3\xa9 \xe2\x82\xac \xf0\x9f\x96\xa8\""));
  CHECK(json_is("\xff\xc3", "\"\xff\xc3\""));
}

static void test_json_objects(void) {
  sp211_buf b = {0};
  sp211_buf_cat(&b, NULL);                           /* nothing appended */
  CHECK(!b.failed && b.len == 0 && !strcmp(sp211_buf_text(&b), ""));

  /* nested object and list, appended piece by piece */
  sp211_buf_printf(&b, "{\"id\":%d,\"name\":", 12);
  sp211_buf_json_string(&b, "a \"b\"");
  sp211_buf_cat(&b, ",\"list\":[");
  sp211_buf_cat(&b, "]}");
  CHECK(!strcmp(sp211_buf_text(&b), "{\"id\":12,\"name\":\"a \\\"b\\\"\",\"list\":[]}"));
  sp211_buf_free(&b);

  /* a freed buffer can be used again; one printf larger than the first
   * allocation, after some text */
  char long_text[1001];
  memset(long_text, 'x', 1000);
  long_text[1000] = '\0';
  sp211_buf_cat(&b, "[");
  sp211_buf_printf(&b, "\"%s\"", long_text);
  sp211_buf_cat(&b, "]");
  CHECK(!b.failed && b.len == 1004 && strlen(sp211_buf_text(&b)) == 1004);
  CHECK(!strncmp(sp211_buf_text(&b), "[\"xx", 4) && !strcmp(sp211_buf_text(&b) + 1000, "xx\"]"));
  sp211_buf_free(&b);

  /* after a failure nothing more is appended */
  sp211_buf_cat(&b, "{");
  b.failed = true;
  sp211_buf_cat(&b, "a");
  sp211_buf_printf(&b, "%d", 1);
  sp211_buf_json_string(&b, "b");
  CHECK(b.failed && !strcmp(sp211_buf_text(&b), "{"));
  sp211_buf_free(&b);
  CHECK(!b.failed);
}

/* The fields of the state message built without PAPPL (docs/MQTT.md, State). */
static void test_state(void) {
  CHECK(!strcmp(sp211_state_toner(-1), "unknown"));
  CHECK(!strcmp(sp211_state_toner(0), "empty"));
  CHECK(!strcmp(sp211_state_toner(1), "low"));
  CHECK(!strcmp(sp211_state_toner(10), "low"));
  CHECK(!strcmp(sp211_state_toner(11), "ok"));
  CHECK(!strcmp(sp211_state_toner(100), "ok"));

  /* nothing read yet: numbers are null */
  sp211_app_status st = {.toner_percent = -1};
  sp211_buf b = {0};
  sp211_state_supplies(&b, &st);
  CHECK(!strcmp(sp211_buf_text(&b),
                ",\"toner\":null,\"toner_state\":\"unknown\",\"pages_total\":null,\"misfeeds\":null"));
  sp211_buf_free(&b);

  /* the values of the example in docs/MQTT.md */
  st.toner_percent = 9;
  st.have_pages = true;
  st.pages = 205;
  st.have_misfeeds = true;
  st.misfeeds = 6;
  sp211_state_supplies(&b, &st);
  CHECK(!strcmp(sp211_buf_text(&b),
                ",\"toner\":9,\"toner_state\":\"low\",\"pages_total\":205,\"misfeeds\":6"));
  sp211_buf_free(&b);

  /* zero is a value, not null; the counters are unsigned */
  st.toner_percent = 0;
  st.pages = 4294967295u;
  st.misfeeds = 0;
  sp211_state_supplies(&b, &st);
  CHECK(!strcmp(sp211_buf_text(&b),
                ",\"toner\":0,\"toner_state\":\"empty\",\"pages_total\":4294967295,\"misfeeds\":0"));
  sp211_buf_free(&b);

  char t[40];
  sp211_state_time(0, t, sizeof t);
  CHECK(!strcmp(t, "1970-01-01T00:00:00+00:00"));
  const time_t off = 1795637778; /* 2026-11-25T20:16:18 UTC */
  sp211_state_time(off, t, sizeof t);
  CHECK(!strcmp(t, "2026-11-25T20:16:18+00:00"));

  /* no plug linked: off_at is null */
  sp211_power p;
  sp211_power_init(&p);
  sp211_state_power(&b, &p, false);
  CHECK(!strcmp(sp211_buf_text(&b),
                ",\"power\":\"OFF\",\"linked\":false,\"auto_on\":\"ON\",\"auto_off\":\"ON\","
                "\"delay\":15,\"off_at\":null"));
  sp211_buf_free(&b);

  /* linked, on and idle: the example in docs/MQTT.md */
  sp211_power_set(&p, true, off - 15 * 60);
  sp211_power_step(&p, true, false, off - 15 * 60);
  sp211_state_power(&b, &p, false);
  CHECK(!strcmp(sp211_buf_text(&b),
                ",\"power\":\"ON\",\"linked\":true,\"auto_on\":\"ON\",\"auto_off\":\"ON\","
                "\"delay\":15,\"off_at\":\"2026-11-25T20:16:18+00:00\""));
  sp211_buf_free(&b);

  /* busy: no auto off; settings changed */
  p.auto_on = false;
  p.delay_min = 240;
  sp211_state_power(&b, &p, true);
  CHECK(!strcmp(sp211_buf_text(&b),
                ",\"power\":\"ON\",\"linked\":true,\"auto_on\":\"OFF\",\"auto_off\":\"ON\","
                "\"delay\":240,\"off_at\":null"));
  sp211_buf_free(&b);
  p.auto_off = false;
  sp211_state_power(&b, &p, false);
  CHECK(strstr(sp211_buf_text(&b), "\"auto_off\":\"OFF\"") &&
        strstr(sp211_buf_text(&b), "\"off_at\":null"));
  sp211_buf_free(&b);
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

/* Values as systemd hands them over from /etc/default/sp211-printer-app: it
 * removes the quotes and the spaces around a value, and takes a comment at the
 * end of a line as part of it (docs/MQTT.md, The settings file). */
static int broker_error(const char *host, const char *port, const char *tls, const char *want) {
  sp211_broker b;
  const char *err = NULL;
  return sp211_parse_broker(host, port, tls, &b, &err) == -1 && err && !strcmp(err, want);
}

static void test_config_values(void) {
  sp211_broker b;
  const char *err = "stale";

  /* every spelling of yes and no, in any case, spaces around */
  static const char *const yes[] = {"yes", "YES", "true", "True", "on", "On", "1", " yes\t", "on\r\n"};
  static const char *const no[] = {"no", "No", "false", "FALSE", "off", "OFF", "0", "  0 "};
  for (size_t i = 0; i < sizeof yes / sizeof yes[0]; i++) CHECK(sp211_parse_bool(yes[i], 0) == 1);
  for (size_t i = 0; i < sizeof no / sizeof no[0]; i++) CHECK(sp211_parse_bool(no[i], 1) == 0);
  CHECK(sp211_parse_bool("", 1) == 1 && sp211_parse_bool("", 0) == 0);

  /* invalid: other words, quotes left in the value, comment on the line */
  static const char *const bad[] = {"y",     "n",    "yess",          "10", "-1", "oui",
                                    "\"yes\"", "'no'", "yes # comment", "yes yes yes yes yes"};
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) CHECK(sp211_parse_bool(bad[i], 1) == -1);

  /* success clears the error; with MQTT off the other settings are not read */
  CHECK(sp211_parse_broker("ha.lan", NULL, NULL, &b, &err) == 1 && !err);
  CHECK(sp211_parse_broker("", "port", "maybe", &b, &err) == 0 && !err);
  CHECK(sp211_parse_broker("ha.lan", "  ", "  ", &b, &err) == 1 && b.port == 1883 && !b.tls);

  /* the limits of a port */
  CHECK(sp211_parse_broker("ha.lan", "1", NULL, &b, &err) == 1 && b.port == 1);
  CHECK(sp211_parse_broker("ha.lan", "65535", NULL, &b, &err) == 1 && b.port == 65535);
  CHECK(sp211_parse_broker("ha.lan:65535", NULL, NULL, &b, &err) == 1 && b.port == 65535);
  CHECK(broker_error("ha.lan:65536", NULL, NULL, "MQTT_HOST"));
  CHECK(broker_error("ha.lan", "0", NULL, "MQTT_PORT"));
  CHECK(broker_error("ha.lan", "-1", NULL, "MQTT_PORT"));
  CHECK(broker_error("ha.lan", "+1883", NULL, "MQTT_PORT"));
  CHECK(broker_error("ha.lan", "1883x", NULL, "MQTT_PORT"));
  CHECK(broker_error("ha.lan", "99999999999999999999", NULL, "MQTT_PORT"));
  CHECK(broker_error("ha.lan", "1883 # broker", NULL, "MQTT_PORT"));
  CHECK(broker_error("ha.lan", "'1883'", NULL, "MQTT_PORT"));
  CHECK(broker_error("ha.lan", "\"1883\"", NULL, "MQTT_PORT"));
  CHECK(broker_error("ha.lan", NULL, "'yes'", "MQTT_TLS"));
  CHECK(broker_error("ha.lan:1884 # broker", NULL, NULL, "MQTT_HOST"));

  /* brackets */
  CHECK(broker_error("[]", NULL, NULL, "MQTT_HOST"));
  CHECK(broker_error("[fd00::5]1884", NULL, NULL, "MQTT_HOST"));
  CHECK(broker_error("[fd00::5]:", NULL, NULL, "MQTT_HOST"));

  /* a name too long for the field is cut, without overflowing it */
  char host[400];
  memset(host, 'a', sizeof host - 1);
  host[sizeof host - 1] = '\0';
  CHECK(sp211_parse_broker(host, NULL, NULL, &b, &err) == 1 && strlen(b.host) == sizeof b.host - 1);
}

/* The settings file the package installs: everything commented out, every
 * setting there once, and the example values accepted once their "#" is
 * removed. Run from the top of the repository, as "make test" does. */
static void test_default_file(void) {
  static const char *const names[] = {
      "MQTT_HOST",         "MQTT_PORT",      "MQTT_USERNAME",         "MQTT_PASSWORD",
      "MQTT_TLS",          "MQTT_CA_FILE",   "MQTT_CERT_FILE",        "MQTT_KEY_FILE",
      "MQTT_TLS_INSECURE", "MQTT_DISCOVERY", "MQTT_DISCOVERY_PREFIX", "MQTT_TOPIC",
      "PRINTER_WEB_URL"};
  enum { N = sizeof names / sizeof names[0] };
  char value[N][128] = {{0}};
  int seen[N] = {0};

  FILE *f = fopen("packaging/sp211-printer-app.default", "r");
  CHECK(f != NULL);
  if (!f) return;
  char line[256];
  while (fgets(line, sizeof line, f)) {
    line[strcspn(line, "\r\n")] = '\0';
    CHECK(!line[0] || line[0] == '#'); /* no setting is active */
    if (line[0] != '#') continue;
    size_t n = strspn(line + 1, "ABCDEFGHIJKLMNOPQRSTUVWXYZ_");
    if (!n || line[1 + n] != '=') continue; /* prose */
    size_t i = 0;
    while (i < N && (strlen(names[i]) != n || strncmp(line + 1, names[i], n))) i++;
    CHECK(i < N); /* a known setting */
    if (i == N) continue;
    seen[i]++;
    snprintf(value[i], sizeof value[i], "%s", line + 2 + n);
    /* the format the file states: no space around "=", no comment after */
    CHECK(value[i][0] && !strchr(value[i], ' ') && !strchr(value[i], '#'));
  }
  fclose(f);
  for (int i = 0; i < N; i++) CHECK(seen[i] == 1);

  /* the example values; indexes as in names[] */
  sp211_broker b;
  const char *err;
  CHECK(sp211_parse_broker(value[0], value[1], NULL, &b, &err) == 1);
  CHECK(!strcmp(b.host, value[0]) && b.port == SP211_MQTT_PORT && !b.tls);
  CHECK(sp211_parse_broker(value[0], NULL, value[4], &b, &err) == 1);
  CHECK(b.tls && b.port == SP211_MQTTS_PORT);
  /* the examples of MQTT_TLS_INSECURE and MQTT_DISCOVERY are their defaults */
  CHECK(sp211_parse_bool(value[8], -1) == 0);
  CHECK(sp211_parse_bool(value[9], -1) == 1);
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
  test_json_strings();
  test_json_objects();
  test_state();
  test_power();
  test_broker();
  test_config_values();
  test_default_file();
  printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
