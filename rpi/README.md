# templog BLE bridge (Raspberry Pi)

Service and CLI that let a Raspberry Pi 4/5 talk to the templog ESP32 device
over Bluetooth Low Energy (BLE), robust against flaky BLE connectivity.

## How it works

* The ESP32 advertises as `templog` and exposes a custom GATT service with:
  * a **telemetry** characteristic (read + notify) — live state: Wi-Fi and
    MQTT connection, heater on/off and mode, what the shunt valve control is
    doing, OTA progress,
  * a **sensors** characteristic (read + notify) — the temperatures, each
    with its age and the role it has been given,
  * a **config** characteristic (read + notify) — settings that only change
    on command: SSID, broker URL, heater thresholds, the heating curve, the
    actuator and the indoor topic, plus the running firmware version,
  * a **command** characteristic (write) — set Wi-Fi, set the MQTT broker,
    give a sensor a role, set heater thresholds, force heater off, start
    heater, adjust the heating curve and drive the shunt valve,
  * a **status** characteristic (read + notify) — result of the last command.
* The link is encrypted and bonded (NimBLE "Just Works" pairing + LE Secure
  Connections). Bonds are stored in the ESP32's flash (NVS), so re-pairing is
  only needed once, or after a factory reset.
* `templog-ble` (a systemd service) owns the single BLE connection. It
  reconnects automatically with exponential backoff whenever the link drops
  (out of range, interference, ESP32 reboot, etc.), so it recovers on its own
  once conditions improve.
* `templogctl` is a CLI that talks to `templog-ble` over a local Unix socket
  (`/run/templog-ble.sock`), so multiple local tools can query status /
  send commands without fighting over the single BLE connection.

See [`../src/ble_service.c`](../src/ble_service.c) and
[`../include/ble_service.h`](../include/ble_service.h) for the firmware side
of the protocol, and [`templog_ble/protocol.py`](templog_ble/protocol.py) for
the shared constants.

## Install on the Raspberry Pi

Requires BlueZ (preinstalled on Raspberry Pi OS) and Python 3.9+.

```bash
sudo apt-get install -y python3-pip bluetooth
cd rpi
sudo pip3 install .
sudo cp templog-ble.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now templog-ble
```

### Pairing

The first connection triggers BLE bonding. BlueZ needs a pairing agent
registered to auto-accept "Just Works" requests headlessly:

```bash
bluetoothctl
[bluetoothctl]# agent NoInputNoOutput
[bluetoothctl]# default-agent
[bluetoothctl]# scan on        # confirm you see "templog", then Ctrl-C scan
[bluetoothctl]# pair <ESP32 MAC address>
[bluetoothctl]# trust <ESP32 MAC address>
[bluetoothctl]# exit
```

After that, `templog-ble` will connect automatically (it scans for the
device by advertised name unless `--address` is given).

## CLI usage

```bash
templogctl status

templogctl set-wifi "MyHomeSSID" "supersecret"

templogctl set-mqtt mqtt://192.168.2.10:1883

templogctl set-sensor-role 28ff640000000000 water    # drives the heater thermostat
templogctl set-sensor-role 28aa550000000000 outdoor  # the curve is drawn from this one
templogctl set-sensor-role 28bb660000000000 supply   # the one the shunt valve regulates
templogctl set-sensor-role 28bb660000000000 none     # take a sensor out of service

templogctl set-thresholds --on 60 --off 80

templogctl heater-off                 # resumes automatically once temp drops to/below the on-threshold
templogctl heater-off --until-started # stays off until an explicit heater-on
templogctl heater-on                  # starts heater unless water temp already >= off-threshold

templogctl set-curve --slope 1.2 --offset -1 --target 21 --min 20 --max 70
templogctl set-shunt --on --travel 120 --authority 50
templogctl set-shunt --off             # stop driving the valve, leaving it where it is
templogctl set-indoor --topic home/livingroom/temperature --gain 3 --max-trim 5 --stale 900
templogctl set-indoor --topic ""       # switch the indoor trim off
templogctl shunt-jog warmer 5          # run the actuator by hand, to check the wiring

templogctl ota-update                 # install the latest GitHub release over the ESP32's Wi-Fi
templogctl ota-update --tag v1.2.0    # install a specific release
```

Every `set-curve`, `set-shunt` and `set-indoor` flag is optional: the settings
left out keep their current values.

The three read characteristics exist because a GATT client will not read more
than 512 bytes of a single attribute value — Android's stack truncates there
without saying so, and the sensor readings alone can approach that with eight
sensors on the bus. `templog-ble` reads all three and merges them, so
`templogctl status` still prints one document:

