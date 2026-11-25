/* SPDX-License-Identifier: GPL-2.0-or-later */
/* sp211_json - see sp211_json.h */
#include "sp211_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool reserve(sp211_buf *b, size_t more) {
  if (b->failed) return false;
  if (b->len + more + 1 <= b->size) return true;
  size_t size = b->size ? b->size : 256;
  while (size < b->len + more + 1) size *= 2;
  char *s = realloc(b->s, size);
  if (!s) {
    b->failed = true;
    return false;
  }
  b->s = s;
  b->size = size;
  return true;
}

static void append(sp211_buf *b, const char *text, size_t n) {
  if (!reserve(b, n)) return;
  memcpy(b->s + b->len, text, n);
  b->len += n;
  b->s[b->len] = '\0';
}

void sp211_buf_free(sp211_buf *b) {
  free(b->s);
  memset(b, 0, sizeof *b);
}

void sp211_buf_cat(sp211_buf *b, const char *text) {
  if (text) append(b, text, strlen(text));
}

void sp211_buf_printf(sp211_buf *b, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n < 0 || !reserve(b, (size_t)n)) return;
  va_start(ap, fmt);
  vsnprintf(b->s + b->len, (size_t)n + 1, fmt, ap);
  va_end(ap);
  b->len += (size_t)n;
}

void sp211_buf_json_string(sp211_buf *b, const char *text) {
  if (!text) {
    sp211_buf_cat(b, "null");
    return;
  }
  append(b, "\"", 1);
  for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
    char esc[8];
    switch (*p) {
      case '"': sp211_buf_cat(b, "\\\""); break;
      case '\\': sp211_buf_cat(b, "\\\\"); break;
      case '\n': sp211_buf_cat(b, "\\n"); break;
      case '\r': sp211_buf_cat(b, "\\r"); break;
      case '\t': sp211_buf_cat(b, "\\t"); break;
      default:
        if (*p < 0x20) {
          snprintf(esc, sizeof esc, "\\u%04x", *p);
          sp211_buf_cat(b, esc);
        } else {
          append(b, (const char *)p, 1);
        }
    }
  }
  append(b, "\"", 1);
}

const char *sp211_buf_text(const sp211_buf *b) {
  return b->s ? b->s : "";
}
