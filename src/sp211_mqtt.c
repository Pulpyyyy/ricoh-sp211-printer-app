/* SPDX-License-Identifier: GPL-2.0-or-later */
/* sp211_mqtt - see sp211_mqtt.h and docs/MQTT.md */
#include "sp211_mqtt.h"

#include <stdlib.h>

#ifndef HAVE_MOSQUITTO

void sp211_mqtt_start(pappl_system_t *system, const char *settings_file) {
  (void)settings_file;
  if (getenv("MQTT_HOST"))
    papplLog(system, PAPPL_LOGLEVEL_WARN, "MQTT_HOST is set but this build has no MQTT support.");
}

#else

#include <ctype.h>
#include <errno.h>
#include <mosquitto.h>
#include <stddef.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "sp211_app.h"
#include "sp211_config.h"
#include "sp211_json.h"
#include "sp211_power.h"
#include "sp211_usb.h"

#define DRIVER_NAME "ricoh_sp211"
#define TICK_SECONDS 2       /* how often the printer's state is gathered */
#define RECONNECT_MAX 60     /* seconds between two connection attempts, at most */
#define POLL_SOON_EVERY 30   /* seconds between two "read the printer soon" requests */
#define SYSTEM_CA_PATH "/etc/ssl/certs" /* Debian's certificate authorities (ca-certificates) */

typedef struct {
  int id;
  char name[256];
  ipp_jstate_t state;
  int pages;
  time_t completed;
} job_info_t;

typedef struct {
  pappl_system_t *system;
  struct mosquitto *mosq;

  /* configuration */
  char host[256];
  int port;
  char broker[300];       /* "mqtt(s)://host:port", for the log */
  bool tls;
  bool tls_insecure;      /* do not check the broker's name in its certificate */
  char ca_file[512];      /* "" = the system's certificates */
  char cert_file[512], key_file[512]; /* client certificate, optional */
  char username[128], password[128];
  char prefix[128];       /* discovery prefix */
  char base[256];         /* base topic */
  char web_url[512];
  char settings_file[1024];

  /* the printer, found by its driver */
  char printer_name[128];
  char node[64];          /* printer name as an MQTT/HA identifier */
  char uid[96];           /* stable part of unique ids: USB serial or node */
  char serial[64];

  /* state */
  bool socket_up;         /* a connection to the broker is open or being set up */
  bool connected;         /* the broker accepted it */
  time_t next_connect;
  int backoff;
  bool republish;         /* discovery and state must be sent again */
  char *last_state;
  char *last_discovery;
  sp211_power power;
  char page_request[24];  /* SP211_PAGE_TEST or SP211_PAGE_MAINTENANCE, "" if none */
  time_t last_poll_soon;
} mqtt_t;

/* ---- settings ------------------------------------------------------------ */

static void load_settings(mqtt_t *m) {
  FILE *f = fopen(m->settings_file, "r");
  char line[128];
  if (!f) return;
  while (fgets(line, sizeof line, f)) {
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq++ = '\0';
    long v = strtol(eq, NULL, 10);
    if (!strcmp(line, "auto_on")) m->power.auto_on = v != 0;
    else if (!strcmp(line, "auto_off")) m->power.auto_off = v != 0;
    else if (!strcmp(line, "delay")) m->power.delay_min = sp211_power_clamp_delay(v);
  }
  fclose(f);
}

static void save_settings(mqtt_t *m) {
  char tmp[sizeof m->settings_file + 8];
  snprintf(tmp, sizeof tmp, "%s.new", m->settings_file);
  FILE *f = fopen(tmp, "w");
  if (!f) {
    papplLog(m->system, PAPPL_LOGLEVEL_ERROR, "MQTT: cannot save %s: %s", tmp, strerror(errno));
    return;
  }
  fprintf(f, "auto_on=%d\nauto_off=%d\ndelay=%d\n", m->power.auto_on, m->power.auto_off,
          m->power.delay_min);
  if (fclose(f) || rename(tmp, m->settings_file))
    papplLog(m->system, PAPPL_LOGLEVEL_ERROR, "MQTT: cannot save %s: %s", m->settings_file,
             strerror(errno));
}

/* ---- the printer --------------------------------------------------------- */

typedef struct {
  pappl_printer_t *printer;
} find_t;

static void find_cb(pappl_printer_t *printer, void *data) {
  find_t *f = data;
  if (!f->printer && !strcmp(papplPrinterGetDriverName(printer), DRIVER_NAME)) f->printer = printer;
}

static pappl_printer_t *find_printer(mqtt_t *m) {
  find_t f = {NULL};
  papplSystemIteratePrinters(m->system, find_cb, &f);
  return f.printer;
}

/* Called under the printer's read lock: only reads the job. */
static void current_job_cb(pappl_job_t *job, void *data) {
  job_info_t *j = data;
  ipp_jstate_t st = papplJobGetState(job);
  bool running = st == IPP_JSTATE_PROCESSING || st == IPP_JSTATE_STOPPED;
  bool have_running = j->id && (j->state == IPP_JSTATE_PROCESSING || j->state == IPP_JSTATE_STOPPED);
  /* active jobs come newest first: keep the running one, else the oldest */
  if (have_running && !running) return;
  j->id = papplJobGetID(job);
  papplCopyString(j->name, papplJobGetName(job) ? papplJobGetName(job) : "", sizeof j->name);
  j->state = st;
  j->pages = papplJobGetImpressionsCompleted(job);
}

