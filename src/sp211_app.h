/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sp211_app - what src/sp211-printer-app.c shares with src/sp211_mqtt.c: the
 * last status read from the printer over USB. One printer per application.
 */
#ifndef SP211_APP_H
#define SP211_APP_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define SP211_APP_VERSION "0.2"

typedef struct {
  bool read;            /* at least one read was attempted */
  bool online;          /* the last read reached the printer */
  bool stopped_offline; /* the queue is stopped because the printer cannot be reached */
  int toner_percent;    /* -1 if unknown */
  bool have_pages;
  uint32_t pages;       /* the printer's lifetime page counter */
  bool have_misfeeds;
  uint32_t misfeeds;    /* paper misfeeds, total */
  char firmware[9];     /* "V1.06", "" if unknown */
  time_t last_read;
} sp211_app_status;

/* Copies the last status. Thread-safe. */
void sp211_app_get_status(sp211_app_status *st);

/* Reads the printer every few seconds for the next three minutes, instead of
 * every minute: used while waiting for a printer that is being switched on. */
void sp211_app_poll_soon(void);

#endif
