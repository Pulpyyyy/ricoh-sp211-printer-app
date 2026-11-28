/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sp211_state - the fields of the MQTT state message that need no PAPPL:
 * supplies, counters, power and times (docs/MQTT.md). The rest of the
 * message is written by sp211_mqtt.c. Tested in tests/test_status.c.
 */
#ifndef SP211_STATE_H
#define SP211_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "sp211_app.h"
#include "sp211_json.h"
#include "sp211_power.h"

/* ISO 8601 in UTC, "2026-11-25T20:01:18+00:00". */
void sp211_state_time(time_t t, char *buf, size_t size);

/* "ok", "low", "empty" or "unknown" (percent below 0). */
const char *sp211_state_toner(int percent);

/* Appends ,"toner":...,"toner_state":...,"pages_total":...,"misfeeds":...
 * A value not read yet is null. */
void sp211_state_supplies(sp211_buf *b, const sp211_app_status *st);

/* Appends ,"power":...,"linked":...,"auto_on":...,"auto_off":...,"delay":...,
 * "off_at":... busy: as for sp211_power_off_at(). */
void sp211_state_power(sp211_buf *b, const sp211_power *p, bool busy);

#endif
