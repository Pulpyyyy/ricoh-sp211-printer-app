/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sp211_mqtt - publishes the printer to Home Assistant over MQTT.
 *
 * Optional: configured with environment variables, read once at start-up
 * (the systemd service reads them from /etc/default/sp211-printer-app):
 *
 *   MQTT_HOST              broker address; MQTT is off when it is not set
 *   MQTT_PORT              default 1883
 *   MQTT_USERNAME, MQTT_PASSWORD
 *   MQTT_DISCOVERY_PREFIX  default "homeassistant"
 *   MQTT_TOPIC             base topic, default "sp211-printer-app/NAME"
 *   PRINTER_WEB_URL        link to the web interface shown in Home Assistant,
 *                          default http://HOSTNAME:PORT/
 *
 * One device is announced with Home Assistant's MQTT discovery: state and
 * problems, toner, counters, current and last job, the printer's own test
 * and maintenance pages, and the power switch with auto on / auto off (see
 * sp211_power.h and homeassistant/blueprints/sp211_power.yaml). Topics and
 * payloads are described in docs/MQTT.md.
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