```json
{
  "ok": true,
  "connected": true,
  "telemetry": {
    "t": [
      {"id": "28ff640000000000", "c": 62.3, "age": 1200, "role": "water"},
      {"id": "28aa550000000000", "c": 1.4, "age": 1200, "role": "outdoor"},
      {"id": "28bb660000000000", "c": 43.9, "age": 1200, "role": "supply"}
    ],
    "wifi": {"c": true, "rssi": -55, "disc": 0},
    "mqtt": {"c": true},
    "ssid": "MyHomeSSID",
    "url": "mqtt://192.168.2.10:1883",
    "heater": true,
    "onC": 60.0,
    "offC": 80.0,
    "forceState": 0,
    "shunt": {
      "en": true, "state": "running", "dir": "idle",
      "sp": 44.6, "sup": 43.9, "out": 1.4, "pos": 0.42,
      "in": 20.8, "inAge": 42000, "inFresh": true, "trim": 0.6
    },
    "curve": {
      "slope": 1.2, "offset": -1.0, "target": 21.0, "min": 20.0, "max": 70.0,
      "travel": 120, "authority": 50.0
    },
    "indoor": {
      "topic": "home/livingroom/temperature", "gain": 3.0, "maxTrim": 5.0, "stale": 900
    },
    "fw": "v1.2.0",
    "ota": {"state": "idle"}
  }
}
```

`age` is milliseconds since that sensor's last good reading; a missing `c`/`age`
means the sensor has never produced a valid reading. `onC`/`offC` are the
current heater thresholds and `forceState` is `0` (automatic), `1` (forced
off until start conditions are met again) or `2` (forced off until
explicitly started).

Firmware can also be pushed straight to the device over BLE by the Android
app, for a device whose Wi-Fi is not working; `templogctl` only triggers the
Wi-Fi download.

`fw` is the version of the running firmware. `ota` describes a firmware
update: `state` is one of `idle`, `downloading` (with `pct` progress),
`rebooting`, `uptodate` (the release is the version already running),
`receiving` (an image arriving over BLE, also with `pct`),
`failed` (with `err`) or `verifying` (a freshly installed image that is
rolled back unless it reaches Wi-Fi within two minutes). `ver` is the
version being installed once known.

## Shunt valve control

Two GPIO outputs (18 and 19, which used to drive diagnostic LEDs) run a three
point actuator on the radiator shunt valve: one makes it travel towards warmer,
the other towards colder, and with neither energised it stays put. They are
never energised at once.

Control is open loop with respect to the house, like the panel it replaces. A
heating curve turns the outdoor temperature into a supply ("framledning")
temperature setpoint:

    supply = target + slope × (target − outdoor) + offset + trim

clamped to `min`..`max`. The loop that *is* closed is the one around the supply
sensor: every 20 seconds the actuator is pulsed for a time proportional to the
difference between setpoint and measured supply temperature. Because the
actuator integrates those pulses, the supply temperature settles on the
setpoint without a standing error. `travel` (its end to end run time) and
`authority` (how much supply temperature that whole travel is worth) are what
turn a temperature error into a pulse length; `shunt-jog` is there to measure
the first and try out the second.

`trim` comes from the indoor temperature, which this device does not measure:
it subscribes to `indoorTopic` on the broker and expects a bare number. The
trim is `gain × (target − indoor)`, limited to ±`maxTrim`, and it is applied
**only while the reading is fresher than `stale` seconds** — a sensor that goes
quiet or a broker outage therefore leaves the curve running on its own rather
than leaving the house cold.

The valve is held where it is, rather than driven blind, whenever the outdoor
or supply reading is missing or more than two minutes old; `state` is then
`holding` and `why` says which. `pos` is an estimate of how far the valve is
open, integrated from run time rather than measured, and is only reported —
the control loop follows the supply temperature, so a drifted estimate cannot
stop the valve from reaching either end.

The controller also mirrors itself onto MQTT as retained messages:
`temp/1/shunt/state`, `temp/1/shunt/setpoint` and `temp/1/shunt/position`.

`set-mqtt` restarts the ESP32's MQTT client against the new broker straight
away and only persists the URL once the client accepts it, so a URL the client
refuses to start on leaves the previous broker in place.

## Provisioning a fresh device

A board whose NVS holds no `SSID`, `PW` or `MQTT` value still boots: it reads
sensors, drives the heater and advertises over BLE, it just does not bring up
the network. Telemetry shows this as an empty SSID and an empty broker URL.
Pair with it and send the two commands to finish provisioning:

```bash
templogctl set-wifi "MyHomeSSID" "supersecret"
templogctl set-mqtt mqtt://192.168.2.10:1883
```

Both take effect immediately — no reboot — and are written to NVS, so the
device comes up configured from then on.

## Security notes

* The link uses LE Secure Connections with bonding; traffic is encrypted
  after pairing.
* Pairing uses "Just Works" (no PIN/passkey), since neither the ESP32 nor a
  typical headless Raspberry Pi has a display/keyboard. This protects against
  passive eavesdropping and replay, but not against an active attacker present
  during the *initial* pairing. Perform first-time pairing in a physically
  trusted environment.
* Only one BLE central (the paired Raspberry Pi) is expected to be connected
  at a time; the ESP32 accepts a single connection.
