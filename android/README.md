# templog Android app

Android client for the templog ESP32 heater controller. It does the same job
as the Raspberry Pi client in [`../rpi`](../rpi): it pairs with the device over
Bluetooth Low Energy, shows live telemetry, and sends the same set of commands.

## What it does

* **Finds the device.** Reconnects straight to an already bonded `templog`
  when there is one, otherwise scans by advertised name. The firmware does not
  put its service UUID in the advertisement, so the name is the only filter
  available.
* **Pairs.** The GATT characteristics require an encrypted link, so the first
  read triggers the firmware's "Just Works" LE Secure Connections pairing.
  Android shows its normal pairing prompt; after that the bond is remembered by
  both sides.
* **Shows status.** Every temperature sensor with its ROM code, value and
  reading age, which one is the water sensor, Wi-Fi connection state and SSID,
  heater on/off, the on/off thresholds and the current force-off mode.
* **Controls the device.** Start the heater, force it off (either until the
  start conditions are met again or until explicitly started), change the
  thresholds, choose the water sensor, and set the Wi-Fi credentials.
* **Survives a bad link.** Disconnects re-enter the connect loop with
  exponential backoff (1s doubling to 30s), matching the Pi service.

## Protocol notes

The firmware sends a fixed one byte payload for *both* notify
characteristics — a notification only means "something changed", because a
notification can never exceed the negotiated ATT MTU. The app therefore always
follows a notification with a read of the characteristic, for status replies
just as much as for telemetry. `Protocol.kt` mirrors
[`../rpi/templog_ble/protocol.py`](../rpi/templog_ble/protocol.py).

## Layout

| Path | Purpose |
| --- | --- |
| `app/src/main/java/.../ble/Protocol.kt` | UUIDs, command builders, telemetry/status parsing |
| `app/src/main/java/.../ble/TemplogBleClient.kt` | Connection loop, GATT operation queue, bonding |
| `app/src/main/java/.../TemplogViewModel.kt` | UI state and command serialisation |
| `app/src/main/java/.../ui/` | Compose screen and theme |

## Building

The APK is built by GitHub Actions
([`.github/workflows/android.yml`](../.github/workflows/android.yml)) on every
push that touches `android/`. Download it from the run's **templog-app-debug**
artifact and install with `adb install app-debug.apk`.

To build locally, use the dev container in
[`../.devcontainer/android`](../.devcontainer/android) (VS Code: *Dev
Containers: Reopen in Container*, then pick **Android (templog app)**) so that
no Android tooling lands on your host:

```bash
cd android
./gradlew assembleDebug
```

> **arm64 hosts (Raspberry Pi, Apple Silicon under Linux):** Google only
> publishes `aapt2` for linux x86_64, so `assembleDebug` cannot run there. The
> dev container is still useful for editing and for Gradle's configuration
> phase; leave the actual packaging to the CI workflow.

## Requirements

* Android 8.0 (API 26) or newer.
* On Android 12+ the app asks for *Nearby devices* (`BLUETOOTH_SCAN` and
  `BLUETOOTH_CONNECT`); on older releases it asks for location instead, which
  is what gated BLE scanning back then.

## Security notes

The same caveats as the Raspberry Pi client apply: the link is encrypted and
bonded, but pairing is "Just Works" because the ESP32 has no display or
keypad. That protects against passive eavesdropping, not against an active
attacker present during the *initial* pairing — so pair for the first time in a
physically trusted place. The ESP32 accepts a single connection at a time, so
the phone and the Raspberry Pi bridge cannot both be connected at once.
