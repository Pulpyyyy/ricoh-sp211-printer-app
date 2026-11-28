/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Test pages for the print encoder (src/lilo2.c). No printer needed.
 *
 *   test_encoder DIR
 *
 * draws synthetic pages and writes, for each of them,
 *   DIR/NAME.prn   a one-page job, set up as sp211-printer-app sets it up
 *   DIR/NAME.pbm   the dots the printer must get, in the layout of lilo2check
 *
 * The pages use integer arithmetic only, so they are the same on every
 * machine. tests/test_encoder.py checks the files; run it with "make test".
 * A change to a page changes its reference: see tests/data/README.md.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dither_matrix.h"
#include "lilo2.h"

#define MARGIN_PX 100 /* as in sp211-printer-app.c */

/* a sheet at 600 dpi */
typedef struct {
  const char *name;             /* PPD keyword */
  const char *prefix;           /* in file names */
  unsigned raster_w, raster_h;  /* whole sheet, pixels */
  unsigned w_pt, h_pt;
} paper_t;

static const paper_t papers[] = {
  {"A4", "a4", 4960, 7016, 595, 841},
  {"Letter", "letter", 5100, 6600, 612, 792},
};

/* imageable area, 8-bit ink values (0 = white, 255 = black) */
typedef struct {
  unsigned w, h;
  unsigned char *ink;
} canvas_t;

static uint32_t next_random(uint32_t *state) { /* xorshift32 */
  uint32_t x = *state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return *state = x;
}

static void fill(canvas_t *c, unsigned x, unsigned y, unsigned w, unsigned h,
                 unsigned char v) {
  for (unsigned j = y; j < c->h && j < y + h; j++)
    for (unsigned i = x; i < c->w && i < x + w; i++)
      c->ink[(size_t)j * c->w + i] = v;
}

/* Lines of made-up characters, rules from 1 to 8 pixels and a frame on the
 * very edges of the imageable area. */
static void draw_text(canvas_t *c) {
  uint32_t rnd = 0x53503231;

  for (unsigned y = 200; y + 400 < c->h; y += 100) {
    unsigned x = 150;
    while (x + 400 < c->w) {
      unsigned letters = 2 + next_random(&rnd) % 8;
      for (unsigned n = 0; n < letters && x + 36 < c->w; n++, x += 36) {
        uint32_t glyph = next_random(&rnd); /* 4 x 7 cells of 7 x 8 pixels */
        for (unsigned bit = 0; bit < 28; bit++)
          if (glyph >> bit & 1) fill(c, x + bit % 4 * 7, y + bit / 4 * 8, 7, 8, 255);
      }
      x += 40; /* space */
    }
  }

  for (unsigned t = 1, y = c->h - 300; t <= 8; t *= 2, y += 40) fill(c, 150, y, c->w - 300, t, 255);
  for (unsigned t = 1, x = 20; t <= 8; t *= 2, x += 25) fill(c, x, 150, t, c->h - 300, 255);
  for (unsigned i = 0; i < c->w && i < c->h; i++) fill(c, i, i, 3, 1, 255);

  fill(c, 0, 0, c->w, 1, 255);
  fill(c, 0, c->h - 1, c->w, 1, 255);
  fill(c, 0, 0, 1, c->h, 255);
  fill(c, c->w - 1, 0, 1, c->h, 255);
}

/* A ramp through every ink value, then 16 flat patches from white to black:
 * all go through the thresholds of dither_matrix.h. */
static void draw_gray(canvas_t *c) {
  unsigned ramp = c->h / 4;
  for (unsigned y = 0; y < ramp; y++)
    for (unsigned x = 0; x < c->w; x++)
      c->ink[(size_t)y * c->w + x] = (unsigned char)((uint64_t)x * 255 / (c->w - 1));

  unsigned pw = c->w / 4, ph = (c->h - ramp) / 4;
  for (unsigned n = 0; n < 16; n++)
    fill(c, n % 4 * pw, ramp + n / 4 * ph, pw, ph, (unsigned char)(n * 17));
}

/* Random gray in squares of 4 pixels: compresses badly, so the encoder
 * produces much data. */
