/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sp211-printer-app - IPP Everywhere Printer Application for the Ricoh SP 211.
 *
 * Built on PAPPL: PAPPL is the IPP server (IPP Everywhere, AirPrint, Mopria,
 * DNS-SD, web interface, spooling) and turns the documents it accepts into
 * raster lines. This file only describes the printer and turns those lines
 * into the printer's "lilo2" stream with the shared encoder (src/lilo2.c).
 *
 * The printer's status is read over USB with vendor control requests
 * (src/sp211_usb.c), between jobs: toner level (printer-supply, marker-*),
 * toner-low, offline, and the printer's own page counter, published as
 * printer-impressions-completed. Out-of-paper is only sent by the printer
 * during a job and is not read yet.
 *
 *   sp211-printer-app server -o server-port=8632 -o spool-directory=DIR
 *   sp211-printer-app add -d NAME -m ricoh_sp211 -v DEVICE-URI
 *   sp211-printer-app --help
 *
 * PAPPL is under the Apache License 2.0, which is compatible with version 3
 * of the GNU GPL only: a binary linked with it is distributed under
 * GPL-3.0-or-later (see NOTICE.md).
 */

#include <pappl/pappl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "lilo2.h"
#include "sp211_status.h"
#include "sp211_usb.h"

#define VERSION "0.1"
#define DRIVER_NAME "ricoh_sp211"
#define DRIVER_DESC "Ricoh SP 211"
#define DEVICE_ID "MFG:RICOH;MDL:SP 211;"

/* 12 pt margins on every side, as in the original CUPS driver: hundredths of mm and 600 dpi pixels */
#define MARGIN_HMM 423
#define MARGIN_PX 100
#define RESOLUTION 600

typedef struct {
  const char *ipp; /* IPP keyword */
  const char *ppd; /* keyword of the original driver's PPD, which the lilo2 helpers take */
} map_t;

/* Paper sizes the printer accepts, with their IPP names. */
static const map_t sizes[] = {
  {"iso_a4_210x297mm", "A4"},       {"na_letter_8.5x11in", "Letter"},
  {"na_legal_8.5x14in", "Legal"},   {"iso_a5_148x210mm", "A5"},
  {"iso_a6_105x148mm", "A6"},       {"jis_b5_182x257mm", "B5"},
  {"na_invoice_5.5x8.5in", "HalfLetter"}, {"na_foolscap_8.5x13in", "Foolscap"},
};
static const map_t types[] = {
  {"stationery", "PlainRecycled"},           {"stationery-recycled", "Recycled"},
  {"stationery-heavyweight", "Thick"},       {"stationery-lightweight", "Thin"},
  {"stationery-letterhead", "Letterhead"},   {"stationery-preprinted", "Preprinted"},
  {"stationery-prepunched", "Prepunched"},   {"stationery-bond", "Bond"},
  {"stationery-colored", "Color"},           {"cardstock", "Cardstock"},
};
static const map_t sources[] = {{"main", "Tray1"}, {"manual", "Manual"}};

#define COUNT(a) (sizeof(a) / sizeof *(a))

static const char *map_lookup(const map_t *m, size_t n, const char *ipp) {
  for (size_t i = 0; ipp && i < n; i++)
    if (!strcmp(m[i].ipp, ipp)) return m[i].ppd;
  return NULL;
}

static pappl_pr_driver_t drivers[] = {{DRIVER_NAME, DRIVER_DESC, DEVICE_ID, NULL}};

/* ---- one job ------------------------------------------------------------ */

typedef struct {
  lilo2_buf out;       /* encoded data not yet sent to the printer */
  lilo2_page pg;       /* page being encoded */
  unsigned x0, y0;     /* top left corner of the imageable area, in raster pixels */
  unsigned w, h;       /* imageable area, in pixels at 600 dpi */
  unsigned lines;      /* lines given to the encoder for this page */
  unsigned char *ink;  /* one line of 8-bit ink values (255 = black) */
} job_data_t;

