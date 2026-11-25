/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sp211_mqtt - publishes the printer over MQTT and takes commands from it.
 *
 * For any MQTT client: the printer's state is published as JSON on
 * BASE/state (retained), commands are read on BASE/set/... and
 * BASE/availability says whether the application runs. Home Assistant also
 * gets a device discovery message, unless MQTT_DISCOVERY=no. The power
 * state with auto on / auto off is decided in sp211_power.c; whatever
 * drives the plug follows it (for Home Assistant, the blueprint in
 * homeassistant/blueprints/sp211_power.yaml).
 *
 * Optional, configured with environment variables read once at start-up
 * (the systemd service reads them from /etc/default/sp211-printer-app):
 * MQTT_HOST (MQTT is off without it), MQTT_PORT, MQTT_USERNAME,
 * MQTT_PASSWORD, MQTT_TLS, MQTT_CA_FILE, MQTT_CERT_FILE, MQTT_KEY_FILE,
 * MQTT_TLS_INSECURE, MQTT_TOPIC, MQTT_DISCOVERY, MQTT_DISCOVERY_PREFIX,
 * PRINTER_WEB_URL. docs/MQTT.md describes settings, topics and payloads.
 *
 * Built without libmosquitto (HAVE_MOSQUITTO undefined) it does nothing.
 */
#ifndef SP211_MQTT_H
#define SP211_MQTT_H

#include <pappl/pappl.h>

/* Starts the MQTT thread if MQTT_HOST is set. settings_file keeps the power
 * settings (auto on, auto off, delay) across restarts. Call once, before
 * papplSystemRun(). */
void sp211_mqtt_start(pappl_system_t *system, const char *settings_file);

#endif
