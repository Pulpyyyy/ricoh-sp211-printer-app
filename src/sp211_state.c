/* SPDX-License-Identifier: GPL-2.0-or-later */
/* sp211_state - see sp211_state.h */
#include "sp211_state.h"

#include "sp211_status.h"

void sp211_state_time(time_t t, char *buf, size_t size) {
  struct tm tm;
  gmtime_r(&t, &tm);
  strftime(buf, size, "%Y-%m-%dT%H:%M:%S+00:00", &tm);
}

const char *sp211_state_toner(int percent) {
  return percent < 0 ? "unknown"
         : percent == 0 ? "empty"
         : percent <= SP211_TONER_LOW_PERCENT ? "low"
                                              : "ok";
}

void sp211_state_supplies(sp211_buf *b, const sp211_app_status *st) {
  if (st->toner_percent >= 0) sp211_buf_printf(b, ",\"toner\":%d", st->toner_percent);
  else sp211_buf_cat(b, ",\"toner\":null");
  sp211_buf_printf(b, ",\"toner_state\":\"%s\"", sp211_state_toner(st->toner_percent));
  if (st->have_pages) sp211_buf_printf(b, ",\"pages_total\":%u", (unsigned)st->pages);
  else sp211_buf_cat(b, ",\"pages_total\":null");
  if (st->have_misfeeds) sp211_buf_printf(b, ",\"misfeeds\":%u", (unsigned)st->misfeeds);
  else sp211_buf_cat(b, ",\"misfeeds\":null");
}

void sp211_state_power(sp211_buf *b, const sp211_power *p, bool busy) {
  time_t off_at = sp211_power_off_at(p, busy);
  sp211_buf_printf(b, ",\"power\":\"%s\",\"linked\":%s,\"auto_on\":\"%s\",\"auto_off\":\"%s\",\"delay\":%d,\"off_at\":",
                   p->on ? "ON" : "OFF", p->linked ? "true" : "false",
                   p->auto_on ? "ON" : "OFF", p->auto_off ? "ON" : "OFF",
                   p->delay_min);
  if (off_at) {
    char t[40];
    sp211_state_time(off_at, t, sizeof t);
    sp211_buf_printf(b, "\"%s\"", t);
  } else {
    sp211_buf_cat(b, "null");
  }
}