/* Data goes to the printer in pieces smaller than PAPPL's device write buffer
 * (8192 bytes). papplDeviceWrite() given more than the buffer holds first
 * flushes the buffer, even when it is empty, which sends a zero-length USB
 * transfer (PAPPL 1.3.1 to 1.4.12). The SP 211 then keeps the job without
 * printing it and refuses all further data until it is switched off.
 * Pieces smaller than the buffer are always buffered, and PAPPL only flushes
 * a buffer that holds data. Fixed upstream after 1.4.12 (PAPPL issue #434,
 * commit bc29de974); this can go once that fix is in the PAPPL used. */
#define SEND_PIECE 2048

static bool send_out(pappl_device_t *device, job_data_t *jd) {
  bool ok = true;
  for (size_t off = 0; ok && off < jd->out.len; off += SEND_PIECE) {
    size_t n = jd->out.len - off < SEND_PIECE ? jd->out.len - off : SEND_PIECE;
    ok = papplDeviceWrite(device, jd->out.d + off, n) == (ssize_t)n;
  }
  jd->out.len = 0;
  return ok;
}

static void free_job_data(pappl_job_t *job, job_data_t *jd) {
  if (!jd) return;
  lilo2_buf_free(&jd->out);
  free(jd->ink);
  free(jd);
  papplJobSetData(job, NULL);
}

static bool sp211_rstartjob(pappl_job_t *job, pappl_pr_options_t *options,
                            pappl_device_t *device) {
  job_data_t *jd = calloc(1, sizeof *jd);
  if (!jd) return false;
  papplJobSetData(job, jd);

  lilo2_job hdr = {0};
  /* PAPPL sends each page once; the printer makes the copies itself. */
  hdr.copies = options->copies > 0 ? options->copies : 1;
  hdr.title = papplJobGetName(job);
  hdr.user = papplJobGetUsername(job);
  time_t t = papplJobGetTimeCreated(job);
  struct tm lt;
  localtime_r(&t, &lt);
  hdr.ctime = (uint32_t)(t + lt.tm_gmtoff); /* the printer shows local time */
  lilo2_job_header(&jd->out, &hdr);
  return send_out(device, jd);
}

static bool sp211_rstartpage(pappl_job_t *job, pappl_pr_options_t *options,
                             pappl_device_t *device, unsigned page) {
  (void)device;
  job_data_t *jd = papplJobGetData(job);
  if (!jd) return false;
  const cups_page_header2_t *hd = &options->header;

  /* The raster covers the whole sheet; the printer takes the imageable area. */
  unsigned fw = hd->cupsWidth, fh = hd->cupsHeight;
  jd->x0 = MARGIN_PX;
  jd->y0 = MARGIN_PX;
  jd->w = fw > 2 * MARGIN_PX ? fw - 2 * MARGIN_PX : fw;
  jd->h = fh > 2 * MARGIN_PX ? fh - 2 * MARGIN_PX : fh;
  if (fw <= 2 * MARGIN_PX) jd->x0 = 0;
  if (fh <= 2 * MARGIN_PX) jd->y0 = 0;
  jd->lines = 0;

  free(jd->ink);
  jd->ink = calloc(jd->w, 1);
  if (!jd->ink) return false;

  /* The paper size is the one of the page received: a client that sends a
   * raster often leaves "media" out of the request, which then holds the
   * printer's default. */
  int width_hmm = (int)((long long)fw * 2540 / RESOLUTION);
  int length_hmm = (int)((long long)fh * 2540 / RESOLUTION);
  pwg_media_t *pwg = pwgMediaForSize(width_hmm, length_hmm);
  const char *size_name = pwg ? pwg->pwg : options->media.size_name;
  const char *size = map_lookup(sizes, COUNT(sizes), size_name);
  const char *type = map_lookup(types, COUNT(types), options->media.type);
  const char *source = map_lookup(sources, COUNT(sources), options->media.source);

  memset(&jd->pg, 0, sizeof jd->pg);
  jd->pg.width = jd->w;
  jd->pg.height = jd->h;
  jd->pg.page_w_pt = (unsigned)((long long)(pwg ? pwg->width : width_hmm) * 72 / 2540);
  jd->pg.page_h_pt = (unsigned)((long long)(pwg ? pwg->length : length_hmm) * 72 / 2540);
  jd->pg.paper = size ? lilo2_paper_code(size) : LILO2_PAPER_UNSPECIFIED;
  jd->pg.tray = lilo2_tray_code(source);
  jd->pg.media = lilo2_media_code(type);
  jd->pg.flags = LILO2_PAGE_NORMAL;
  jd->pg.pageno = (int)page; /* PAPPL counts pages from 1 */

  papplLogJob(job, PAPPL_LOGLEVEL_INFO,
              "page %d: raster %dx%d at %dx%d dpi, %d bits, color space %d, size %s (%s), "
              "source %s, type %s, copies %d",
              (int)page, (int)fw, (int)fh, (int)hd->HWResolution[0], (int)hd->HWResolution[1],
              (int)hd->cupsBitsPerPixel, (int)hd->cupsColorSpace, size_name, size ? size : "?",
              options->media.source, options->media.type, options->copies);

  if (hd->HWResolution[0] != RESOLUTION || hd->HWResolution[1] != RESOLUTION) {
    papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "unsupported resolution %dx%d",
                (int)hd->HWResolution[0], (int)hd->HWResolution[1]);
    return false;
  }
  return lilo2_page_begin(&jd->pg, &jd->out) == 0;
}

