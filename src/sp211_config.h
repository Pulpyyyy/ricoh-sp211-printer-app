/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sp211_config - reading the settings of /etc/default/sp211-printer-app.
 * No dependency; tested in tests/test_status.c.
 */
#ifndef SP211_CONFIG_H
#define SP211_CONFIG_H

#include <stddef.h>

#define SP211_MQTT_PORT 1883
#define SP211_MQTTS_PORT 8883

typedef struct {
  char host[256];
  int port;
  int tls; /* MQTT over TLS (mqtts) */
} sp211_broker;

/* The broker from MQTT_HOST, MQTT_PORT and MQTT_TLS:
 *   - MQTT_HOST: a name or an IPv4 address, an IPv6 address in brackets
 *     ("[fd00::5]"), optionally with ":PORT", optionally after "mqtt://" or
 *     "mqtts://" (TLS);
 *   - MQTT_TLS: yes/no (also true/false, on/off, 1/0); wins over the scheme;
 *   - MQTT_PORT: wins over a port in MQTT_HOST;
 *   - otherwise the port is 8883 with TLS, 1883 without.
 * The environment values may be NULL. Returns 0 when MQTT_HOST is empty
 * (MQTT off), -1 when a value is not valid (*error names the variable), 1
 * on success. */
int sp211_parse_broker(const char *host_env, const char *port_env, const char *tls_env,
                       sp211_broker *b, const char **error);

/* yes/true/on/1 -> 1, no/false/off/0 -> 0, empty or NULL -> dflt,
 * anything else -> -1. Case does not matter. */
int sp211_parse_bool(const char *s, int dflt);

#endif
