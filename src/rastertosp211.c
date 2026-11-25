/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Alexandru Hrisca
 */
/*
 * rastertosp211 - native CUPS filter for Ricoh SP 211 / SP 213 / SP 220 / SP 221
 * class GDI printers (the "lilo2" GDIJ stream format).
 *
 * Drop-in replacement for Ricoh's Intel-only rastertolilo2 filter; the output
 * is byte-identical to it for the supported options. The stream encoder lives
 * in lilo2.c.
 *
 * Usage (as a CUPS filter): rastertosp211 job user title copies options [file]
 *
 * MODIFIED (GPL v2 section 2a notice):
 * 2026-11-25, for the ricoh-sp211-printer-app project. This file comes from
 * https://github.com/Creative8428/ricoh-sp211-driver (src/rastertosp211.c).
 * Changes: options the job does not set are taken from the queue's PPD;
 * the printer's status packets are read from the CUPS back-channel and
 * reported to CUPS (toner, out of paper); the filter stays until the printer
 * has finished the job; the printer's page and misfeed counters are read
 * over USB and reported, with the number of pages actually printed.
 */

#include <cups/backend.h>
#include <cups/cups.h>
#include <cups/ppd.h>
#include <cups/sidechannel.h>
#include <cups/raster.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "lilo2.h"
#include "sp211_status.h"
#include "sp211_usb.h"

/* ---- printer status from the CUPS back-channel ---------------------------
 * CUPS gives a filter the printer's replies on file descriptor 3. The printer
 * sends a GDIR status packet every few seconds while it holds a job. Run by
 * hand (no back-channel) the filter behaves exactly as before. */
#define BACKCHANNEL_FD 3

#define POWER_PROBE_SECONDS 5

static int status_enabled;
static sp211_stream status_stream;
static sp211_reporter status_reporter;
static sp211_status status_last;
static int status_have, status_seen_active;

/* Lifetime counters read from the printer before the job; the difference
 * with the value after the job is the number of pages that really came out. */
static sp211_counters counters_before;
static int counters_have_before;

static double now_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void status_on_packet(const sp211_status *st, void *arg) {
  (void)arg;
  status_last = *st;
  status_have = 1;
  if (st->jobs_pending) status_seen_active = 1;
  sp211_report(&status_reporter, st);
}

/* Reads the counters before the job, once. Fails while the printer is
 * switched off; it is then tried again when the printer starts answering. */
static void counters_read_before(void) {
  if (counters_have_before) return;
  if (sp211_usb_read_counters(getenv("DEVICE_URI"), 0, &counters_before)) {
    counters_have_before = 1;
    sp211_report_counters(&status_reporter, &counters_before);
  }
}

static void status_init(void) {
  struct stat sb;
  /* only a pipe or socket is a back-channel; anything else is not ours */
  if (fstat(BACKCHANNEL_FD, &sb) == 0 && (S_ISFIFO(sb.st_mode) || S_ISSOCK(sb.st_mode))) {
    status_enabled = 1;
    sp211_reporter_init(&status_reporter, stderr);
    counters_read_before();
  }
}

/* Reads what the printer sent, waiting at most timeout seconds.
 * Returns 1 if any data arrived. */
static int status_poll(double timeout) {
  unsigned char buf[1024];
  if (!status_enabled) return 0;
  ssize_t n = cupsBackChannelRead((char *)buf, sizeof buf, timeout);
  if (n <= 0) return 0;
  counters_read_before(); /* the printer is on now */
  sp211_stream_feed(&status_stream, buf, (size_t)n, status_on_packet, NULL);
  return 1;
}

/* All data is sent: stay until the printer has finished the job, so that a
 * paper error keeps the CUPS job (and its state) visible and the counter
 * read at the end is final. Then hand the counters and the number of pages
 * that really came out to CUPS. */
