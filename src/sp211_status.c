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

size_t sp211_counters_message(const sp211_counters *c, char *buf, size_t bufsize) {
  size_t len = 0;
  if (!bufsize) return 0;
  buf[0] = '\0';
  if (c->have_pages)
    len += (size_t)snprintf(buf + len, bufsize - len, "page-counter=%lu",
                            (unsigned long)c->page_counter);
  if (c->have_misfeeds && len < bufsize)
    len += (size_t)snprintf(buf + len, bufsize - len, "%smisfeeds=%lu;misfeeds-tray1=%lu",
                            len ? ";" : "", (unsigned long)c->misfeed_total,
                            (unsigned long)c->misfeed_tray1);
  return len < bufsize ? len : bufsize - 1;
}

unsigned sp211_pages_printed(const sp211_counters *before, const sp211_counters *after) {
  if (!before || !after || !before->have_pages || !after->have_pages) return 0;
  if (after->page_counter <= before->page_counter) return 0;
  uint32_t delta = after->page_counter - before->page_counter;
  return delta <= 10000 ? (unsigned)delta : 0;
}

void sp211_reporter_init(sp211_reporter *r, FILE *out) {
  memset(r, 0, sizeof *r);
  r->out = out;
  r->toner = r->toner_low = r->paper_out = r->sent_toner = -1;
}

/* One ATTR line carries the whole marker set; send it when the toner level
 * or the counters text changed. */
static void report_supplies(sp211_reporter *r) {
  if (r->toner < 0) return;
  if (r->toner == r->sent_toner && !strcmp(r->message, r->sent_message)) return;
  r->sent_toner = r->toner;
  strcpy(r->sent_message, r->message);
  fprintf(r->out,
          "ATTR: marker-colors=#000000 marker-names='Black Toner' marker-types=toner "
          "marker-low-levels=%d marker-high-levels=100 marker-levels=%d",
          SP211_TONER_LOW_PERCENT, r->toner);
  if (r->message[0]) fprintf(r->out, " marker-message=%s", r->message);
  fputc('\n', r->out);
  int low = r->toner <= SP211_TONER_LOW_PERCENT;
  if (low != r->toner_low) {
    r->toner_low = low;
    fprintf(r->out, "STATE: %ctoner-low-warning\n", low ? '+' : '-');
  }
}

void sp211_report(sp211_reporter *r, const sp211_status *st) {
  if (st->toner_percent >= 0) r->toner = st->toner_percent;
  report_supplies(r);
  if (st->paper_out != r->paper_out) {
    r->paper_out = st->paper_out;
    if (st->paper_out) {
      fputs("STATE: +media-empty-error\n", r->out);
      fputs("INFO: Out of paper - load paper, then press Start on the printer\n", r->out);
    } else {
      fputs("STATE: -media-empty-error\n", r->out);
      fputs("INFO: Printing\n", r->out);
    }
  }
  fflush(r->out);
}

void sp211_report_counters(sp211_reporter *r, const sp211_counters *c) {
  char text[sizeof r->message];
  if (!c) return;
  if (r->toner < 0 && c->toner_percent >= 0) r->toner = c->toner_percent;
  if (sp211_counters_message(c, text, sizeof text)) strcpy(r->message, text);
  report_supplies(r);
  fflush(r->out);
}

void sp211_report_pages(sp211_reporter *r, unsigned pages) {
  if (!pages) return;
  fprintf(r->out, "PAGE: total %u\n", pages);
  fflush(r->out);
}

int sp211_keep_waiting(double elapsed, double silence, const sp211_status *st,
                       int seen_active, int waiting_for_power) {
  int paper_out = st && st->paper_out;
  int pending = st && st->jobs_pending;
  if (waiting_for_power) return 1;                    /* CUPS is holding the job */
  if (silence > SP211_WAIT_SILENCE_SECONDS) return 0; /* off or unplugged */
  if (elapsed > SP211_WAIT_MAX_SECONDS) return 0;
  if (paper_out || pending) return 1;                 /* job held or printing */
  if (seen_active) return 0;                          /* was printing, now done */
  return elapsed <= SP211_WAIT_START_SECONDS;         /* let the printer start */
}