static bool sp211_rwriteline(pappl_job_t *job, pappl_pr_options_t *options,
                             pappl_device_t *device, unsigned y, const unsigned char *line) {
  job_data_t *jd = papplJobGetData(job);
  if (!jd) return false;
  if (y < jd->y0 || y - jd->y0 >= jd->h) return true; /* outside the imageable area */

  const cups_page_header2_t *hd = &options->header;
  unsigned bpp = hd->cupsBitsPerPixel;
  /* white-is-high spaces (sGray, W) store paper as 255 or as a set bit */
  int white_high = hd->cupsColorSpace == CUPS_CSPACE_SW || hd->cupsColorSpace == CUPS_CSPACE_W;

  /* missing lines (none expected) are left white */
  while (jd->lines < y - jd->y0) {
    memset(jd->ink, 0, jd->w);
    lilo2_page_line(&jd->pg, jd->ink);
    jd->lines++;
  }

  for (unsigned x = 0; x < jd->w; x++) {
    unsigned sx = x + jd->x0, v;
    if (sx >= hd->cupsWidth) { jd->ink[x] = 0; continue; }
    if (bpp == 1) {
      v = (line[sx >> 3] >> (7 - (sx & 7)) & 1) ? 255 : 0;
    } else {
      v = line[sx * (bpp / 8)];
    }
    jd->ink[x] = (unsigned char)(white_high ? 255 - v : v);
  }
  lilo2_page_line(&jd->pg, jd->ink);
  jd->lines++;

  /* keep the buffer small: send each band once the encoder has written it */
  if (jd->out.len >= 65536) return send_out(device, jd);
  return true;
}

static bool sp211_rendpage(pappl_job_t *job, pappl_pr_options_t *options,
                           pappl_device_t *device, unsigned page) {
  (void)options;
  (void)page;
  job_data_t *jd = papplJobGetData(job);
  if (!jd) return false;
  memset(jd->ink, 0, jd->w);
  while (jd->lines < jd->h) { /* the encoder needs every line of the page */
    lilo2_page_line(&jd->pg, jd->ink);
    jd->lines++;
  }
  lilo2_page_end(&jd->pg);
  return send_out(device, jd);
}

