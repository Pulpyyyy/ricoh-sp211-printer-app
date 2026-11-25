/* SPDX-License-Identifier: GPL-2.0-or-later */
/* sp211_config - see sp211_config.h */
#include "sp211_config.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* A port: 1 to 65535, digits only. */
static int parse_port(const char *s, int *port) {
  char *end;
  if (!isdigit((unsigned char)*s)) return 0;
  long v = strtol(s, &end, 10);
  if (*end || v < 1 || v > 65535) return 0;
  *port = (int)v;
  return 1;
}

/* Copies s without leading and trailing spaces. */
static void trim_copy(const char *s, char *out, size_t size) {
  while (isspace((unsigned char)*s)) s++;
  size_t len = strlen(s);
  while (len && isspace((unsigned char)s[len - 1])) len--;
  if (len >= size) len = size - 1;
  memcpy(out, s, len);
  out[len] = '\0';
}

int sp211_parse_bool(const char *s, int dflt) {
  char v[16];
  if (!s) return dflt;
  trim_copy(s, v, sizeof v);
  if (!v[0]) return dflt;
  if (!strcasecmp(v, "yes") || !strcasecmp(v, "true") || !strcasecmp(v, "on") || !strcmp(v, "1"))
    return 1;
  if (!strcasecmp(v, "no") || !strcasecmp(v, "false") || !strcasecmp(v, "off") || !strcmp(v, "0"))
    return 0;
  return -1;
}

int sp211_parse_broker(const char *host_env, const char *port_env, const char *tls_env,
                       sp211_broker *b, const char **error) {
  char buf[sizeof b->host];
  int port = 0; /* 0: not given */

  memset(b, 0, sizeof *b);
  *error = NULL;
  if (!host_env) return 0;
  trim_copy(host_env, buf, sizeof buf);
  char *h = buf;

  /* scheme */
  if (!strncasecmp(h, "mqtts://", 8)) {
    b->tls = 1;
    h += 8;
  } else if (!strncasecmp(h, "mqtt://", 7)) {
    h += 7;
  } else if (strstr(h, "://")) {
    *error = "MQTT_HOST";
    return -1;
  }
  size_t len = strlen(h);
  while (len && h[len - 1] == '/') h[--len] = '\0'; /* "mqtt://host/" */
  if (!h[0]) {
    if (h != buf) *error = "MQTT_HOST"; /* "mqtt://" alone */
    return h != buf ? -1 : 0;
  }

  if (h[0] == '[') {
    /* "[v6]" or "[v6]:port" */
    char *close = strchr(h, ']');
    if (!close || close == h + 1) {
      *error = "MQTT_HOST";
      return -1;
    }
    *close = '\0';
    const char *rest = close + 1;
    if ((*rest == ':' && !parse_port(rest + 1, &port)) || (*rest && *rest != ':')) {
      *error = "MQTT_HOST";
      return -1;
    }
    h++;
  } else {
    /* "name:port" or "1.2.3.4:port"; several colons: a bare IPv6 address */
    char *colon = strchr(h, ':');
    if (colon && colon == strrchr(h, ':')) {
      *colon = '\0';
      if (!h[0] || !parse_port(colon + 1, &port)) {
        *error = "MQTT_HOST";
        return -1;
      }
    }
  }
  memmove(b->host, h, strlen(h) + 1);

  int tls = sp211_parse_bool(tls_env, b->tls);
  if (tls < 0) {
    *error = "MQTT_TLS";
    return -1;
  }
  b->tls = tls;

  char p[16];
  if (port_env) {
    trim_copy(port_env, p, sizeof p);
    if (p[0] && !parse_port(p, &port)) {
      *error = "MQTT_PORT";
      return -1;
    }
  }
  b->port = port ? port : b->tls ? SP211_MQTTS_PORT : SP211_MQTT_PORT;
  return 1;
}