static void last_job_cb(pappl_job_t *job, void *data) {
  job_info_t *j = data;
  j->id = papplJobGetID(job);
  papplCopyString(j->name, papplJobGetName(job) ? papplJobGetName(job) : "", sizeof j->name);
  j->state = papplJobGetState(job);
  j->pages = papplJobGetImpressionsCompleted(job);
  j->completed = papplJobGetTimeCompleted(job);
}

static const char *job_state_keyword(ipp_jstate_t st) {
  switch (st) {
    case IPP_JSTATE_PENDING: return "pending";
    case IPP_JSTATE_HELD: return "pending_held";
    case IPP_JSTATE_PROCESSING: return "processing";
    case IPP_JSTATE_STOPPED: return "processing_stopped";
    case IPP_JSTATE_CANCELED: return "canceled";
    case IPP_JSTATE_ABORTED: return "aborted";
    case IPP_JSTATE_COMPLETED: return "completed";
    default: return "unknown";
  }
}

/* printer-state-reasons with their RFC 8011 severity suffix. */
static const struct {
  pappl_preason_t bit;
  const char *keyword;
} reasons[] = {
  {PAPPL_PREASON_OTHER, "other-error"},
  {PAPPL_PREASON_COVER_OPEN, "cover-open-error"},
  {PAPPL_PREASON_DOOR_OPEN, "door-open-error"},
  {PAPPL_PREASON_INPUT_TRAY_MISSING, "input-tray-missing-error"},
  {PAPPL_PREASON_MEDIA_EMPTY, "media-empty-error"},
  {PAPPL_PREASON_MEDIA_JAM, "media-jam-error"},
  {PAPPL_PREASON_MEDIA_NEEDED, "media-needed-error"},
  {PAPPL_PREASON_MEDIA_LOW, "media-low-warning"},
  {PAPPL_PREASON_TONER_EMPTY, "toner-empty-error"},
  {PAPPL_PREASON_TONER_LOW, "toner-low-warning"},
  {PAPPL_PREASON_MARKER_SUPPLY_EMPTY, "marker-supply-empty-error"},
  {PAPPL_PREASON_MARKER_SUPPLY_LOW, "marker-supply-low-warning"},
  {PAPPL_PREASON_MARKER_WASTE_FULL, "marker-waste-full-error"},
  {PAPPL_PREASON_MARKER_WASTE_ALMOST_FULL, "marker-waste-almost-full-warning"},
  {PAPPL_PREASON_SPOOL_AREA_FULL, "spool-area-full-error"},
  {PAPPL_PREASON_OFFLINE, "offline-report"},
};

static void iso_time(time_t t, char *buf, size_t size) {
  struct tm tm;
  gmtime_r(&t, &tm);
  strftime(buf, size, "%Y-%m-%dT%H:%M:%S+00:00", &tm);
}

/* Gathers the printer's state, runs the power decisions, and writes the
 * retained state message (see docs/MQTT.md). */
