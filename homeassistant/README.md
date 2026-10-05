# Home Assistant entities for templog

[`templog.yaml`](templog.yaml) defines Home Assistant entities for the MQTT
topics the firmware publishes and for the one it subscribes to, so the
controller can be watched and its mode changed from a dashboard rather than
only from the app over BLE.

It lives here rather than only in Home Assistant's configuration directory
because every topic and payload in it comes from [`../src/main.c`](../src/main.c):
keeping the two side by side is what stops them drifting apart.

## What it defines

One Home Assistant device, `templog`, with twelve entities:

| Entity | Topic | What it is |
| --- | --- | --- |
| `binary_sensor.templog_online` | `temp/1/status` | `start` on connect, `disconnected` as the last will. Also the availability topic for everything below |
| `binary_sensor.templog_heater` | `temp/1/heater` | Whether the burner is running |
| `select.templog_heater_mode` | `temp/1/heater/mode` ↔ `…/mode/set` | Reads and sets the mode |
| `button.templog_heat_once` | `temp/1/heater/mode/set` | One heating cycle, then off until started |
| `sensor.templog_heater_on_threshold` | `temp/1/heater/on_c` | What the thermostat switches on at |
| `sensor.templog_heater_off_threshold` | `temp/1/heater/off_c` | …and off at |
| `sensor.templog_shunt_state` | `temp/1/shunt/state` | `off`, `running`, `holding` or `manual` |
| `sensor.templog_shunt_setpoint` | `temp/1/shunt/setpoint` | Supply temperature the curve asks for |
| `sensor.templog_shunt_bursts` | `temp/1/shunt/bursts` | Corrections in a row, positive towards warmer |
| `sensor.templog_read_errors_*` ×3 | `temp/errors/<rom code>` | Failed reads per sensor since boot |

The four modes the select offers, which are also the payloads the command topic
takes, are `auto`, `off_until_conditions`, `off_until_started` and `heat_once`.
`auto` and `heat_once` are refused unless a sensor in the `water` role has
reported recently — that reading is the only thing that ever stops the heater —
and a refusal leaves the mode untouched. There is no reply topic: the outcome is
the mode that comes back on `temp/1/heater/mode`.

**The three DS18B20 temperatures are not defined here.** They already exist as
`sensor.panna_temperatur`, `sensor.framledning_temperatur` and
`sensor.utomhus_temperatur`, created by the *Uppstart MQTT Config* automation
publishing MQTT discovery configs whose `unique_id`s are the bare ROM codes.
Defining them again would collide on those ids and risk orphaning their history,
so they stay as they are — which does mean they sit outside the `templog` device.

## Installing it

[`install.sh`](install.sh) does it. Read it first — it edits
`configuration.yaml` and restarts the container — then:

```bash
./install.sh --dry-run   # says what it would do, changes nothing
./install.sh             # does it, asking before the restart
```

It copies `templog.yaml` into `<ha-config>/packages/`, appends a
`homeassistant: packages: !include_dir_named packages` block to
`configuration.yaml` after backing that file up, checks the result, and
restarts. Every step is skipped if it has already been done, so re-running is
safe; if a check fails the backup goes back and the container is left alone.

By hand it is four steps, with `~/certbotazure/homeassistant/config` as the
config directory:

1. `mkdir -p <ha-config>/packages && cp templog.yaml <ha-config>/packages/`
2. Add to `<ha-config>/configuration.yaml` — there is no `homeassistant:` key
   yet, so the whole block is new:

   ```yaml
   homeassistant:
     packages: !include_dir_named packages
   ```
3. Check it (see below).
4. Restart Home Assistant (*Developer Tools → Restart*, or
   `docker restart homeassistant`).

After the first install, editing the copy needs no restart: *Developer Tools →
YAML → Manually configured MQTT entities → Reload* is enough. Only the
`packages:` key itself is read at startup.

### Checking it before restarting

`check_config` on its own is not enough, which is worth knowing before trusting
it:

```bash
docker exec homeassistant python -m homeassistant --script check_config -c /config
```

It catches a YAML mistake, and a package naming an integration that does not
exist — but it reports the latter with the words `Incorrect config` while still
**exiting 0**, so its exit status cannot be the only gate. It also does not look
inside `mqtt:` at all: an invalid `device_class` passes it as
`Successful config`. On this installation it additionally prints a handful of
pre-existing `ERROR` lines about automations referencing an unresolvable BTHome
device, which have nothing to do with this package.

What actually validates the entities is the per-platform schema the MQTT
integration itself uses:

```bash
docker exec -i homeassistant python - /config/packages/templog.yaml <<'PY'
import importlib, sys
import voluptuous as vol, yaml
config = yaml.safe_load(open(sys.argv[1]))["mqtt"]
for platform, entities in config.items():
    schema = importlib.import_module(
        f"homeassistant.components.mqtt.{platform}").PLATFORM_SCHEMA_MODERN
    for entity in entities:
        try:
            schema(dict(entity))
        except vol.Invalid as err:
            print(f"{platform}/{entity.get('name')}: {err}")
PY
```

`install.sh` runs both.

### Avoiding the copy

Step 1 leaves two copies to keep in step. Bind-mounting the file instead keeps
one, at the cost of editing the shared compose file and recreating the
container. Add to the `homeassistant` service's `volumes:` — an absolute path,
because this repository is outside the compose project directory:

```yaml
- /home/hakan/templog/homeassistant/templog.yaml:/config/packages/templog.yaml:ro
```

then `cd ~/certbotazure && docker compose up -d homeassistant`. The file must
exist before the container is recreated, or Docker creates a directory in its
place. A symlink from inside `config/` would not work either way — it would
point outside the bind mount and dangle inside the container.

## Notes on this broker

The broker is the Zenoh router with the MQTT plugin, not Mosquitto. Two
consequences worth knowing:

- Its `storage_manager` storage on `temp/**` persists **every** publication on
  those topics and replays the last one to new subscribers, whatever the MQTT
  retain flag said. That is why these entities populate immediately on a Home
  Assistant restart. Nothing outside `temp/**` is persisted, which is why the
  discovery configs for the temperature sensors have to be republished by an
  automation on every start — and why defining entities in YAML, as this file
  does, is the more robust option here.
- `retain: false` on the select and the button is load-bearing, not incidental.
  The firmware ignores a retained message on the command topic precisely because
  this broker would replay it on reconnect, and `heat_once` would then start a
  fresh cycle every time the link bounced.

## The dashboard view

[`dashboard-view.yaml`](dashboard-view.yaml) is a view for the default
dashboard. The dashboards here are in UI/storage mode, so it cannot be installed
as a file: open the dashboard, pencil icon, three-dot menu → *Raw configuration
editor*, and paste it into the `views:` list.
