"""Shared BLE protocol constants matching the ESP32 firmware (src/ble_service.c).

The GATT service exposes three characteristics:
  * TELEMETRY  - read + notify. Value is a compact JSON document describing
                 temperature sensors, Wi-Fi status and heater state. Because
                 BLE notifications are truncated at the negotiated ATT MTU,
                 a notification only signals "something changed" - clients
                 must always follow up with a `read` to get the full,
                 untruncated value (BlueZ performs the GATT "long read"
                 procedure transparently).
  * COMMAND    - write (with response). Body is a JSON command object.
  * STATUS     - read + notify. JSON {"cmd", "ok", "error"} describing the
                 outcome of the most recently processed command.
"""

from __future__ import annotations

DEVICE_NAME = "templog"

SERVICE_UUID = "6e400000-b5a3-f393-e0a9-e50e24dcca9e"
TELEMETRY_CHAR_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
COMMAND_CHAR_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
STATUS_CHAR_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

# Commands (sent as JSON on COMMAND_CHAR_UUID)
CMD_SET_WIFI = "set_wifi"
CMD_SET_MQTT = "set_mqtt"
CMD_SET_WATER_SENSOR = "set_water_sensor"
CMD_SET_THRESHOLDS = "set_thresholds"
CMD_HEATER_OFF = "heater_off"
CMD_HEATER_ON = "heater_on"

HEATER_OFF_UNTIL_CONDITIONS = "until_conditions"
HEATER_OFF_UNTIL_STARTED = "until_started"
