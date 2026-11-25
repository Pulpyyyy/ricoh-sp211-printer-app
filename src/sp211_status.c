/* SPDX-License-Identifier: GPL-2.0-or-later */
/* sp211_status - see sp211_status.h */
#include "sp211_status.h"

#include <string.h>

static const unsigned char gdir_magic[4] = {'G', 'D', 'I', 'R'};

static uint32_t le32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int sp211_decode_gdir(const unsigned char *p, size_t n, sp211_status *st) {
  if (!p || n < SP211_GDIR_LEN || memcmp(p, gdir_magic, 4)) return 0;
  st->toner_percent = p[0x34] <= 100 ? p[0x34] : -1;
  st->paper_out = (p[0x2b] & 0x20) != 0;
  st->jobs_pending = (unsigned)p[0x22] | ((unsigned)p[0x23] << 8);
  st->job_pages_done = le32(p + 0x10);
  st->free_memory = le32(p + 0x1c);
  st->status_code = p[0x24];
  return 1;
}

int sp211_stream_feed(sp211_stream *s, const unsigned char *data, size_t n,
                      sp211_status_cb cb, void *arg) {
  int found = 0;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = data[i];
    if (s->len < 4) {
      /* still matching the "GDIR" marker */
      if (c == gdir_magic[s->len]) s->buf[s->len++] = c;
      else if (c == gdir_magic[0]) { s->buf[0] = c; s->len = 1; }
      else s->len = 0;
      continue;
    }
    s->buf[s->len++] = c;
    if (s->len == SP211_GDIR_LEN) {
      sp211_status st;
      if (sp211_decode_gdir(s->buf, s->len, &st)) {
        found++;
        if (cb) cb(&st, arg);
      }
      s->len = 0;
    }
  }
  return found;
}

static int register_valid(const unsigned char *reg, size_t n, unsigned index) {
  return reg && n >= SP211_REGISTER_LEN && !memcmp(reg, "GDIL", 4) && reg[4] == index;
}

void sp211_counters_init(sp211_counters *c) {
  memset(c, 0, sizeof *c);
  c->toner_percent = -1;
}

int sp211_decode_register1(const unsigned char *reg, size_t n, sp211_counters *c) {
  if (!register_valid(reg, n, SP211_REG_COUNTERS)) return 0;
  c->page_counter = le32(reg + 0x2c);
  c->have_pages = 1;
  if (reg[5] <= 100) c->toner_percent = reg[5];
  /* firmware version, NUL-padded text at 0x18-0x1F; kept only if printable */
  size_t len = 0;
  while (len < sizeof c->firmware - 1 && reg[0x18 + len]) len++;
  int printable = len > 0;
  for (size_t i = 0; i < len; i++)
    if (reg[0x18 + i] < 0x20 || reg[0x18 + i] > 0x7e) printable = 0;
  if (printable) {
    memcpy(c->firmware, reg + 0x18, len);
    c->firmware[len] = '\0';
  }
  return 1;
}

int sp211_decode_register3(const unsigned char *reg, size_t n, sp211_counters *c) {
  if (!register_valid(reg, n, SP211_REG_COUNTERS_ALT)) return 0;
  c->page_counter = le32(reg + 0x3c);
  c->have_pages = 1;
  return 1;
}

int sp211_decode_misfeeds(const unsigned char *reg, size_t n, sp211_counters *c) {
  if (!register_valid(reg, n, SP211_REG_MISFEEDS)) return 0;
  c->misfeed_total = le32(reg + 0x18);
  c->misfeed_tray1 = le32(reg + 0x1c);
  c->have_misfeeds = 1;
  return 1;
}