static void status_wait_for_printer(void) {
  if (!status_enabled) return;
  double start = now_seconds(), last_data = start, next_probe = 0;
  int heard = status_have;
  for (;;) {
    if (status_poll(1.0)) {
      last_data = now_seconds();
      if (!heard) { heard = 1; start = last_data; }
    }
    double now = now_seconds();
    /* Never heard from the printer and it cannot be reached: it is switched
     * off and the CUPS backend is waiting for it. Wait too (CUPS stops the
     * filter if the job is cancelled) and start timing once it is on. */
    int waiting_for_power = 0;
    if (!heard && !counters_have_before) {
      if (now >= next_probe) {
        next_probe = now + POWER_PROBE_SECONDS;
        counters_read_before();
      }
      if (counters_have_before) start = last_data = now;
      else waiting_for_power = 1;
    }
    if (!sp211_keep_waiting(now - start, now - last_data, status_have ? &status_last : NULL,
                            status_seen_active, waiting_for_power))
      break;
  }

  sp211_counters after;
  if (sp211_usb_read_counters(getenv("DEVICE_URI"), 1, &after)) {
    sp211_report_counters(&status_reporter, &after);
    if (counters_have_before)
      sp211_report_pages(&status_reporter, sp211_pages_printed(&counters_before, &after));
  }
}

static void write_all(const void *p, size_t n) {
  const unsigned char *c = p;
  while (n) {
    ssize_t w = write(1, c, n);
    if (w <= 0) { perror("ERROR: write"); exit(1); }
    c += w; n -= (size_t)w;
  }
  status_poll(0.0);
}

/* the original filter only accepts "true" (any case) */
static int is_true(const char *v) { return v && !strcasecmp(v, "true"); }

/* Value of a PPD option for this job: what the job asked for, otherwise the
 * queue default from the PPD. CUPS only passes the options a client set, so
 * a plain "lp file" carries no PageSize or InputSlot; without the PPD default
 * the stream says "paper unspecified" and a printer loaded with A4 holds the
 * job until Start is pressed. NULL if neither is known. */
static const char *job_option(ppd_file_t *ppd, const char *name,
                              int num_options, cups_option_t *options) {
  if (ppd) {
    ppd_choice_t *c = ppdFindMarkedChoice(ppd, name);
    if (c) return c->choice;
  }
  return cupsGetOption(name, num_options, options);
}

typedef struct {
  int paper, tray, media, toner_save;
} job_opts_t;

/* Reads one raster page (header already read) and appends GDIP + GDIB blocks
 * to out. rotate: 180 degrees (manual duplex back sides, long-edge binding). */
static int do_page(cups_raster_t *ras, cups_page_header2_t *hdr, lilo2_buf *out,
                   const job_opts_t *jo, int tray, int pflags, int pageno, int rotate) {
  unsigned w = hdr->cupsWidth, H = hdr->cupsHeight, bpl = hdr->cupsBytesPerLine;

  if (hdr->cupsBitsPerPixel != 8 || hdr->cupsBitsPerColor != 8) {
    fprintf(stderr, "ERROR: unsupported raster format (%u bpp)\n", hdr->cupsBitsPerPixel);
    return -1;
  }

  lilo2_page pg = {0};
  pg.width = w; pg.height = H;
  pg.page_w_pt = hdr->PageSize[0]; pg.page_h_pt = hdr->PageSize[1];
  pg.paper = jo->paper; pg.tray = tray; pg.media = jo->media;
  pg.flags = pflags; pg.pageno = pageno; pg.toner_save = jo->toner_save;
  if (lilo2_page_begin(&pg, out)) { fputs("ERROR: bad page size\n", stderr); return -1; }

  unsigned char *line = malloc(bpl > w ? bpl : w);
  if (!line) { fputs("ERROR: out of memory\n", stderr); exit(1); }

  if (rotate) {
    unsigned char *page = malloc((size_t)w * H);
    if (!page) { fputs("ERROR: out of memory\n", stderr); exit(1); }
    for (unsigned y = 0; y < H; y++) {
      if (cupsRasterReadPixels(ras, line, bpl) == 0) memset(line, 0, bpl);
      unsigned char *dst = page + (size_t)(H - 1 - y) * w;
      for (unsigned x = 0; x < w; x++) dst[w - 1 - x] = line[x];
    }
    for (unsigned y = 0; y < H; y++) lilo2_page_line(&pg, page + (size_t)y * w);
    free(page);
  } else {
    for (unsigned y = 0; y < H; y++) {
      if (cupsRasterReadPixels(ras, line, bpl) == 0) memset(line, 0, bpl);
      lilo2_page_line(&pg, line);
    }
  }
  lilo2_page_end(&pg);
  free(line);
  return 0;
}

