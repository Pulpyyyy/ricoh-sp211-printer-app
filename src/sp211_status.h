/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sp211_status - printer status of the Ricoh SP 211 class GDI printers.
 *
 * While it holds a job (and whenever it is asked with "GDIC GDIR"), the
 * printer sends a 64-byte "GDIR" packet on its USB bulk IN endpoint. CUPS
 * hands those bytes to filters on the back-channel. This module decodes the
 * packets and turns them into CUPS "ATTR:" / "STATE:" messages.
 *
 * No CUPS or USB dependency; see docs/STATUS_PROTOCOL.md for the layout and
 * how each field was established.
 */
#ifndef SP211_STATUS_H
#define SP211_STATUS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define SP211_GDIR_LEN 64
#define SP211_TONER_LOW_PERCENT 10

typedef struct {
  int toner_percent;       /* black toner left, 0..100; -1 if out of range */
  int paper_out;           /* tray pulled out, or a sheet could not be fed */
  unsigned jobs_pending;   /* 1 while the printer holds a job */
  uint32_t job_pages_done; /* pages finished in the current job */
  uint32_t free_memory;    /* free image memory, bytes */
  int status_code;         /* 0 normally, 7 in factory mode; nothing else */
} sp211_status;

/* Decodes one packet. Returns 1 on success, 0 if p is not a GDIR packet. */
int sp211_decode_gdir(const unsigned char *p, size_t n, sp211_status *st);

/* Finds GDIR packets in a byte stream that may be split across reads and may
 * contain other bytes. Zero-initialize before first use. */
typedef struct {
  unsigned char buf[SP211_GDIR_LEN];
  size_t len;
} sp211_stream;

typedef void (*sp211_status_cb)(const sp211_status *st, void *arg);

/* Feeds n bytes; calls cb for every complete packet. Returns the number of
 * packets found. */
int sp211_stream_feed(sp211_stream *s, const unsigned char *data, size_t n,
                      sp211_status_cb cb, void *arg);

/* ---- counters read from the printer's vendor registers -------------------
 * A second channel, independent of the print stream: USB vendor control
 * requests return 64-byte "GDIL" registers (see sp211_usb.h). */
#define SP211_REGISTER_LEN 64
#define SP211_REG_COUNTERS 0x01
#define SP211_REG_COUNTERS_ALT 0x03
#define SP211_REG_MISFEEDS 0x27 /* asks the print engine: takes about 3 s */
/* Reading register 0x2B makes the SP 211 controller reboot (firmware V1.06).
 * Nothing outside this range may be read. */
#define SP211_REG_SAFE_FIRST 0x01
#define SP211_REG_SAFE_LAST 0x2A

typedef struct {
  int have_pages;        /* page_counter is valid */
  uint32_t page_counter; /* lifetime page total */
  int toner_percent;     /* -1 if unknown */
  int have_misfeeds;
  uint32_t misfeed_total, misfeed_tray1;
} sp211_counters;

void sp211_counters_init(sp211_counters *c);
/* Each returns 1 if the register was valid and used. reg may be NULL. */
int sp211_decode_register1(const unsigned char *reg, size_t n, sp211_counters *c);
int sp211_decode_register3(const unsigned char *reg, size_t n, sp211_counters *c);
int sp211_decode_misfeeds(const unsigned char *reg, size_t n, sp211_counters *c);

/* Text for the CUPS "marker-message" attribute, for example
 * "page-counter=193;misfeeds=6;misfeeds-tray1=6". CUPS has no attribute for a
 * lifetime counter and this is the only free-text field a filter can set.
 * Returns the length written (0 if nothing is known). */
size_t sp211_counters_message(const sp211_counters *c, char *buf, size_t bufsize);

/* Pages that physically came out during a job, from the counter before and
 * after it. 0 if unknown or implausible. */
unsigned sp211_pages_printed(const sp211_counters *before, const sp211_counters *after);

/* Writes CUPS messages to out, only when something changed. */
typedef struct {
  FILE *out;
  int toner;     /* last known, -1 = none yet */
  int toner_low; /* -1 = none yet */
  int paper_out; /* -1 = none yet */
  char message[96];      /* last known counters text */
  int sent_toner;        /* what the last ATTR line carried */
  char sent_message[96];
} sp211_reporter;

void sp211_reporter_init(sp211_reporter *r, FILE *out);
void sp211_report(sp211_reporter *r, const sp211_status *st);
/* Counters (and the toner level, if not known yet) from the registers. */
void sp211_report_counters(sp211_reporter *r, const sp211_counters *c);
/* Job accounting: "PAGE: total N". Nothing if pages is 0. */
void sp211_report_pages(sp211_reporter *r, unsigned pages);

/* After all data is sent: should the filter stay alive? Staying alive keeps
 * the CUPS job "processing" until the printer is really done, so a paper
 * error stays visible instead of the job completing silently.
 * st may be NULL (no packet received yet). Times are in seconds. */
#define SP211_WAIT_START_SECONDS 20   /* for the printer to start the job */
#define SP211_WAIT_SILENCE_SECONDS 15 /* printer stopped talking */
#define SP211_WAIT_MAX_SECONDS 1800
/* waiting_for_power: the printer has never answered and cannot be reached,
 * so it is switched off and CUPS is holding the job; wait with it. */
int sp211_keep_waiting(double elapsed, double silence, const sp211_status *st,
                       int seen_active, int waiting_for_power);

#endif