static void build_state(mqtt_t *m, pappl_printer_t *printer, sp211_buf *b) {
  ipp_pstate_t pstate = papplPrinterGetState(printer);
  pappl_preason_t preasons = papplPrinterGetReasons(printer);
  bool accepting = papplPrinterIsAcceptingJobs(printer);
  int active = papplPrinterGetNumberOfActiveJobs(printer);
  job_info_t job, last;
  memset(&job, 0, sizeof job);
  memset(&last, 0, sizeof last);
  if (active > 0) papplPrinterIterateActiveJobs(printer, current_job_cb, &job, 1, 0);
  papplPrinterIterateCompletedJobs(printer, last_job_cb, &last, 1, 1);

  sp211_app_status st;
  sp211_app_get_status(&st);
  bool online = st.read ? st.online : !(preasons & PAPPL_PREASON_OFFLINE);
  bool busy = active > 0 || pstate == IPP_PSTATE_PROCESSING;
  time_t now = time(NULL);

  if (sp211_power_step(&m->power, online, busy, now) && m->power.linked)
    papplLogPrinter(printer, PAPPL_LOGLEVEL_INFO, "Power %s (%s).", m->power.on ? "on" : "off",
                    m->power.on ? "a job is waiting" : "nothing to print");
  if (m->power.linked && m->power.on && !online && busy && now - m->last_poll_soon >= POLL_SOON_EVERY) {
    sp211_app_poll_soon(); /* the printer is being switched on: watch for it */
    m->last_poll_soon = now;
  }

  /* A queue stopped by PAPPL or by this application because the printer
   * cannot be reached is a printer that is off, not a queue paused by a
   * person: report it as idle (offline), and "paused" only otherwise. */
  const char *state = pstate == IPP_PSTATE_PROCESSING ? "printing" : "idle";
  bool paused = pstate == IPP_PSTATE_STOPPED && online && !st.stopped_offline;
  if (paused) state = "stopped";

  bool problem = false;
  sp211_buf r = {0};
  sp211_buf_cat(&r, "[");
  bool first = true;
  if (paused) {
    sp211_buf_cat(&r, "\"paused\"");
    first = false;
    problem = true;
  }
  for (size_t i = 0; i < sizeof reasons / sizeof reasons[0]; i++) {
    if (!(preasons & reasons[i].bit)) continue;
    if (reasons[i].bit == PAPPL_PREASON_OFFLINE && online) continue;
    sp211_buf_printf(&r, "%s\"%s\"", first ? "" : ",", reasons[i].keyword);
    first = false;
    if (!strstr(reasons[i].keyword, "-report")) problem = true;
  }
  if (!online && !(preasons & PAPPL_PREASON_OFFLINE)) {
    sp211_buf_printf(&r, "%s\"offline-report\"", first ? "" : ",");
  }
  sp211_buf_cat(&r, "]");

  const char *toner_state = st.toner_percent < 0 ? "unknown"
                            : st.toner_percent == 0 ? "empty"
                            : st.toner_percent <= SP211_TONER_LOW_PERCENT ? "low"
                                                                         : "ok";
  char t[40];
  sp211_buf_printf(b, "{\"state\":\"%s\",\"reasons\":%s,\"problem\":%s,\"message\":\"\"", state,
                   sp211_buf_text(&r), problem ? "true" : "false");
  sp211_buf_free(&r);
  sp211_buf_printf(b, ",\"accepting\":%s,\"active\":%d,\"online\":%s", accepting ? "true" : "false",
                   active, online ? "true" : "false");
  if (st.toner_percent >= 0) sp211_buf_printf(b, ",\"toner\":%d", st.toner_percent);
  else sp211_buf_cat(b, ",\"toner\":null");
  sp211_buf_printf(b, ",\"toner_state\":\"%s\"", toner_state);
  if (st.have_pages) sp211_buf_printf(b, ",\"pages_total\":%u", (unsigned)st.pages);
  else sp211_buf_cat(b, ",\"pages_total\":null");
  if (st.have_misfeeds) sp211_buf_printf(b, ",\"misfeeds\":%u", (unsigned)st.misfeeds);
  else sp211_buf_cat(b, ",\"misfeeds\":null");

  sp211_buf_cat(b, ",\"job\":");
  if (job.id) {
    sp211_buf_printf(b, "{\"id\":%d,\"name\":", job.id);
    sp211_buf_json_string(b, job.name);
    sp211_buf_printf(b, ",\"state\":\"%s\",\"pages\":%d}", job_state_keyword(job.state), job.pages);
  } else {
    sp211_buf_cat(b, "null");
  }
  sp211_buf_cat(b, ",\"last_job\":");
  if (last.id) {
    sp211_buf_printf(b, "{\"id\":%d,\"name\":", last.id);
    sp211_buf_json_string(b, last.name);
    sp211_buf_printf(b, ",\"state\":\"%s\",\"pages\":%d,\"time\":", job_state_keyword(last.state),
                     last.pages);
    if (last.completed > 0) {
      iso_time(last.completed, t, sizeof t);
      sp211_buf_printf(b, "\"%s\"}", t);
    } else {
      sp211_buf_cat(b, "null}");
    }
  } else {
    sp211_buf_cat(b, "null");
  }

  time_t off_at = sp211_power_off_at(&m->power, busy);
  sp211_buf_printf(b, ",\"power\":\"%s\",\"linked\":%s,\"auto_on\":\"%s\",\"auto_off\":\"%s\",\"delay\":%d,\"off_at\":",
                   m->power.on ? "ON" : "OFF", m->power.linked ? "true" : "false",
                   m->power.auto_on ? "ON" : "OFF", m->power.auto_off ? "ON" : "OFF",
                   m->power.delay_min);
  if (off_at) {
    iso_time(off_at, t, sizeof t);
    sp211_buf_printf(b, "\"%s\"", t);
  } else {
    sp211_buf_cat(b, "null");
  }
  sp211_buf_cat(b, ",\"firmware\":");
  sp211_buf_json_string(b, st.firmware[0] ? st.firmware : NULL);
  sp211_buf_cat(b, "}");

  /* a test or maintenance page asked for from Home Assistant */
  if (m->page_request[0]) {
    if (online && !busy && pstate != IPP_PSTATE_STOPPED) {
      bool ok = sp211_usb_print_internal_page(papplPrinterGetDeviceURI(printer), m->page_request);
      papplLogPrinter(printer, ok ? PAPPL_LOGLEVEL_INFO : PAPPL_LOGLEVEL_ERROR,
                      ok ? "Printing the printer's %s page." : "The printer refused the %s page.",
                      strcmp(m->page_request, SP211_PAGE_TEST) ? "maintenance" : "test");
    } else {
      papplLogPrinter(printer, PAPPL_LOGLEVEL_WARN,
                      "Internal page not printed: the printer is %s.",
                      online ? "busy" : "not reachable");
    }
    m->page_request[0] = '\0';
  }
}

/* ---- discovery ------------------------------------------------------------ */

/* Appends one component of the device discovery message. extra: more
 * "key":value pairs, starting with a comma, or "". */
