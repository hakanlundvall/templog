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
  the MQTT broker URL and whether it is connected, heater on/off, the on/off
  thresholds and the current force-off mode.
* **Controls the device.** Start the heater, force it off (either until the
  start conditions are met again or until explicitly started), change the
  thresholds, choose the water sensor, set the Wi-Fi credentials, and point
  the device at a different MQTT broker.
* **Survives a bad link.** Disconnects re-enter the connect loop with
  exponential backoff (1s doubling to 30s), matching the Pi service.

## Provisioning a fresh device

A device with nothing in NVS still advertises and accepts commands; it simply
has no network. The app shows an empty SSID and "no broker configured", and the
*Change credentials* and *Change broker URL* dialogs finish the setup. Both
apply immediately and persist on the device.

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

The APKs are built by GitHub Actions
([`.github/workflows/android.yml`](../.github/workflows/android.yml)) on every
push that touches `android/`. Each run uploads two artifacts:

| Artifact | Build | Use it for |
| --- | --- | --- |
| `templog-app-release` | `app-release.apk` | Installing on a phone |
| `templog-app-debug` | `app-debug.apk` | Debugging with attached tooling |

The release artifact also carries R8's `mapping.txt`, which is what turns an
obfuscated stack trace back into readable class and method names. Keep the copy
that matches whatever build is installed.

Install with `adb install -r app-release.apk`.

**Prefer the release APK.** Debug builds are signed with AGP's throwaway debug
keystore, and a CI runner has no persistent one, so *every run produces a
different signing key*. Android refuses to update an installed app across a key
change, so each debug build has to be uninstalled before the next can go on.
Release builds are signed with a fixed key and update in place. The release
build is also not debuggable and runs through R8 with resource shrinking, which
takes it from around 9.5 MB to a little over 1 MB.

### Release signing

The keystore is never committed — this repository is public. The build reads it
from the environment, and produces an unsigned APK when it is absent:

| Variable | GitHub Actions secret |
| --- | --- |
| `ANDROID_KEYSTORE_PATH` | derived from `ANDROID_KEYSTORE_BASE64` by the workflow |
| `ANDROID_KEYSTORE_PASSWORD` | `ANDROID_KEYSTORE_PASSWORD` |
| `ANDROID_KEY_ALIAS` | `ANDROID_KEY_ALIAS` |
| `ANDROID_KEY_PASSWORD` | `ANDROID_KEY_PASSWORD` |

To build a signed release locally, export the same four variables and run
`./gradlew assembleRelease`. The keystore is a PKCS12 file, which uses a single
password for both the store and the key.

> Back the keystore up. Losing it does not cost any app data — the BLE bond
> lives in Android's Bluetooth settings, not in the app — but every phone with
> the app installed would have to uninstall before taking another update.

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
