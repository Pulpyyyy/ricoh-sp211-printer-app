/* SPDX-License-Identifier: GPL-2.0-or-later */
/* sp211_power - see sp211_power.h */
#include "sp211_power.h"

#include <string.h>

void sp211_power_init(sp211_power *p) {
  memset(p, 0, sizeof *p);
  p->auto_on = true;
  p->auto_off = true;
  p->delay_min = SP211_POWER_DELAY_DEFAULT;
}

int sp211_power_clamp_delay(long minutes) {
  if (minutes < SP211_POWER_DELAY_MIN) return SP211_POWER_DELAY_MIN;
  if (minutes > SP211_POWER_DELAY_MAX) return SP211_POWER_DELAY_MAX;
  return (int)minutes;
}

void sp211_power_set(sp211_power *p, bool on, time_t now) {
  p->linked = true;
  if (on && !p->on) p->idle_since = now;
  p->on = on;
}

bool sp211_power_step(sp211_power *p, bool online, bool busy, time_t now) {
  bool was = p->on;

  if (busy) p->idle_since = 0;
  else if (!p->idle_since) p->idle_since = now;

  if (!p->linked) {
    /* nothing to switch: show whether the printer has power */
    p->on = online;
    return p->on != was;
  }
  if (busy) {
    if (!p->on && p->auto_on) p->on = true;
  } else if (p->on && p->auto_off &&
             now - p->idle_since >= (time_t)sp211_power_clamp_delay(p->delay_min) * 60) {
    p->on = false;
  }
  return p->on != was;
}

time_t sp211_power_off_at(const sp211_power *p, bool busy) {
  if (!p->linked || !p->on || !p->auto_off || busy || !p->idle_since) return 0;
  return p->idle_since + (time_t)sp211_power_clamp_delay(p->delay_min) * 60;
}