static void component(mqtt_t *m, sp211_buf *b, bool *first, const char *key,
                      const char *platform, const char *name, const char *role,
                      const char *value_template, const char *extra) {
  char topic[sizeof m->base + 16];
  sp211_buf_printf(b, "%s\"%s\":{\"platform\":\"%s\",\"unique_id\":\"sp211pa_%s_%s\",\"name\":",
                   *first ? "" : ",", key, platform, m->uid, key);
  *first = false;
  sp211_buf_json_string(b, name);
  snprintf(topic, sizeof topic, "%s/availability", m->base);
  sp211_buf_cat(b, ",\"availability_topic\":");
  sp211_buf_json_string(b, topic);
  snprintf(topic, sizeof topic, "%s/state", m->base);
  if (value_template) {
    sp211_buf_cat(b, ",\"state_topic\":");
    sp211_buf_json_string(b, topic);
    sp211_buf_cat(b, ",\"value_template\":");
    sp211_buf_json_string(b, value_template);
  }
  /* the card finds each entity's role in the printer_role attribute */
  sp211_buf_cat(b, ",\"json_attributes_topic\":");
  sp211_buf_json_string(b, topic);
  char tpl[640];
  if (!strcmp(role, "printer_state"))
    snprintf(tpl, sizeof tpl,
             "{{ {'printer_role': 'printer_state', 'state_reasons': value_json.reasons, "
             "'state_message': value_json.message, 'accepting_jobs': value_json.accepting, "
             "'active_jobs': value_json.active} | tojson }}");
  else if (!strcmp(role, "printer_problem"))
    snprintf(tpl, sizeof tpl,
             "{{ {'printer_role': 'printer_problem', 'reasons': value_json.reasons | "
             "reject('search', '-report$') | list, 'state_message': value_json.message} | tojson }}");
  else if (!strcmp(role, "supply_level"))
    snprintf(tpl, sizeof tpl,
             "{{ {'printer_role': 'supply_level', 'type': 'toner', 'color': '#000000', "
             "'level_state': value_json.toner_state, 'low_level': %d, 'high_level': 100} | tojson }}",
             SP211_TONER_LOW_PERCENT);
  else if (!strcmp(role, "last_job"))
    snprintf(tpl, sizeof tpl,
             "{{ {'printer_role': 'last_job', 'job_state': value_json.last_job.state if "
             "value_json.last_job else none, 'pages': value_json.last_job.pages if "
             "value_json.last_job else none} | tojson }}");
  else if (!strcmp(role, "power_control"))
    snprintf(tpl, sizeof tpl,
             "{{ {'printer_role': 'power_control', 'linked': value_json.linked} | tojson }}");
  else
    snprintf(tpl, sizeof tpl, "{{ {'printer_role': '%s'} | tojson }}", role);
  sp211_buf_cat(b, ",\"json_attributes_template\":");
  sp211_buf_json_string(b, tpl);
  sp211_buf_cat(b, extra);
  sp211_buf_cat(b, "}");
}

static void command(mqtt_t *m, sp211_buf *b, const char *what) {
  char topic[sizeof m->base + 32];
  snprintf(topic, sizeof topic, "%s/set/%s", m->base, what);
  sp211_buf_cat(b, ",\"command_topic\":");
  sp211_buf_json_string(b, topic);
}