static bool sp211_rendjob(pappl_job_t *job, pappl_pr_options_t *options,
                          pappl_device_t *device) {
  (void)options;
  job_data_t *jd = papplJobGetData(job);
  if (!jd) return false;
  lilo2_job_end(&jd->out);
  bool ok = send_out(device, jd);
  free_job_data(job, jd);
  return ok;
}

/* A document already in the printer's own format is sent unchanged. */
static bool sp211_printfile(pappl_job_t *job, pappl_pr_options_t *options,
                            pappl_device_t *device) {
  (void)options;
  FILE *f = fopen(papplJobGetFilename(job), "rb");
  if (!f) return false;
  unsigned char buf[65536];
  size_t n;
  bool ok = true;
  while (ok && (n = fread(buf, 1, sizeof buf, f)) > 0)
    ok = papplDeviceWrite(device, buf, n) == (ssize_t)n;
  fclose(f);
  return ok;
}

/* ---- printer status ---------------------------------------------------- */

#define STATUS_MIN_INTERVAL 30 /* seconds between two reads of the printer */
#define STATUS_TIMER 60        /* seconds between two reads without any client */

static pthread_mutex_t status_lock = PTHREAD_MUTEX_INITIALIZER;
static time_t status_last; /* last read attempt (one printer per application) */

static void set_toner(pappl_printer_t *printer, int level) {
  pappl_supply_t s;
  memset(&s, 0, sizeof s);
  s.color = PAPPL_SUPPLY_COLOR_BLACK;
  papplCopyString(s.description, "Black Toner", sizeof s.description);
  s.is_consumed = true;
  s.level = level; /* -1: unknown */
  s.type = PAPPL_SUPPLY_TYPE_TONER_CARTRIDGE;
  papplPrinterSetSupplies(printer, 1, &s);
}

/* Reads toner and page counter from the printer and publishes them. Not
 * while a job prints: reading the registers then has not been tested. */
static void update_status(pappl_printer_t *printer, bool force) {
  if (papplPrinterGetState(printer) == IPP_PSTATE_PROCESSING) return;
  time_t now = time(NULL);
  pthread_mutex_lock(&status_lock);
  if (!force && status_last && now - status_last < STATUS_MIN_INTERVAL) {
    pthread_mutex_unlock(&status_lock);
    return;
  }
  status_last = now;

  sp211_counters c;
  sp211_counters_init(&c);
  if (!sp211_usb_read_counters(papplPrinterGetDeviceURI(printer), 0, &c)) {
    /* switched off, unplugged, or no permission on the USB device */
    papplPrinterSetReasons(printer, PAPPL_PREASON_OFFLINE, PAPPL_PREASON_NONE);
    pthread_mutex_unlock(&status_lock);
    return;
  }
  pappl_preason_t add = PAPPL_PREASON_NONE, remove = PAPPL_PREASON_OFFLINE;
  if (c.toner_percent >= 0) {
    set_toner(printer, c.toner_percent);
    if (c.toner_percent <= SP211_TONER_LOW_PERCENT) add |= PAPPL_PREASON_TONER_LOW;
    else remove |= PAPPL_PREASON_TONER_LOW;
  }
  papplPrinterSetReasons(printer, add, remove);

  /* printer-impressions-completed follows the printer's own counter. PAPPL
   * also adds the pages of each job: the difference brings it back. */
  if (c.have_pages) {
    int delta = (int)c.page_counter - papplPrinterGetImpressionsCompleted(printer);
    if (delta) papplPrinterSetImpressionsCompleted(printer, delta);
  }
  pthread_mutex_unlock(&status_lock);
}

static bool app_status_cb(pappl_printer_t *printer) {
  update_status(printer, false);
  return true;
}

static void status_each_printer(pappl_printer_t *printer, void *data) {
  (void)data;
  if (!strcmp(papplPrinterGetDriverName(printer), DRIVER_NAME)) update_status(printer, true);
}

static bool status_timer(pappl_system_t *system, void *data) {
  (void)data;
  papplSystemIteratePrinters(system, status_each_printer, NULL);
  return true;
}