int main(int argc, char *argv[]) {
  if (argc < 6 || argc > 7) {
    fputs("Usage: rastertosp211 job user title copies options [file]\n", stderr);
    return 1;
  }
  signal(SIGPIPE, SIG_IGN);
  status_init();

  int fd = 0;
  if (argc == 7 && (fd = open(argv[6], O_RDONLY)) < 0) {
    perror("ERROR: unable to open raster file");
    return 1;
  }

  cups_option_t *options = NULL;
  int num_options = cupsParseOptions(argv[5], 0, &options);
  const char *v;

  /* The queue's PPD (CUPS sets $PPD for filters): its defaults, overridden
   * by the job's options. cupsMarkOptions also maps "media=A4" to PageSize. */
  ppd_file_t *ppd = getenv("PPD") ? ppdOpenFile(getenv("PPD")) : NULL;
  if (ppd) {
    ppdMarkDefaults(ppd);
    cupsMarkOptions(ppd, num_options, options);
  }

  job_opts_t jo;
  v = job_option(ppd, "PageSize", num_options, options);
  if (!v) v = cupsGetOption("media", num_options, options);
  jo.paper = v ? lilo2_paper_code(v) : LILO2_PAPER_UNSPECIFIED;
  jo.media = lilo2_media_code(job_option(ppd, "MediaType", num_options, options));
  jo.tray = lilo2_tray_code(job_option(ppd, "InputSlot", num_options, options));
  jo.toner_save = is_true(job_option(ppd, "TonerSave", num_options, options));

  lilo2_job job = {0};
  job.toner_save = jo.toner_save;
  job.edge_smoothing = is_true(job_option(ppd, "EdgeSmoothing", num_options, options));
  v = job_option(ppd, "Duplex", num_options, options);
  int duplex = v && (!strcmp(v, "DuplexNoTumble") || !strcmp(v, "DuplexTumble")) &&
               lilo2_duplex_allowed(jo.paper);
  int tumble = v && !strcmp(v, "DuplexTumble");
  job.duplex = duplex;
  if ((v = cupsGetOption("time-at-creation", num_options, options)) != NULL) {
    time_t t = (time_t)strtoll(v, NULL, 10);
    struct tm lt;
    localtime_r(&t, &lt);
    job.ctime = (uint32_t)(t + lt.tm_gmtoff);
  }
  job.title = argv[3];
  job.user = argv[2];

  cups_raster_t *ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
  if (!ras) { fputs("ERROR: cannot read raster data\n", stderr); return 1; }

  lilo2_buf out = {0};
  lilo2_buf *pages = NULL; /* manual duplex: buffered output for each page */
  int npages = 0, header_done = 0;
  cups_page_header2_t hdr;

  while (cupsRasterReadHeader2(ras, &hdr)) {
    npages++;
    if (!header_done) {
      job.copies = (int)hdr.NumCopies;
      job.hires = hdr.HWResolution[0] >= 1200;
      out.len = 0;
      lilo2_job_header(&out, &job);
      write_all(out.d, out.len);
      header_done = 1;
    }
    fprintf(stderr, "INFO: page %d\n", npages);

    if (!duplex) {
      out.len = 0;
      if (do_page(ras, &hdr, &out, &jo, jo.tray, LILO2_PAGE_NORMAL, npages, 0)) return 1;
      write_all(out.d, out.len);
    } else {
      /* manual duplex: buffer every page; even pages are printed first
       * (rotated for long-edge binding), then the odd pages from Tray 1 */
      pages = realloc(pages, sizeof(lilo2_buf) * npages);
      lilo2_buf *pb = &pages[npages - 1];
      memset(pb, 0, sizeof *pb);
      if (npages % 2 == 0) {
        if (do_page(ras, &hdr, pb, &jo, jo.tray, LILO2_PAGE_DUPLEX_FRONT, npages, !tumble)) return 1;
      } else {
        if (do_page(ras, &hdr, pb, &jo, 2, LILO2_PAGE_DUPLEX_BACK, npages, 0)) return 1;
      }
    }
  }

  if (duplex) {
    for (int i = 2; i <= npages; i += 2) write_all(pages[i - 1].d, pages[i - 1].len);
    for (int i = 1; i <= npages; i += 2) {
      if (i == 1) pages[0].d[33] = LILO2_PAGE_DUPLEX_BACK1;
      write_all(pages[i - 1].d, pages[i - 1].len);
    }
  }

  if (header_done) {
    out.len = 0;
    lilo2_job_end(&out);
    write_all(out.d, out.len);
  }
  cupsRasterClose(ras);
  if (fd) close(fd);
  if (header_done) status_wait_for_printer();
  return npages ? 0 : 1;
}
