"""Shared BLE protocol constants matching the ESP32 firmware (src/ble_service.c).

What the device reports is split over three read characteristics rather than
one, because a GATT client will not read more than 512 bytes of a single
attribute value - Android's stack truncates there without saying so. Sensor
readings are the part that grows (about 58 bytes each, up to eight of them),
so they have a characteristic of their own.

The GATT service exposes:
  * TELEMETRY  - read + notify. Live state: Wi-Fi and MQTT connection, heater
                 output and mode, what the shunt valve control is doing, the
                 indoor reading it was given, and OTA progress. Because BLE
                 notifications are truncated at the negotiated ATT MTU, a
                 notification only signals "something changed" - clients must
                 always follow up with a `read` to get the full, untruncated
                 value (BlueZ performs the GATT "long read" procedure
                 transparently).
  * SENSORS    - read + notify. The DS18B20 readings, each with its age and
                 the role it has been given.
  * CONFIG     - read + notify. Settings that only change on command: Wi-Fi
                 SSID, broker URL, heater thresholds, the heating curve, the
                 actuator, the indoor topic and the running firmware version.
                 Notified only when something actually changed.
  * COMMAND    - write (with response). Body is a JSON command object.
  * STATUS     - read + notify. JSON {"cmd", "ok", "error"} describing the
                 outcome of the most recently processed command.
"""

from __future__ import annotations

DEVICE_NAME = "templog"

SERVICE_UUID = "6e400000-b5a3-f393-e0a9-e50e24dcca9e"
TELEMETRY_CHAR_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
SENSORS_CHAR_UUID = "6e400005-b5a3-f393-e0a9-e50e24dcca9e"
CONFIG_CHAR_UUID = "6e400006-b5a3-f393-e0a9-e50e24dcca9e"
COMMAND_CHAR_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
STATUS_CHAR_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

# Commands (sent as JSON on COMMAND_CHAR_UUID)
CMD_SET_WIFI = "set_wifi"
CMD_SET_MQTT = "set_mqtt"
CMD_SET_SENSOR_ROLE = "set_sensor_role"
# What set_sensor_role was called before there was more than one role.
CMD_SET_WATER_SENSOR = "set_water_sensor"
CMD_SET_THRESHOLDS = "set_thresholds"
CMD_HEATER_OFF = "heater_off"
CMD_HEATER_ON = "heater_on"
CMD_OTA_UPDATE = "ota_update"
CMD_SET_SHUNT = "set_shunt"
CMD_SHUNT_JOG = "shunt_jog"

# Roles a DS18B20 can be given. The water sensor drives the heater thermostat;
# the outdoor and supply sensors drive the shunt valve. "none" clears a role.
ROLE_WATER = "water"
ROLE_OUTDOOR = "outdoor"
ROLE_SUPPLY = "supply"
ROLE_NONE = "none"
SENSOR_ROLES = (ROLE_WATER, ROLE_OUTDOOR, ROLE_SUPPLY, ROLE_NONE)

JOG_WARMER = "warmer"
JOG_COLDER = "colder"

HEATER_OFF_UNTIL_CONDITIONS = "until_conditions"
HEATER_OFF_UNTIL_STARTED = "until_started"