/* ---- printer description ------------------------------------------------- */

static void set_media(pappl_media_col_t *m, const char *size, const char *source) {
  pwg_media_t *pwg = pwgMediaForPWG(size);
  memset(m, 0, sizeof *m);
  papplCopyString(m->size_name, size, sizeof m->size_name);
  if (pwg) {
    m->size_width = pwg->width;
    m->size_length = pwg->length;
  }
  m->bottom_margin = m->left_margin = m->right_margin = m->top_margin = MARGIN_HMM;
  papplCopyString(m->source, source, sizeof m->source);
  papplCopyString(m->type, "stationery", sizeof m->type);
}

static bool driver_cb(pappl_system_t *system, const char *driver_name, const char *device_uri,
                      const char *device_id, pappl_pr_driver_data_t *data, ipp_t **attrs,
                      void *cbdata) {
  (void)system;
  (void)device_uri;
  (void)device_id;
  (void)attrs;
  (void)cbdata;
  if (strcmp(driver_name, DRIVER_NAME)) return false;

  data->printfile_cb = sp211_printfile;
  data->rstartjob_cb = sp211_rstartjob;
  data->rstartpage_cb = sp211_rstartpage;
  data->rwriteline_cb = sp211_rwriteline;
  data->rendpage_cb = sp211_rendpage;
  data->rendjob_cb = sp211_rendjob;
  data->status_cb = app_status_cb;
  data->format = "application/vnd.ricoh-lilo2";

  papplCopyString(data->make_and_model, DRIVER_DESC, sizeof data->make_and_model);
  data->ppm = 22;
  data->kind = PAPPL_KIND_DOCUMENT;
  data->has_supplies = true;

  data->color_supported = PAPPL_COLOR_MODE_AUTO | PAPPL_COLOR_MODE_MONOCHROME;
  data->color_default = PAPPL_COLOR_MODE_MONOCHROME;
  data->content_default = PAPPL_CONTENT_AUTO;
  data->quality_default = IPP_QUALITY_NORMAL;
  data->scaling_default = PAPPL_SCALING_AUTO;
  /* Gray in, so that the encoder dithers exactly like Ricoh's driver. */
  data->raster_types = PAPPL_PWG_RASTER_TYPE_BLACK_1 | PAPPL_PWG_RASTER_TYPE_SGRAY_8;
  data->force_raster_type = PAPPL_PWG_RASTER_TYPE_SGRAY_8;

  data->duplex = PAPPL_DUPLEX_NONE; /* manual duplex: not in this version */
  data->sides_supported = PAPPL_SIDES_ONE_SIDED;
  data->sides_default = PAPPL_SIDES_ONE_SIDED;

  data->num_resolution = 1;
  data->x_resolution[0] = data->y_resolution[0] = RESOLUTION;
  data->x_default = data->y_default = RESOLUTION;

  data->left_right = MARGIN_HMM;
  data->bottom_top = MARGIN_HMM;
  data->num_media = (int)COUNT(sizes);
  for (size_t i = 0; i < COUNT(sizes); i++) data->media[i] = sizes[i].ipp;
  data->num_source = (int)COUNT(sources);
  for (size_t i = 0; i < COUNT(sources); i++) data->source[i] = sources[i].ipp;
  data->num_type = (int)COUNT(types);
  for (size_t i = 0; i < COUNT(types); i++) data->type[i] = types[i].ipp;

  set_media(&data->media_default, "iso_a4_210x297mm", "main");
  set_media(&data->media_ready[0], "iso_a4_210x297mm", "main");
  set_media(&data->media_ready[1], "iso_a4_210x297mm", "manual");
  return true;
}

/* True if value is word, ignoring case and trailing spaces. */
static bool id_value_is(const char *value, const char *word) {
  size_t n = strlen(word);
  if (!value || strncasecmp(value, word, n)) return false;
  while (value[n] == ' ') n++;
  return value[n] == '\0';
}