static void build_discovery(mqtt_t *m, const char *firmware, sp211_buf *b) {
  bool first = true;
  sp211_buf_cat(b, "{\"device\":{\"identifiers\":[");
  sp211_buf_printf(b, "\"sp211-printer-app_%s\"],\"name\":", m->uid);
  sp211_buf_json_string(b, m->printer_name);
  sp211_buf_cat(b, ",\"manufacturer\":\"Ricoh\",\"model\":\"SP 211\"");
  if (firmware && firmware[0]) {
    sp211_buf_cat(b, ",\"sw_version\":");
    sp211_buf_json_string(b, firmware);
  }
  if (m->serial[0]) {
    sp211_buf_cat(b, ",\"serial_number\":");
    sp211_buf_json_string(b, m->serial);
  }
  if (m->web_url[0]) {
    sp211_buf_cat(b, ",\"configuration_url\":");
    sp211_buf_json_string(b, m->web_url);
  }
  sp211_buf_cat(b, "},\"origin\":{\"name\":\"sp211-printer-app\",\"sw_version\":\"" SP211_APP_VERSION
                   "\",\"support_url\":\"https://github.com/Pulpyyyy/ricoh-sp211-printer-app\"},");
  sp211_buf_cat(b, "\"components\":{");

  component(m, b, &first, "state", "sensor", NULL, "printer_state", "{{ value_json.state }}",
            ",\"device_class\":\"enum\",\"options\":[\"idle\",\"printing\",\"stopped\"],"
            "\"icon\":\"mdi:printer\"");
  component(m, b, &first, "problem", "binary_sensor", "Problem", "printer_problem",
            "{{ 'ON' if value_json.problem else 'OFF' }}", ",\"device_class\":\"problem\"");
  component(m, b, &first, "online", "binary_sensor", "Connection", "printer_online",
            "{{ 'ON' if value_json.online else 'OFF' }}",
            ",\"device_class\":\"connectivity\",\"entity_category\":\"diagnostic\"");
  component(m, b, &first, "toner", "sensor", "Black Toner", "supply_level", "{{ value_json.toner }}",
            ",\"unit_of_measurement\":\"%\",\"state_class\":\"measurement\",\"icon\":\"mdi:water-opacity\"");
  component(m, b, &first, "queued", "sensor", "Jobs waiting", "queued_jobs", "{{ value_json.active }}",
            ",\"state_class\":\"measurement\",\"icon\":\"mdi:tray-full\"");
  component(m, b, &first, "job", "sensor", "Current job", "current_job",
            "{{ value_json.job.name if value_json.job else None }}", ",\"icon\":\"mdi:file-document-outline\"");
  component(m, b, &first, "job_id", "sensor", "Current job ID", "current_job_id",
            "{{ value_json.job.id if value_json.job else 0 }}",
            ",\"entity_category\":\"diagnostic\",\"icon\":\"mdi:identifier\"");
  component(m, b, &first, "job_state", "sensor", "Current job state", "current_job_state",
            "{{ value_json.job.state if value_json.job else 'idle' }}",
            ",\"device_class\":\"enum\",\"options\":[\"idle\",\"pending\",\"pending_held\",\"processing\","
            "\"processing_stopped\",\"canceled\",\"aborted\",\"completed\",\"unknown\"],"
            "\"icon\":\"mdi:list-status\"");
  component(m, b, &first, "job_pages", "sensor", "Current job pages", "pages_completed",
            "{{ value_json.job.pages if value_json.job else 0 }}", ",\"icon\":\"mdi:file-multiple-outline\"");
  component(m, b, &first, "last_job", "sensor", "Last job", "last_job",
            "{{ value_json.last_job.name if value_json.last_job else None }}", ",\"icon\":\"mdi:history\"");
  component(m, b, &first, "last_job_time", "sensor", "Last job time", "last_job_time",
            "{{ value_json.last_job.time if value_json.last_job and value_json.last_job.time else None }}",
            ",\"device_class\":\"timestamp\"");
  component(m, b, &first, "pages_total", "sensor", "Page counter", "page_counter",
            "{{ value_json.pages_total }}",
            ",\"state_class\":\"total_increasing\",\"unit_of_measurement\":\"pages\","
            "\"entity_category\":\"diagnostic\",\"icon\":\"mdi:counter\"");
  component(m, b, &first, "misfeeds", "sensor", "Paper misfeeds", "misfeeds", "{{ value_json.misfeeds }}",
            ",\"state_class\":\"total_increasing\",\"entity_category\":\"diagnostic\","
            "\"icon\":\"mdi:paper-roll-outline\"");

  sp211_buf extra = {0};
  sp211_buf_cat(&extra, ",\"payload_on\":\"ON\",\"payload_off\":\"OFF\",\"icon\":\"mdi:power-plug\"");
  command(m, &extra, "power");
  component(m, b, &first, "power", "switch", "Power", "power_control", "{{ value_json.power }}",
            sp211_buf_text(&extra));
  sp211_buf_free(&extra);

  sp211_buf_cat(&extra, ",\"payload_on\":\"ON\",\"payload_off\":\"OFF\",\"entity_category\":\"config\","
                        "\"icon\":\"mdi:power-plug-outline\"");
  command(m, &extra, "auto_on");
  component(m, b, &first, "auto_on", "switch", "Auto power on", "auto_power_on",
            "{{ value_json.auto_on }}", sp211_buf_text(&extra));
  sp211_buf_free(&extra);

  sp211_buf_cat(&extra, ",\"payload_on\":\"ON\",\"payload_off\":\"OFF\",\"entity_category\":\"config\","
                        "\"icon\":\"mdi:power-plug-off-outline\"");
  command(m, &extra, "auto_off");
  component(m, b, &first, "auto_off", "switch", "Auto power off", "auto_power_off",
            "{{ value_json.auto_off }}", sp211_buf_text(&extra));
  sp211_buf_free(&extra);

  sp211_buf_printf(&extra, ",\"min\":%d,\"max\":%d,\"step\":1,\"mode\":\"box\",\"unit_of_measurement\":\"min\","
                           "\"entity_category\":\"config\",\"icon\":\"mdi:timer-outline\"",
                   SP211_POWER_DELAY_MIN, SP211_POWER_DELAY_MAX);
  command(m, &extra, "delay");
  component(m, b, &first, "delay", "number", "Power-off delay", "power_off_delay",
            "{{ value_json.delay }}", sp211_buf_text(&extra));
  sp211_buf_free(&extra);

  component(m, b, &first, "off_at", "sensor", "Power-off time", "power_countdown",
            "{{ value_json.off_at }}", ",\"device_class\":\"timestamp\",\"icon\":\"mdi:timer-off-outline\"");

  sp211_buf_cat(&extra, ",\"payload_press\":\"test\",\"icon\":\"mdi:file-document-check-outline\"");
  command(m, &extra, "print");
  component(m, b, &first, "test_page", "button", "Print test page", "print_test_page", NULL,
            sp211_buf_text(&extra));
  sp211_buf_free(&extra);

  sp211_buf_cat(&extra, ",\"payload_press\":\"maintenance\",\"entity_category\":\"diagnostic\","
                        "\"icon\":\"mdi:file-chart-outline\"");
  command(m, &extra, "print");
  component(m, b, &first, "maintenance_page", "button", "Print maintenance page",
            "print_maintenance_page", NULL, sp211_buf_text(&extra));
  sp211_buf_free(&extra);

  sp211_buf_cat(b, "}}");
}

/* ---- MQTT ------------------------------------------------------------------ */

static void publish(mqtt_t *m, const char *topic, const char *payload, bool retain) {
  int rc = mosquitto_publish(m->mosq, NULL, topic, (int)strlen(payload), payload, 1, retain);
  if (rc != MOSQ_ERR_SUCCESS)
    papplLog(m->system, PAPPL_LOGLEVEL_WARN, "MQTT: cannot publish %s: %s", topic, mosquitto_strerror(rc));
}

