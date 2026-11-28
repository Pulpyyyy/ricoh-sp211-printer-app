# MQTT

With a broker set, `sp211-printer-app` publishes the printer's state over
MQTT and takes commands from it. Any MQTT client can use it: Home
Assistant, Node-RED, openHAB, Jeedom, Domoticz, a script. The messages are
described below in full, so that nothing depends on a particular home
automation system.

For Home Assistant, the printer also announces itself with MQTT discovery:
the device and its entities appear by themselves (see
[Home Assistant](#home-assistant)). Other systems ignore those messages,
and `MQTT_DISCOVERY=no` stops them.

What it gives, on top of IPP: the current job and the last one with its
result, the printer's page counter and paper misfeeds, its own test and
maintenance pages, and a power state with auto power on and auto power off
that a smart plug can follow.

## Setting it up

1. A broker reachable from the printer application's machine, and a login
   on it for the application if the broker asks for one (with Home
   Assistant's Mosquitto add-on: a Home Assistant user, or an entry under
   `logins` in the add-on's configuration; the names `homeassistant` and
   `addons` are reserved by the add-on).
2. On the printer application's machine (where the service runs, not the
   host of a container), edit `/etc/default/sp211-printer-app` as root and
   set `MQTT_HOST`, and `MQTT_USERNAME` and `MQTT_PASSWORD` if the broker
   asks for a login. Remove the `#` in front of the lines you use:

   ```
   MQTT_HOST=192.168.1.52
   MQTT_USERNAME=sp211
   MQTT_PASSWORD=secret
   ```

3. Restart the service and read its log:

   ```
   systemctl restart sp211-printer-app
   journalctl -u sp211-printer-app -n 30 --no-pager | grep MQTT
   ```

   `MQTT: connected to mqtt://192.168.1.52:1883.` means it works;
   [Log messages](#log-messages) explains the others.
4. Watch the messages from any machine with the Mosquitto clients
   (`apt install mosquitto-clients`):

   ```
   mosquitto_sub -h 192.168.1.52 -u sp211 -P secret -v -t 'sp211-printer-app/#'
   ```

## The settings file

`/etc/default/sp211-printer-app` is read by systemd when the service starts
(`EnvironmentFile=` in the service): a change needs a restart of the
service. The package installs it with every setting commented out, owned by
root and readable by root only (mode 600), since it holds the MQTT
password; an upgrade asks before replacing a file you changed.

Its format is systemd's, not a shell script's:

- one setting per line, `NAME=value`, with no space around `=`;
- a line starting with `#` is a comment; there is no comment at the end of
  a line (`MQTT_PORT=1883 # broker` makes the port `1883 # broker`, which is
  refused);
- spaces at the start and the end of a value are dropped; spaces inside it
  are kept;
- without quotes, a backslash keeps the next character as it is: `\\` is
  one backslash;
- between single quotes everything is kept exactly as written, with no
  escape at all: `MQTT_PASSWORD='p@ss word#1$\'` is `p@ss word#1$\`. The
  simplest way to write a password with special characters. It cannot
  contain a single quote;
- a value containing a single quote goes between double quotes, where `\"`,
  `\\`, `` \` `` and `\$` stand for `"`, `\`, `` ` `` and `$`:
  `MQTT_PASSWORD="it's \"ok\""` is `it's "ok"`;
- no `$NAME` is replaced by another variable's value.

Yes/no settings take `yes` or `no`; `true`/`false`, `on`/`off` and `1`/`0`
are accepted too, in any case.

### All the settings

| Setting | Default | Values |
|---|---|---|
| `MQTT_HOST` | not set: MQTT is off | The broker: a host name (`broker.lan`), an IPv4 address (`192.168.1.52`) or an IPv6 address in brackets (`[fd00::52]`). Optionally followed by `:PORT` (`192.168.1.52:1884`), optionally preceded by `mqtt://` or `mqtts://` (`mqtts://broker.lan:8883`); `mqtts://` turns TLS on. |
| `MQTT_PORT` | `1883`, or `8883` with TLS | The broker's port, 1 to 65535. Wins over a port written in `MQTT_HOST`. |
| `MQTT_USERNAME` | none | Login on the broker. Not set: no login is sent. |
| `MQTT_PASSWORD` | none | Its password. |
| `MQTT_TLS` | `no`, or `yes` with `mqtts://` | MQTT over TLS. Wins over the scheme in `MQTT_HOST`. |
| `MQTT_CA_FILE` | the system's authorities (`/etc/ssl/certs`) | With TLS: the certificate (PEM) of the authority that signed the broker's certificate; for a self-signed broker certificate, that certificate itself. |
| `MQTT_CERT_FILE`, `MQTT_KEY_FILE` | none | With TLS, for a broker that asks clients for a certificate: the client's certificate and its private key (PEM, the key without a passphrase). Both or neither. |
| `MQTT_TLS_INSECURE` | `no` | `yes`: do not check that the broker's certificate carries the name or address used in `MQTT_HOST` (the certificate is still checked against the authority). Only for a broker whose certificate does not name it. |
| `MQTT_TOPIC` | `sp211-printer-app/NAME` | Base of this printer's topics, `BASE` below; NAME is the printer's name in lower case, other characters replaced by `_` (`SP211` → `sp211`). |
| `MQTT_DISCOVERY` | `yes` | Home Assistant discovery messages. `no`: none are sent, and a device announced before is removed from Home Assistant. Everything else is the same. |
| `MQTT_DISCOVERY_PREFIX` | `homeassistant` | Home Assistant's discovery prefix, if it was changed in its MQTT integration. |
| `PRINTER_WEB_URL` | `http://HOSTNAME:631/` | Link to the printer's web interface, given to Home Assistant (the device's "Visit" link). Set it when the machine's name does not resolve, e.g. `http://192.168.1.14:631/`. |

A value that cannot be read (a port that is not a number, `MQTT_TLS=maybe`,
a certificate file that does not exist or cannot be read) turns MQTT off
with an error in the log naming the setting; printing is not affected.

Examples:

```
# Plain MQTT with a login
MQTT_HOST=192.168.1.52
MQTT_USERNAME=sp211
MQTT_PASSWORD=secret

# Another port
MQTT_HOST=192.168.1.52
MQTT_PORT=1884

# TLS, broker certificate signed by a public authority
MQTT_HOST=mqtts://mqtt.example.org

# TLS, self-signed broker certificate copied to the printer's machine
MQTT_HOST=mqtts://192.168.1.52:8883
MQTT_CA_FILE=/etc/sp211-printer-app/broker.crt

# Not Home Assistant: no discovery messages
MQTT_HOST=192.168.1.52
MQTT_DISCOVERY=no
```

The power settings changed over MQTT (auto on, auto off, delay) are not in
this file: they are kept in `/var/lib/sp211-printer-app/sp211-mqtt.conf`.

### Log messages

`journalctl -u sp211-printer-app | grep MQTT`:

| Message | Meaning |
|---|---|
| `MQTT: broker mqtt://HOST:PORT, user NAME.` | The settings read at start-up. `user (none)`: no `MQTT_USERNAME`. Ends with `, TLS` and `, no Home Assistant discovery` when those apply. |
| `MQTT: publishing printer "SP211" under BASE.` | The printer was found; its topics start with BASE. |
| `MQTT: connected to mqtt://HOST:PORT.` | Working. |
| `MQTT: connection refused by ...: Connection Refused: not authorised. (check MQTT_USERNAME and MQTT_PASSWORD)` | The broker refuses the login, or none was given and it asks for one. |
| `MQTT: cannot connect to ...: Connection refused` | Nothing listens on that address and port: wrong `MQTT_HOST` or `MQTT_PORT`, or the broker is stopped. |
| `MQTT: cannot connect to ...: No route to host`, `Lookup error.` | The address cannot be reached, or the name cannot be resolved. |
| `MQTT: TLS set-up failed: ...` | A certificate or key could not be loaded. |
| `MQTT: connection lost (...)` | The connection dropped; it is opened again by itself. |
| `MQTT: ignored BASE/set/...="..."` | A command that is not one of those listed under [Commands](#commands). |
| `MQTT: MQTT_PORT="..." is not valid (see docs/MQTT.md); MQTT is off.` | A setting cannot be read (also `MQTT_HOST`, `MQTT_TLS`, `MQTT_TLS_INSECURE`, `MQTT_DISCOVERY`, the certificate files). |
| `MQTT_HOST is set but this build has no MQTT support.` | The program was built without libmosquitto. |

After a failure the connection is tried again after 2 s, then 4, 8, 16, 32
and every 60 s, without limit. A TLS problem (wrong authority, name not in
the certificate) shows as a lost or refused connection; the broker's own
log usually says more.

## Topics

`BASE` is `sp211-printer-app/NAME` (for a printer named SP211:
`sp211-printer-app/sp211`), or `MQTT_TOPIC`.

| Topic | Direction | Retained | Payload |
|---|---|---|---|
| `BASE/availability` | published | yes | `online` while the application is connected; `offline` when it stops (it publishes it, then disconnects). If it crashes or its connection is lost, the broker publishes `offline` in its place (last will), at once when the connection closes, at most 90 s later when it goes silent. |
| `BASE/state` | published | yes | the printer's state, JSON, below |
| `BASE/set/power` | listened to | | `ON` or `OFF` |
| `BASE/set/auto_on` | listened to | | `ON` or `OFF` |
| `BASE/set/auto_off` | listened to | | `ON` or `OFF` |
| `BASE/set/delay` | listened to | | minutes, `1` to `240` |
| `BASE/set/print` | listened to | | `test` or `maintenance` |

Messages are sent with QoS 1. `BASE/availability` says whether the
application is running; whether the printer answers is `online` in the
state.

### State

`BASE/state` is published when something in it changes (it is checked every
2 s), after every command, and on each connection. Example:

```json
{"state": "idle", "reasons": ["toner-low-warning"], "problem": true, "message": "",
 "accepting": true, "active": 0, "online": true,
 "toner": 9, "toner_state": "low", "pages_total": 205, "misfeeds": 6,
 "job": null,
 "last_job": {"id": 12, "name": "invoice.pdf", "state": "completed", "pages": 2,
              "time": "2026-11-25T20:01:18+00:00"},
 "power": "ON", "linked": true, "auto_on": "ON", "auto_off": "ON", "delay": 15,
 "off_at": "2026-11-25T20:16:18+00:00", "firmware": "V1.06"}
```

| Field | Type | Meaning |
|---|---|---|
| `state` | string | `idle`, `printing`, or `stopped`: a queue paused by someone (in the web interface, or over IPP). A queue that the application stops itself because the printer cannot be reached is `idle`, with `online` false. |
| `reasons` | list of strings | IPP printer-state-reasons with their RFC 8011 severity suffix: `toner-low-warning`, `toner-empty-error`, `media-empty-error`, `media-jam-error`, `offline-report`, `paused` for a stopped queue, and `hold-new-jobs-warning` while the queue holds new jobs ("Hold New Jobs" in the web interface: every new document is refused until it is released there). Empty when all is well. |
| `problem` | boolean | true when a reason is an error or a warning (`-report` reasons do not count). |
| `message` | string | reserved, empty. |
| `accepting` | boolean | the queue accepts new jobs. |
| `active` | number | jobs waiting or printing. |
| `online` | boolean | the printer answers on USB (read every minute, every 5 s while it is being switched on). |
| `toner` | number or null | black toner left, percent; null until first read. |
| `toner_state` | string | `ok`, `low` (10 % or less), `empty` (0 %), `unknown`. |
| `pages_total` | number or null | the printer's own lifetime page counter. |
| `misfeeds` | number or null | the printer's paper misfeed counter, read at start-up and after each job. |
| `job` | object or null | the job printing, or the oldest one waiting: `id`, `name` (document name), `state` (`pending`, `pending_held`, `processing`, `processing_stopped`), `pages` (pages printed so far). null when there is none. |
| `last_job` | object or null | the last finished job: `id`, `name`, `state` (`completed`, `canceled`, `aborted`), `pages`, `time` (when it finished, ISO 8601 in UTC). Taken from the application's own job history, not from the printer. |
| `power` | `ON` or `OFF` | the power the printer has or should have; see [Power](#power). |
| `linked` | boolean | a plug follows `power`: true once a `BASE/set/power` command was received since the application started. |
| `auto_on`, `auto_off` | `ON` or `OFF` | the power settings. |
| `delay` | number | minutes without a job before auto power off. |
| `off_at` | string or null | when auto power off will happen, ISO 8601 in UTC; null when it will not (busy, off, auto off disabled, or no plug linked). |
| `firmware` | string or null | the printer's firmware version. |

### Commands

Payloads are plain text, not JSON. `ON` and `OFF` may also be written `yes`/`no`,
`true`/`false`, `1`/`0`, in any case. Anything else is ignored and logged.
The new state follows within 2 s.

| Topic | Payload | Effect |
|---|---|---|
| `BASE/set/power` | `ON`, `OFF` | Tells the application that the printer's plug is (or must be) on or off. Used by whatever drives the plug; see [Power](#power). |
| `BASE/set/auto_on` | `ON`, `OFF` | Auto power on when a document arrives. Kept across restarts. |
| `BASE/set/auto_off` | `ON`, `OFF` | Auto power off after `delay` minutes without a job. Kept across restarts. |
| `BASE/set/delay` | `1` to `240` | Minutes for auto power off; a decimal is rounded, a value outside the range is brought into it. Kept across restarts. |
| `BASE/set/print` | `test`, `maintenance` | The printer prints its own test page, or its maintenance page (counters, misfeeds, firmware), about 10 s after it is asked to. If the printer is off, the request counts as a job: auto power on switches it on. The page is asked for as soon as the printer answers and has nothing else to print; after 3 minutes without that, the request is dropped and the log says why. Waiting for a printer that is off, and switching it on, is implemented but not tested on the printer yet. |

Examples:

```
mosquitto_pub -h 192.168.1.52 -u sp211 -P secret -t sp211-printer-app/sp211/set/print -m test
mosquitto_pub -h 192.168.1.52 -u sp211 -P secret -t sp211-printer-app/sp211/set/delay -m 30
```

## Power

The application never switches anything itself: it publishes the power the
printer should have (`power` in the state), and something else switches
the plug and reports back. It decides:

- **auto power on** (`auto_on`, on by default): a job waits and the printer
  does not answer → `power` becomes `ON`. The application then reads the
  printer every 5 s and sends the job as soon as it answers;
- **auto power off** (`auto_off`, on by default): no job for `delay`
  minutes (15 by default) → `power` becomes `OFF`. Never while a job is
  printing or waiting; `off_at` says when;
- a `BASE/set/power` command with `ON` starts a new delay, so that a printer
  switched on by hand is not switched off at once.

To link a plug, whatever the system:

1. when `power` in `BASE/state` changes, switch the plug to that value;
2. when the plug changes by other means (by hand, another automation),
   publish its state to `BASE/set/power`;
3. when `BASE/availability` becomes `online` (the application started),
   publish the plug's state to `BASE/set/power`, even if nothing changed:
   it is how the application learns that a plug follows it (`linked`).

Until the first `BASE/set/power` after it starts, the application does not
decide anything: `linked` is false and `power` only shows whether the
printer answers.

A job that the printer itself holds (paper out, red light) is finished as
far as the application is concerned: auto power off can then cut the power
after the delay, and the printer forgets that job.

In Home Assistant, the blueprint below does the three steps.

## Home Assistant

### Discovery

With `MQTT_DISCOVERY` left at `yes`, the application publishes one device
discovery message, `PREFIX/device/ID/config` (retained; `PREFIX` is
`homeassistant`, `ID` the printer's USB serial number in lower case, or
NAME). Home Assistant then shows a device named after the printer, with the
entities below, under Settings → Devices & services → MQTT. It listens to
`PREFIX/status` and publishes everything again when Home Assistant comes
back `online`.

The entities read `BASE/state` and send the commands above: nothing in
them is specific to Home Assistant except their names. Each one carries
its role in a `printer_role` attribute, which the CUPS card uses to find it
whatever it is renamed to.

| Entity | Type | `printer_role` | From |
|---|---|---|---|
| (device name) | sensor, `idle`/`printing`/`stopped` | `printer_state` | `state`; attributes `state_reasons`, `state_message`, `accepting_jobs`, `active_jobs` |
| Problem | binary sensor, problem | `printer_problem` | `problem`; attribute `reasons` (without `-report`) |
| Connection | binary sensor, connectivity | `printer_online` | `online` |
| Black Toner | sensor, % | `supply_level` | `toner`; attributes `type`, `color`, `level_state`, `low_level`, `high_level` |
| Jobs waiting | sensor | `queued_jobs` | `active` |
| Current job | sensor | `current_job` | `job.name`, unknown when none |
| Current job ID | sensor | `current_job_id` | `job.id`, 0 when none |
| Current job state | sensor | `current_job_state` | `job.state`, `idle` when none |
| Current job pages | sensor | `pages_completed` | `job.pages` |
| Last job | sensor | `last_job` | `last_job.name`; attributes `job_state`, `pages` |
| Last job time | sensor, timestamp | `last_job_time` | `last_job.time` |
| Page counter | sensor, total increasing | `page_counter` | `pages_total` |
| Paper misfeeds | sensor, total increasing | `misfeeds` | `misfeeds` |
| Power | switch | `power_control` | `power`, `set/power`; attribute `linked` |
| Auto power on | switch | `auto_power_on` | `auto_on`, `set/auto_on` |
| Auto power off | switch | `auto_power_off` | `auto_off`, `set/auto_off` |
| Power-off delay | number, minutes | `power_off_delay` | `delay`, `set/delay` |
| Power-off time | sensor, timestamp | `power_countdown` | `off_at` |
| Print test page | button | `print_test_page` | `set/print` `test` |
| Print maintenance page | button | `print_maintenance_page` | `set/print` `maintenance` |

All are unavailable while `BASE/availability` is `offline`.

### The plug: blueprint

[`homeassistant/blueprints/sp211_power.yaml`](../homeassistant/blueprints/sp211_power.yaml)
does the three steps of [Power](#power) for any plug that Home Assistant
shows as a switch (Zigbee, Wi-Fi, Z-Wave...). Import it (Settings →
Automations & scenes → Blueprints → Import blueprint) with

```
https://github.com/Pulpyyyy/ricoh-sp211-printer-app/blob/main/homeassistant/blueprints/sp211_power.yaml
```

then create an automation from it and choose two entities: the printer's
"Power" switch and the plug.

### Without discovery

With `MQTT_DISCOVERY=no`, Home Assistant can still use the printer through
MQTT entities written in YAML against the topics above, for example:

```yaml
mqtt:
  sensor:
    - name: SP211 toner
      state_topic: sp211-printer-app/sp211/state
      value_template: "{{ value_json.toner }}"
      unit_of_measurement: "%"
      availability_topic: sp211-printer-app/sp211/availability
```