/* Printers found on USB (or by "autoadd") that this application drives. The
 * SP 211 reports "MFG:RICOH ;...;MODEL:SP 211;": a space after RICOH, and the
 * long key names that IEEE 1284 also allows (MANUFACTURER, MODEL). */
static const char *autoadd_cb(const char *device_info, const char *device_uri,
                              const char *device_id, void *data) {
  (void)device_info;
  (void)device_uri;
  (void)data;
  cups_option_t *pairs = NULL;
  int n = device_id ? papplDeviceParseID(device_id, &pairs) : 0;
  const char *mfg = cupsGetOption("MFG", n, pairs);
  const char *mdl = cupsGetOption("MDL", n, pairs);
  if (!mfg) mfg = cupsGetOption("MANUFACTURER", n, pairs);
  if (!mdl) mdl = cupsGetOption("MODEL", n, pairs);
  bool match = id_value_is(mfg, "RICOH") && id_value_is(mdl, "SP 211");
  cupsFreeOptions(n, pairs);
  return match ? DRIVER_NAME : NULL;
}

/* ---- system ------------------------------------------------------------- */

static char state_file[1024];

static pappl_system_t *system_cb(int num_options, cups_option_t *options, void *data) {
  const char *val;
  const char *spooldir = cupsGetOption("spool-directory", num_options, options);
  const char *logfile = cupsGetOption("log-file", num_options, options);
  const char *hostname = cupsGetOption("server-hostname", num_options, options);
  const char *name = cupsGetOption("server-name", num_options, options);
  int port = 0;
  pappl_loglevel_t loglevel = PAPPL_LOGLEVEL_WARN;

  if ((val = cupsGetOption("server-port", num_options, options)) != NULL) port = atoi(val);
  if ((val = cupsGetOption("log-level", num_options, options)) != NULL) {
    if (!strcmp(val, "debug")) loglevel = PAPPL_LOGLEVEL_DEBUG;
    else if (!strcmp(val, "info")) loglevel = PAPPL_LOGLEVEL_INFO;
    else if (!strcmp(val, "error")) loglevel = PAPPL_LOGLEVEL_ERROR;
  }
  if ((val = cupsGetOption("state-file", num_options, options)) != NULL)
    papplCopyString(state_file, val, sizeof state_file);
  else if (spooldir)
    snprintf(state_file, sizeof state_file, "%s/sp211-printer-app.state", spooldir);
  else
    papplCopyString(state_file, "/var/lib/sp211-printer-app.state", sizeof state_file);

  pappl_system_t *system =
      papplSystemCreate(PAPPL_SOPTIONS_MULTI_QUEUE | PAPPL_SOPTIONS_WEB_INTERFACE |
                            PAPPL_SOPTIONS_WEB_LOG | PAPPL_SOPTIONS_WEB_SECURITY,
                        name ? name : "SP 211 Printer App", port, "_print,_universal", spooldir,
                        logfile ? logfile : "-", loglevel,
                        cupsGetOption("auth-service", num_options, options), false);
  if (!system) return NULL;

  papplSystemAddListeners(system, NULL);
  if (hostname) papplSystemSetHostName(system, hostname);
  papplSystemSetPrinterDrivers(system, (int)COUNT(drivers), drivers, autoadd_cb, NULL,
                               driver_cb, data);
  papplSystemSetFooterHTML(system, "Ricoh SP 211 Printer Application " VERSION);
  papplSystemSetSaveCallback(system, (pappl_save_cb_t)papplSystemSaveState, state_file);
  papplSystemLoadState(system, state_file);
  papplSystemAddTimerCallback(system, time(NULL) + 5, STATUS_TIMER, status_timer, NULL);
  return system;
}

int main(int argc, char *argv[]) {
  return papplMainloop(argc, argv, VERSION, NULL, (int)COUNT(drivers), drivers, autoadd_cb,
                       driver_cb, NULL, NULL, system_cb, NULL, NULL);
}