static void on_connect(struct mosquitto *mosq, void *data, int rc) {
  mqtt_t *m = data;
  if (rc) {
    /* rc 4 and 5: bad user name or password, not authorized (MQTT 3.1.1) */
    papplLog(m->system, PAPPL_LOGLEVEL_ERROR, "MQTT: connection refused by %s: %s%s", m->broker,
             mosquitto_connack_string(rc),
             rc == 4 || rc == 5 ? " (check MQTT_USERNAME and MQTT_PASSWORD)" : "");
    return;
  }
  char topic[sizeof m->base + 16];
  papplLog(m->system, PAPPL_LOGLEVEL_INFO, "MQTT: connected to %s.", m->broker);
  m->connected = true;
  m->backoff = 0;
  snprintf(topic, sizeof topic, "%s/set/+", m->base);
  mosquitto_subscribe(mosq, NULL, topic, 1);
  snprintf(topic, sizeof topic, "%s/status", m->prefix);
  mosquitto_subscribe(mosq, NULL, topic, 1); /* Home Assistant restarted */
  snprintf(topic, sizeof topic, "%s/availability", m->base);
  publish(m, topic, "online", true);
  m->republish = true;
}

static void on_disconnect(struct mosquitto *mosq, void *data, int rc) {
  (void)mosq;
  mqtt_t *m = data;
  if (m->connected)
    papplLog(m->system, PAPPL_LOGLEVEL_WARN, "MQTT: disconnected (%s).", mosquitto_strerror(rc));
  m->connected = false;
}

static bool payload_on(const char *p) {
  return !strcasecmp(p, "ON") || !strcmp(p, "1") || !strcasecmp(p, "true");
}

static void on_message(struct mosquitto *mosq, void *data, const struct mosquitto_message *msg) {
  (void)mosq;
  mqtt_t *m = data;
  char payload[64];
  size_t n = msg->payloadlen > 0 ? (size_t)msg->payloadlen : 0;
  if (n >= sizeof payload) n = sizeof payload - 1;
  if (n) memcpy(payload, msg->payload, n);
  payload[n] = '\0';

  char ha_status[sizeof m->prefix + 8];
  snprintf(ha_status, sizeof ha_status, "%s/status", m->prefix);
  if (!strcmp(msg->topic, ha_status)) {
    if (!strcmp(payload, "online")) m->republish = true;
    return;
  }

  size_t blen = strlen(m->base);
  if (strncmp(msg->topic, m->base, blen) || strncmp(msg->topic + blen, "/set/", 5)) return;
  const char *what = msg->topic + blen + 5;
  time_t now = time(NULL);

  if (!strcmp(what, "power")) {
    sp211_power_set(&m->power, payload_on(payload), now);
  } else if (!strcmp(what, "auto_on")) {
    m->power.auto_on = payload_on(payload);
    save_settings(m);
  } else if (!strcmp(what, "auto_off")) {
    m->power.auto_off = payload_on(payload);
    save_settings(m);
  } else if (!strcmp(what, "delay")) {
    char *end;
    double v = strtod(payload, &end);
    if (end == payload) return;
    m->power.delay_min = sp211_power_clamp_delay((long)(v + 0.5));
    save_settings(m);
  } else if (!strcmp(what, "print")) {
    if (!strcmp(payload, "test")) papplCopyString(m->page_request, SP211_PAGE_TEST, sizeof m->page_request);
    else if (!strcmp(payload, "maintenance"))
      papplCopyString(m->page_request, SP211_PAGE_MAINTENANCE, sizeof m->page_request);
    else return;
  } else {
    return;
  }
  m->republish = true; /* answer at the next tick: Home Assistant waits for the new state */
}

/* Lower-case letters, digits and underscores: a valid MQTT topic level and
 * Home Assistant object id. */
static void identifier(const char *in, char *out, size_t size) {
  size_t n = 0;
  for (; *in && n + 1 < size; in++) {
    unsigned char c = (unsigned char)*in;
    if (isalnum(c)) out[n++] = (char)tolower(c);
    else if (n && out[n - 1] != '_') out[n++] = '_';
  }
  while (n && out[n - 1] == '_') n--;
  out[n] = '\0';
  if (!n) papplCopyString(out, "printer", size);
}

static void setup_printer(mqtt_t *m, pappl_printer_t *printer) {
  papplCopyString(m->printer_name, papplPrinterGetName(printer), sizeof m->printer_name);
  identifier(m->printer_name, m->node, sizeof m->node);
  char serial[64];
  if (sp211_usb_uri_serial(papplPrinterGetDeviceURI(printer), serial, sizeof serial)) {
    papplCopyString(m->serial, serial, sizeof m->serial);
    identifier(serial, m->uid, sizeof m->uid);
  } else {
    papplCopyString(m->uid, m->node, sizeof m->uid);
  }
  const char *topic = getenv("MQTT_TOPIC");
  if (topic && *topic) papplCopyString(m->base, topic, sizeof m->base);
  else snprintf(m->base, sizeof m->base, "sp211-printer-app/%s", m->node);
  size_t len = strlen(m->base);
  while (len && m->base[len - 1] == '/') m->base[--len] = '\0';

  const char *url = getenv("PRINTER_WEB_URL");
  if (url && *url) {
    papplCopyString(m->web_url, url, sizeof m->web_url);
  } else {
    char host[256];
    if (papplSystemGetHostName(m->system, host, sizeof host))
      snprintf(m->web_url, sizeof m->web_url, "http://%s:%d/", host, papplSystemGetHostPort(m->system));
  }
}

