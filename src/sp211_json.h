/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sp211_json - a small growing buffer to write JSON (and other text) for the
 * MQTT messages. Out of memory sets failed and stops appending; the caller
 * checks it once at the end. No dependency; tested in tests/test_status.c.
 */
#ifndef SP211_JSON_H
#define SP211_JSON_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
  char *s;     /* NUL-terminated text, NULL until something is written */
  size_t len;
  size_t size;
  bool failed;
} sp211_buf;

void sp211_buf_free(sp211_buf *b);
void sp211_buf_cat(sp211_buf *b, const char *text);
void sp211_buf_printf(sp211_buf *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
/* Appends text as a JSON string, quotes included; NULL gives null. Control
 * characters are escaped; invalid UTF-8 is passed through unchanged. */
void sp211_buf_json_string(sp211_buf *b, const char *text);
/* The text, "" if nothing was written. */
const char *sp211_buf_text(const sp211_buf *b);

#endif
