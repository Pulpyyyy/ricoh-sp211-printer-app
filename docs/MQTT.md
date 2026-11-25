# MQTT and Home Assistant

With an MQTT broker set, `sp211-printer-app` publishes the printer to Home
Assistant: the device appears by itself in the MQTT integration, with more
than the IPP integration can show (jobs, counters, test pages), and a power
switch that a smart plug can follow.

## Setting it up

1. Home Assistant needs an MQTT broker, for example the Mosquitto add-on,
   and the MQTT integration. Create a Home Assistant user for the printer
   application (Settings → People → Users), or use the broker's own users.
2. On the machine of the printer application, in
   `/etc/default/sp211-printer-app`:

   ```
   MQTT_HOST=192.168.1.10
   MQTT_USERNAME=sp211
   MQTT_PASSWORD=secret
   ```

   then `sudo systemctl restart sp211-printer-app`. The log
   (`journalctl -u sp211-printer-app`) shows `MQTT: connected to ...`.
3. The device "SP211" (the printer's name) appears in Settings → Devices,
   integration MQTT.

| Variable | Default | |
|---|---|---|
| `MQTT_HOST` | none: MQTT off | broker address |
| `MQTT_PORT` | `1883` | |
| `MQTT_USERNAME`, `MQTT_PASSWORD` | none | |
| `MQTT_DISCOVERY_PREFIX` | `homeassistant` | Home Assistant's discovery prefix |
| `MQTT_TOPIC` | `sp211-printer-app/NAME` | base topic; NAME is the printer's name in lower case |
| `PRINTER_WEB_URL` | `http://HOSTNAME:631/` | link to the web interface on the device page |

The power settings changed from Home Assistant (auto on, auto off, delay)
are kept in `/var/lib/sp211-printer-app/sp211-mqtt.conf`.

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