/* Opens the connection; the broker's answer arrives in on_connect(). Returns
 * true when the connection is being set up. */
static bool connect_broker(mqtt_t *m) {
  char topic[sizeof m->base + 16];
  int rc;
  if (!m->mosq) {
    char id[128];
    snprintf(id, sizeof id, "sp211-printer-app-%s", m->uid);
    m->mosq = mosquitto_new(id, true, m);
    if (!m->mosq) return false;
    if (m->username[0]) mosquitto_username_pw_set(m->mosq, m->username, m->password[0] ? m->password : NULL);
    if (m->tls) {
      rc = mosquitto_tls_set(m->mosq, m->ca_file[0] ? m->ca_file : NULL,
                             m->ca_file[0] ? NULL : SYSTEM_CA_PATH,
                             m->cert_file[0] ? m->cert_file : NULL,
                             m->key_file[0] ? m->key_file : NULL, NULL);
      if (rc == MOSQ_ERR_SUCCESS && m->tls_insecure) rc = mosquitto_tls_insecure_set(m->mosq, true);
      if (rc != MOSQ_ERR_SUCCESS) {
        papplLog(m->system, PAPPL_LOGLEVEL_ERROR, "MQTT: TLS set-up failed: %s",
                 rc == MOSQ_ERR_ERRNO ? strerror(errno) : mosquitto_strerror(rc));
        mosquitto_destroy(m->mosq);
        m->mosq = NULL;
        return false;
      }
    }
    snprintf(topic, sizeof topic, "%s/availability", m->base);
    mosquitto_will_set(m->mosq, topic, 7, "offline", 1, true);
    mosquitto_connect_callback_set(m->mosq, on_connect);
    mosquitto_disconnect_callback_set(m->mosq, on_disconnect);
    mosquitto_message_callback_set(m->mosq, on_message);
    rc = mosquitto_connect(m->mosq, m->host, m->port, 60);
    if (rc != MOSQ_ERR_SUCCESS)
      papplLog(m->system, PAPPL_LOGLEVEL_ERROR, "MQTT: cannot connect to %s: %s", m->broker,
               rc == MOSQ_ERR_ERRNO ? strerror(errno) : mosquitto_strerror(rc));
  } else {
    rc = mosquitto_reconnect(m->mosq);
    if (rc != MOSQ_ERR_SUCCESS)
      papplLog(m->system, PAPPL_LOGLEVEL_DEBUG, "MQTT: reconnect failed: %s",
               rc == MOSQ_ERR_ERRNO ? strerror(errno) : mosquitto_strerror(rc));
  }
  return rc == MOSQ_ERR_SUCCESS;
}

/* Waits before the next connection attempt: 2, 4, 8... up to RECONNECT_MAX s. */
static void back_off(mqtt_t *m) {
  m->backoff = m->backoff ? m->backoff * 2 : 2;
  if (m->backoff > RECONNECT_MAX) m->backoff = RECONNECT_MAX;
  m->next_connect = time(NULL) + m->backoff;
}

static void publish_if_changed(mqtt_t *m, const char *topic, sp211_buf *b, char **last) {
  if (b->failed) return;
  if (!m->republish && *last && !strcmp(*last, sp211_buf_text(b))) return;
  publish(m, topic, sp211_buf_text(b), true);
  free(*last);
  *last = strdup(sp211_buf_text(b));
}

static void *mqtt_thread(void *data) {
  mqtt_t *m = data;
  pappl_printer_t *printer = NULL;
  time_t next_tick = 0;

  while (!papplSystemIsRunning(m->system) && !papplSystemIsShutdown(m->system)) sleep(1);

  while (!papplSystemIsShutdown(m->system) && papplSystemIsRunning(m->system)) {
    time_t now = time(NULL);
    if (!printer) {
      if ((printer = find_printer(m)) == NULL) {
        sleep(TICK_SECONDS); /* no printer added yet */
        continue;
      }
      setup_printer(m, printer);
      papplLog(m->system, PAPPL_LOGLEVEL_INFO, "MQTT: publishing printer \"%s\" under %s.",
               m->printer_name, m->base);
    }
    if (!m->socket_up && now >= m->next_connect) {
      m->socket_up = connect_broker(m);
      if (!m->socket_up) back_off(m);
    }
    if (m->socket_up) {
      int rc = mosquitto_loop(m->mosq, 250, 1);
      if (rc != MOSQ_ERR_SUCCESS) {
        /* lost, or refused by the broker (on_connect logged why) */
        if (m->connected)
          papplLog(m->system, PAPPL_LOGLEVEL_WARN, "MQTT: connection lost (%s).", mosquitto_strerror(rc));
        m->socket_up = m->connected = false;
        back_off(m);
      }
    } else {
      usleep(250000);
    }

    now = time(NULL);
    if (!m->republish && now < next_tick) continue;
    next_tick = now + TICK_SECONDS;

    /* the printer may have been deleted or replaced */
    pappl_printer_t *current = find_printer(m);
    if (current != printer) {
      printer = NULL;
      continue;
    }

    sp211_buf state = {0};
    build_state(m, printer, &state);
    if (m->connected) {
      char topic[sizeof m->base + 16];
      sp211_app_status st;
      sp211_app_get_status(&st);
      sp211_buf disc = {0};
      build_discovery(m, st.firmware, &disc);
      snprintf(topic, sizeof topic, "%s/device/%s/config", m->prefix, m->uid);
      publish_if_changed(m, topic, &disc, &m->last_discovery);
      sp211_buf_free(&disc);
      snprintf(topic, sizeof topic, "%s/state", m->base);
      publish_if_changed(m, topic, &state, &m->last_state);
      m->republish = false;
    }
    sp211_buf_free(&state);
  }

  /* shutting down: say so, so that Home Assistant shows the printer as
   * unavailable rather than idle */
  if (m->mosq && m->connected) {
    char topic[sizeof m->base + 16];
    snprintf(topic, sizeof topic, "%s/availability", m->base);
    mosquitto_publish(m->mosq, NULL, topic, 7, "offline", 1, true);
    for (int i = 0; i < 4; i++) mosquitto_loop(m->mosq, 100, 1);
    mosquitto_disconnect(m->mosq);
  }
  return NULL;
}

