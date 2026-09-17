# templog BLE bridge (Raspberry Pi)

Service and CLI that let a Raspberry Pi 4/5 talk to the templog ESP32 device
over Bluetooth Low Energy (BLE), robust against flaky BLE connectivity.

## How it works

* The ESP32 advertises as `templog` and exposes a custom GATT service with:
  * a **telemetry** characteristic (read + notify) — temperatures (with age),
    Wi-Fi connection state + SSID, MQTT broker URL + connection state,
    heater on/off,
  * a **command** characteristic (write) — set Wi-Fi, set the MQTT broker,
    choose water sensor, set heater thresholds, force heater off, start
    heater,
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

templogctl set-water-sensor 28ff640000000000

templogctl set-thresholds --on 60 --off 80

templogctl heater-off                 # resumes automatically once temp drops to/below the on-threshold
templogctl heater-off --until-started # stays off until an explicit heater-on
templogctl heater-on                  # starts heater unless water temp already >= off-threshold
```

`templogctl status` prints the latest telemetry snapshot, e.g.:

```json
{
  "ok": true,
  "connected": true,
  "telemetry": {
    "t": [
      {"id": "28ff640000000000", "c": 62.3, "age": 1200, "water": true},
      {"id": "28aa550000000000", "c": 21.4, "age": 1200, "water": false}
    ],
    "wifi": {"c": true, "ssid": "MyHomeSSID"},
    "mqtt": {"c": true, "url": "mqtt://192.168.2.10:1883"},
    "heater": true,
    "onC": 60.0,
    "offC": 80.0,
    "forceState": 0
  }
}
```

`age` is milliseconds since that sensor's last good reading; a missing `c`/`age`
means the sensor has never produced a valid reading. `onC`/`offC` are the
current heater thresholds and `forceState` is `0` (automatic), `1` (forced
off until start conditions are met again) or `2` (forced off until
explicitly started).

`set-mqtt` restarts the ESP32's MQTT client against the new broker straight
away and only persists the URL once the client accepts it, so a URL the client
refuses to start on leaves the previous broker in place.

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
