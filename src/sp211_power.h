/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sp211_power - when the printer's plug should be on.
 *
 * The printer application does not drive the plug itself: it publishes the
 * power it wants as an MQTT switch, and a Home Assistant automation (the
 * blueprint in homeassistant/blueprints/) keeps that switch and the real
 * plug in step, in both directions. This module only decides:
 *
 *   - auto on: a job waits and the printer is switched off -> on;
 *   - auto off: nothing to print for delay_min minutes -> off;
 *   - never off while a job is active or waiting.
 *
 * Without a linked plug (no command ever received from Home Assistant) the
 * switch only mirrors whether the printer answers, and nothing is decided.
 * No PAPPL, no MQTT: tested in tests/test_status.c.
 */
#ifndef SP211_POWER_H
#define SP211_POWER_H

#include <stdbool.h>
#include <time.h>

#define SP211_POWER_DELAY_MIN 1
#define SP211_POWER_DELAY_MAX 240
#define SP211_POWER_DELAY_DEFAULT 15

typedef struct {
  bool auto_on, auto_off;
  int delay_min;     /* minutes without a job before auto off */
  bool linked;       /* a plug follows this switch (Home Assistant sent a command) */
  bool on;           /* the power the printer has, or should have */
  time_t idle_since; /* start of the current period without jobs; 0 while busy */
} sp211_power;

void sp211_power_init(sp211_power *p);

/* Clamps a delay to SP211_POWER_DELAY_MIN..MAX. */
int sp211_power_clamp_delay(long minutes);

/* A command from Home Assistant: the plug is (or must be) on/off. Marks the
 * plug as linked. Switching on starts a new idle period, so that a printer
 * switched on by hand is not switched off again at once. */
void sp211_power_set(sp211_power *p, bool on, time_t now);

/* One decision step. online: the printer answers on USB; busy: a job is
 * active or waiting. Returns true when p->on changed. */
bool sp211_power_step(sp211_power *p, bool online, bool busy, time_t now);

/* When auto off will switch the printer off, or 0 if it will not. */
time_t sp211_power_off_at(const sp211_power *p, bool busy);

#endif