void sp211_mqtt_start(pappl_system_t *system, const char *settings_file) {
  const char *host = getenv("MQTT_HOST");
  if (!host || !*host) return;

  mqtt_t *m = calloc(1, sizeof *m);
  if (!m) return;
  m->system = system;
  const char *v, *bad;
  sp211_broker br;
  if (sp211_parse_broker(host, getenv("MQTT_PORT"), getenv("MQTT_TLS"), &br, &bad) <= 0) {
    if (bad)
      papplLog(system, PAPPL_LOGLEVEL_ERROR, "MQTT: %s=\"%s\" is not valid (see docs/MQTT.md); MQTT is off.",
               bad, getenv(bad) ? getenv(bad) : "");
    free(m);
    return;
  }
  papplCopyString(m->host, br.host, sizeof m->host);
  m->port = br.port;
  m->tls = br.tls;
  /* PAPPL 1.3's log drops the last digit of numbers: the port is logged as text */
  snprintf(m->broker, sizeof m->broker, strchr(m->host, ':') ? "%s://[%s]:%d" : "%s://%s:%d",
           m->tls ? "mqtts" : "mqtt", m->host, m->port);

  int insecure = sp211_parse_bool(getenv("MQTT_TLS_INSECURE"), 0);
  if (insecure < 0) {
    papplLog(system, PAPPL_LOGLEVEL_ERROR, "MQTT: MQTT_TLS_INSECURE=\"%s\" is not valid (yes or no); MQTT is off.",
             getenv("MQTT_TLS_INSECURE"));
    free(m);
    return;
  }
  m->tls_insecure = insecure;
  static const struct {
    const char *name;
    size_t offset;
  } files[] = {
    {"MQTT_CA_FILE", offsetof(mqtt_t, ca_file)},
    {"MQTT_CERT_FILE", offsetof(mqtt_t, cert_file)},
    {"MQTT_KEY_FILE", offsetof(mqtt_t, key_file)},
  };
  for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
    if (!(v = getenv(files[i].name)) || !*v) continue;
    if (access(v, R_OK)) {
      papplLog(system, PAPPL_LOGLEVEL_ERROR, "MQTT: %s=\"%s\": %s; MQTT is off.", files[i].name, v,
               strerror(errno));
      free(m);
      return;
    }
    papplCopyString((char *)m + files[i].offset, v, sizeof m->ca_file);
  }
  if (!m->cert_file[0] != !m->key_file[0]) {
    papplLog(system, PAPPL_LOGLEVEL_ERROR,
             "MQTT: MQTT_CERT_FILE and MQTT_KEY_FILE go together; MQTT is off.");
    free(m);
    return;
  }
  if (!m->tls && (m->ca_file[0] || m->cert_file[0]))
    papplLog(system, PAPPL_LOGLEVEL_WARN,
             "MQTT: certificates are set but TLS is off (use mqtts:// or MQTT_TLS=yes).");

  if ((v = getenv("MQTT_USERNAME")) != NULL) papplCopyString(m->username, v, sizeof m->username);
  if ((v = getenv("MQTT_PASSWORD")) != NULL) papplCopyString(m->password, v, sizeof m->password);
  papplCopyString(m->prefix, (v = getenv("MQTT_DISCOVERY_PREFIX")) && *v ? v : "homeassistant",
                  sizeof m->prefix);
  papplCopyString(m->settings_file, settings_file, sizeof m->settings_file);
  sp211_power_init(&m->power);
  load_settings(m);

  mosquitto_lib_init();
  pthread_t tid;
  if (pthread_create(&tid, NULL, mqtt_thread, m)) {
    papplLog(system, PAPPL_LOGLEVEL_ERROR, "MQTT: cannot start: %s", strerror(errno));
    free(m);
    return;
  }
  pthread_detach(tid);
  papplLog(system, PAPPL_LOGLEVEL_INFO, "MQTT: broker %s, user %s%s.", m->broker,
           m->username[0] ? m->username : "(none)",
           !m->tls ? "" : m->tls_insecure ? ", TLS without name check" : ", TLS");
}

#endif /* HAVE_MOSQUITTO */