static void draw_heavy(canvas_t *c) {
  uint32_t rnd = 0x52494348;
  for (unsigned y = 0; y < c->h; y += 4)
    for (unsigned x = 0; x < c->w; x += 4)
      fill(c, x, y, 4, 4, (unsigned char)(next_random(&rnd) >> 24));
}

static const struct {
  const char *name;
  void (*draw)(canvas_t *);
} pages[] = {{"text", draw_text}, {"gray", draw_gray}, {"heavy", draw_heavy}};

#define COUNT(a) (sizeof(a) / sizeof *(a))

static FILE *create(const char *dir, const char *prefix, const char *page,
                    const char *ext) {
  char path[1024];
  snprintf(path, sizeof path, "%s/%s-%s.%s", dir, prefix, page, ext);
  FILE *f = fopen(path, "wb");
  if (!f) perror(path);
  return f;
}

/* The halftoning rule applied here, apart from the encoder: a dot is printed
 * where the ink value reaches the threshold. Lines are padded to a multiple
 * of 128 dots, as in the stream. */
static int write_expected(FILE *f, const canvas_t *c) {
  unsigned W = (c->w + 127) / 128 * 128;
  unsigned char *row = malloc(W / 8);
  if (!row) return 0;
  fprintf(f, "P4\n%u %u\n", W, c->h);
  for (unsigned y = 0; y < c->h; y++) {
    memset(row, 0, W / 8);
    for (unsigned x = 0; x < c->w; x++) {
      unsigned v = c->ink[(size_t)y * c->w + x];
      if (v && v >= sp_dither[y % 24][x % 24]) row[x / 8] |= (unsigned char)(0x80 >> x % 8);
    }
    fwrite(row, 1, W / 8, f);
  }
  free(row);
  return !ferror(f);
}

static int write_job(FILE *f, const canvas_t *c, const paper_t *paper) {
  lilo2_buf out = {0};

  /* nothing here depends on the clock or on the user running the test */
  lilo2_job job = {0};
  job.copies = 1;
  job.ctime = 1700000000;
  job.title = "encoder test";
  job.user = "test";
  lilo2_job_header(&out, &job);

  lilo2_page pg;
  memset(&pg, 0, sizeof pg);
  pg.width = c->w;
  pg.height = c->h;
  pg.page_w_pt = paper->w_pt;
  pg.page_h_pt = paper->h_pt;
  pg.paper = lilo2_paper_code(paper->name);
  pg.tray = lilo2_tray_code("Tray1");
  pg.media = lilo2_media_code("PlainRecycled");
  pg.flags = LILO2_PAGE_NORMAL;
  pg.pageno = 1;
  if (lilo2_page_begin(&pg, &out) != 0) {
    lilo2_buf_free(&out);
    return 0;
  }
  for (unsigned y = 0; y < c->h; y++) lilo2_page_line(&pg, c->ink + (size_t)y * c->w);
  lilo2_page_end(&pg);
  lilo2_job_end(&out);

  int ok = fwrite(out.d, 1, out.len, f) == out.len;
  lilo2_buf_free(&out);
  return ok;
}

int main(int argc, char *argv[]) {
  if (argc != 2) {
    fputs("Usage: test_encoder DIR\n", stderr);
    return 1;
  }
  for (size_t p = 0; p < COUNT(papers); p++) {
    for (size_t n = 0; n < COUNT(pages); n++) {
      canvas_t c;
      c.w = papers[p].raster_w - 2 * MARGIN_PX;
      c.h = papers[p].raster_h - 2 * MARGIN_PX;
      c.ink = calloc(c.w, c.h);
      if (!c.ink) {
        fputs("out of memory\n", stderr);
        return 1;
      }
      pages[n].draw(&c);

      FILE *prn = create(argv[1], papers[p].prefix, pages[n].name, "prn");
      FILE *pbm = create(argv[1], papers[p].prefix, pages[n].name, "pbm");
      int ok = prn && pbm && write_job(prn, &c, &papers[p]) && write_expected(pbm, &c);
      if (prn && fclose(prn)) ok = 0;
      if (pbm && fclose(pbm)) ok = 0;
      free(c.ink);
      if (!ok) {
        fprintf(stderr, "cannot write page %s-%s\n", papers[p].prefix, pages[n].name);
        return 1;
      }
    }
  }
  return 0;
}
