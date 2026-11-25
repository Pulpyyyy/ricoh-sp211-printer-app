# MQTT and Home Assistant

With an MQTT broker set, `sp211-printer-app` publishes the printer to Home
Assistant: the device appears by itself in the MQTT integration, with more
than the IPP integration can show (jobs, counters, test pages), and a power
switch that a smart plug can follow.

## Setting it up

1. Home Assistant needs an MQTT broker, for example the Mosquitto add-on,
   and the MQTT integration. Give the printer application its own login:
   with the Mosquitto add-on, a Home Assistant user (Settings → People →
   Users) or an entry under `logins` in the add-on's configuration.
2. On the machine of the printer application, edit
   `/etc/default/sp211-printer-app` (as root) and set at least `MQTT_HOST`,
   and `MQTT_USERNAME` and `MQTT_PASSWORD` if the broker asks for a login:

   ```
   MQTT_HOST=192.168.1.52
   MQTT_USERNAME=sp211
   MQTT_PASSWORD=secret
   ```

3. Restart the service, then read its log:

   ```
   sudo systemctl restart sp211-printer-app
   journalctl -u sp211-printer-app -n 30 --no-pager | grep MQTT
   ```

   `MQTT: connected to mqtt://192.168.1.52:1883.` means it works; the table
   under [Log messages](#log-messages) explains the others.
4. The device appears in Home Assistant under Settings → Devices &
   services → MQTT, named after the printer ("SP211").

## The settings file

`/etc/default/sp211-printer-app` is read by systemd when the service starts
(`EnvironmentFile=` in the service), so a change needs a restart of the
service. The package installs it with every line commented out, owned by
root and readable by root only (mode 600), since it holds the MQTT password;
an upgrade keeps your version.

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

### All the settings

| Setting | Default | Values |
|---|---|---|
| `MQTT_HOST` | not set: MQTT is off | The broker: a host name (`ha.lan`), an IPv4 address (`192.168.1.52`) or an IPv6 address in brackets (`[fd00::52]`). Optionally followed by `:PORT` (`192.168.1.52:1884`), optionally preceded by `mqtt://` or `mqtts://` (`mqtts://ha.lan:8883`); `mqtts://` turns TLS on. |
| `MQTT_PORT` | `1883`, or `8883` with TLS | The broker's port, 1 to 65535. Wins over a port written in `MQTT_HOST`. |
| `MQTT_USERNAME` | none | Login on the broker. Not set: no login is sent. |
| `MQTT_PASSWORD` | none | Its password. |
| `MQTT_TLS` | `no`, or `yes` with `mqtts://` | `yes` or `no` (also `true`/`false`, `on`/`off`, `1`/`0`): MQTT over TLS. Wins over the scheme in `MQTT_HOST`. |
| `MQTT_CA_FILE` | the system's authorities (`/etc/ssl/certs`) | With TLS: the certificate (PEM) of the authority that signed the broker's certificate. For a self-signed broker certificate, that certificate itself. |
| `MQTT_CERT_FILE`, `MQTT_KEY_FILE` | none | With TLS, for a broker that asks clients for a certificate: the client's certificate and its private key (PEM, the key without a passphrase). Both or neither. |
| `MQTT_TLS_INSECURE` | `no` | `yes`: do not check that the broker's certificate carries the name or address used in `MQTT_HOST` (the certificate is still checked against the authority). Only for a broker whose certificate does not name it. |
| `MQTT_DISCOVERY_PREFIX` | `homeassistant` | Home Assistant's discovery prefix, if it was changed in the MQTT integration. |
| `MQTT_TOPIC` | `sp211-printer-app/NAME` | Base of this printer's topics; NAME is the printer's name in lower case, other characters replaced by `_` (`SP211` → `sp211`). |
| `PRINTER_WEB_URL` | `http://HOSTNAME:631/` | The "Visit" link of the device in Home Assistant, and the card's link to the web interface. Set it when Home Assistant cannot resolve the machine's name, e.g. `http://192.168.1.14:631/`. |

A value that cannot be read (a port that is not a number, `MQTT_TLS=maybe`,
a certificate file that does not exist or cannot be read) turns MQTT off
with an error in the log naming the setting; printing is not affected.

Examples:

```
# Mosquitto add-on, plain MQTT
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
```

The power settings changed from Home Assistant (auto on, auto off, delay)
are not in this file: they are kept in
`/var/lib/sp211-printer-app/sp211-mqtt.conf`.

### Log messages

`journalctl -u sp211-printer-app | grep MQTT`:

| Message | Meaning |
|---|---|
| `MQTT: broker mqtt://HOST:PORT, user NAME.` | The settings read at start-up. `user (none)`: no `MQTT_USERNAME`. |
| `MQTT: publishing printer "SP211" under BASE.` | The printer was found; its topics start with BASE. |
| `MQTT: connected to mqtt://HOST:PORT.` | Working. |
| `MQTT: connection refused by ...: Connection Refused: not authorised. (check MQTT_USERNAME and MQTT_PASSWORD)` | The broker refuses the login, or none was given and it asks for one. |
| `MQTT: cannot connect to ...: Connection refused` | Nothing listens on that address and port: wrong `MQTT_HOST` or `MQTT_PORT`, or the broker is stopped. |
| `MQTT: cannot connect to ...: No route to host`, `Lookup error.` | The address cannot be reached, or the name cannot be resolved. |
| `MQTT: TLS set-up failed: ...` | A certificate or key could not be loaded. |
| `MQTT: connection lost (...)` | The connection dropped; it is opened again by itself. |
| `MQTT: MQTT_PORT="..." is not valid (see docs/MQTT.md); MQTT is off.` | A setting cannot be read (also `MQTT_HOST`, `MQTT_TLS`, `MQTT_TLS_INSECURE`, the certificate files). |
| `MQTT_HOST is set but this build has no MQTT support.` | The program was built without libmosquitto. |

After a failure the connection is tried again after 2 s, then 4, 8, 16, 32
and every 60 s, without limit. A TLS problem (wrong authority, name not in
the certificate) shows as a lost or refused connection; the broker's own
log usually says more.

## The plug

The printer application does not talk to the plug: it publishes the power
it wants as the switch "Power", and a Home Assistant automation keeps that
switch and the plug in step. Any plug that Home Assistant shows as a switch
works (Zigbee, Wi-Fi, Z-Wave...).

Import the blueprint
[`homeassistant/blueprints/sp211_power.yaml`](../homeassistant/blueprints/sp211_power.yaml)
(Settings → Automations → Blueprints → Import, with the file's URL on
GitHub), create an automation from it, and choose two entities: the
printer's "Power" switch and the plug.

Then:

- **Auto power on** (on by default): a document arrives while the printer is
  off; the application turns "Power" on, the automation turns the plug on,
  and the document prints as soon as the printer answers on USB (it is read
  every 5 s while it starts).
- **Auto power off** (on by default): after "Power-off delay" minutes (15 by
  default) without any job, "Power" goes off and so does the plug. Never
  while a job is active or waiting. "Power-off time" shows when.
- Switching the plug by hand, or "Power" from Home Assistant, is followed
  both ways. Switching it on by hand starts a new delay.

Without the automation, "Power" only shows whether the printer answers,
and nothing is switched (its `linked` attribute stays `false`).

A job that the printer itself holds (paper out, red light) is finished as
far as the application is concerned: auto off can then cut the power after
the delay, and the printer forgets that job.

## Entities

Each entity has a `printer_role` attribute, which the
[CUPS card](https://github.com/Pulpyyyy/cupscard) uses to find it whatever
it is renamed to.

| Entity | `printer_role` | Value |
|---|---|---|
| (device name) | `printer_state` | `idle`, `printing`, `stopped` (a queue paused by someone); attributes `state_reasons`, `state_message`, `accepting_jobs`, `active_jobs` |
| Problem | `printer_problem` | on when a reason is an error or a warning; attribute `reasons` |
| Connection | `printer_online` | on when the printer answers on USB |
| Black Toner | `supply_level` | percent; attributes `type`, `color`, `level_state`, `low_level`, `high_level` |
| Jobs waiting | `queued_jobs` | active jobs |
| Current job | `current_job` | document name, unknown when none |
| Current job ID, state, pages | `current_job_id`, `current_job_state`, `pages_completed` | |
| Last job | `last_job` | document name; attributes `job_state` (`completed`, `canceled`, `aborted`), `pages` |
| Last job time | `last_job_time` | timestamp |
| Page counter | `page_counter` | the printer's lifetime page total |
| Paper misfeeds | `misfeeds` | the printer's misfeed counter, read after each job |
| Power | `power_control` | switch; attribute `linked` |
| Auto power on, Auto power off | `auto_power_on`, `auto_power_off` | switches |
| Power-off delay | `power_off_delay` | minutes, 1 to 240 |
| Power-off time | `power_countdown` | timestamp, unknown when no auto off is planned |
| Print test page | `print_test_page` | button: the printer's own "Test Page" |
| Print maintenance page | `print_maintenance_page` | button: counters, misfeeds, firmware |

The two pages are printed by the printer itself, only when it is idle and
reachable; otherwise the press is ignored and the log says why.

`state_reasons` are IPP keywords with their RFC 8011 severity:
`toner-low-warning`, `toner-empty-error`, `offline-report`, `paused`... A
queue that the application stops because the printer cannot be reached is
reported as `idle` with `offline-report`, not as `stopped`.

## Topics

`BASE` is `sp211-printer-app/NAME` unless `MQTT_TOPIC` is set.

| Topic | Retained | Payload |
|---|---|---|
| `BASE/availability` | yes | `online`, or `offline` (also the last will) |
| `BASE/state` | yes | JSON, below; published when it changes |
| `BASE/set/power`, `BASE/set/auto_on`, `BASE/set/auto_off` | no | `ON` or `OFF` |
| `BASE/set/delay` | no | minutes |
| `BASE/set/print` | no | `test` or `maintenance` |
| `PREFIX/device/ID/config` | yes | Home Assistant device discovery; ID is the USB serial number, or NAME |
| `PREFIX/status` | | listened to: `online` (Home Assistant restarted) publishes everything again |

```json
{"state": "idle", "reasons": ["toner-low-warning"], "problem": true, "message": "",
 "accepting": true, "active": 0, "online": true,
 "toner": 9, "toner_state": "low", "pages_total": 205, "misfeeds": 6,
 "job": null,
 "last_job": {"id": 12, "name": "facture.pdf", "state": "completed", "pages": 2,
              "time": "2026-11-25T20:01:18+00:00"},
 "power": "ON", "linked": true, "auto_on": "ON", "auto_off": "ON", "delay": 15,
 "off_at": "2026-11-25T20:16:18+00:00", "firmware": "V1.06"}
```

`job` has the same fields as `last_job`, without `time`, while a job is
active. `toner`, `pages_total`, `misfeeds` and `off_at` are `null` when
unknown.
