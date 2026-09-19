"""Command line client for the templog BLE service.

Talks to the local `templog-ble` service over a Unix domain socket; the
service itself owns the single BLE connection to the ESP32.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import sys

from . import protocol
from .service import DEFAULT_SOCKET_PATH


async def _request(socket_path: str, payload: dict, timeout: float = 15.0) -> dict:
    reader, writer = await asyncio.open_unix_connection(socket_path)
    try:
        writer.write((json.dumps(payload) + "\n").encode("utf-8"))
        await writer.drain()
        line = await asyncio.wait_for(reader.readline(), timeout=timeout)
        if not line:
            raise RuntimeError("service closed the connection without a response")
        return json.loads(line.decode("utf-8"))
    finally:
        writer.close()


def _run(socket_path: str, payload: dict) -> dict:
    try:
        return asyncio.run(_request(socket_path, payload))
    except (ConnectionRefusedError, FileNotFoundError):
        print(f"error: could not reach templog-ble service on {socket_path}. "
              "Is it running? (systemctl status templog-ble)", file=sys.stderr)
        sys.exit(1)


def cmd_status(args: argparse.Namespace) -> None:
    resp = _run(args.socket, {"action": "get_state"})
    print(json.dumps(resp, indent=2))


def cmd_set_wifi(args: argparse.Namespace) -> None:
    _issue_command(args, {"cmd": protocol.CMD_SET_WIFI, "ssid": args.ssid, "password": args.password})


def cmd_set_mqtt(args: argparse.Namespace) -> None:
    _issue_command(args, {"cmd": protocol.CMD_SET_MQTT, "url": args.url})


def cmd_set_water_sensor(args: argparse.Namespace) -> None:
    _issue_command(args, {"cmd": protocol.CMD_SET_WATER_SENSOR, "id": args.sensor_id})


def cmd_set_thresholds(args: argparse.Namespace) -> None:
    _issue_command(
        args,
        {"cmd": protocol.CMD_SET_THRESHOLDS, "on": args.on, "off": args.off},
    )


def cmd_heater_off(args: argparse.Namespace) -> None:
    mode = protocol.HEATER_OFF_UNTIL_STARTED if args.until_started else protocol.HEATER_OFF_UNTIL_CONDITIONS
    _issue_command(args, {"cmd": protocol.CMD_HEATER_OFF, "mode": mode})


def cmd_heater_on(args: argparse.Namespace) -> None:
    _issue_command(args, {"cmd": protocol.CMD_HEATER_ON})


def cmd_ota_update(args: argparse.Namespace) -> None:
    _issue_command(args, {"cmd": protocol.CMD_OTA_UPDATE, "tag": args.tag, "force": args.force})


def _issue_command(args: argparse.Namespace, command: dict) -> None:
    resp = _run(args.socket, {"action": "command", "command": command})
    print(json.dumps(resp, indent=2))
    if not resp.get("ok"):
        sys.exit(1)


def main() -> None:
    parser = argparse.ArgumentParser(prog="templogctl", description="Control the templog ESP32 device via the BLE bridge service")
    parser.add_argument("--socket", default=DEFAULT_SOCKET_PATH, help="Unix socket path of the templog-ble service")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("status", help="Show connection state and latest telemetry")
    p.set_defaults(func=cmd_status)

    p = sub.add_parser("set-wifi", help="Change the Wi-Fi credentials used by the ESP32")
    p.add_argument("ssid")
    p.add_argument("password")
    p.set_defaults(func=cmd_set_wifi)

    p = sub.add_parser("set-mqtt", help="Change the MQTT broker URL used by the ESP32")
    p.add_argument("url", help="Broker URI, e.g. mqtt://192.168.2.10:1883")
    p.set_defaults(func=cmd_set_mqtt)

    p = sub.add_parser("set-water-sensor", help="Select which DS18B20 sensor (by ROM code hex) is the water sensor")
    p.add_argument("sensor_id", help="16 hex character ROM code, e.g. from `status`")
    p.set_defaults(func=cmd_set_water_sensor)

    p = sub.add_parser("set-thresholds", help="Set heater on/off temperature thresholds (Celsius)")
    p.add_argument("--on", type=float, required=True, help="Turn heater on at/below this temperature")
    p.add_argument("--off", type=float, required=True, help="Turn heater off at/above this temperature")
    p.set_defaults(func=cmd_set_thresholds)

    p = sub.add_parser("heater-off", help="Force the heater off")
    p.add_argument(
        "--until-started",
        action="store_true",
        help="Stay off until an explicit 'heater-on' command (default: resume automatically once start conditions are met again)",
    )
    p.set_defaults(func=cmd_heater_off)

    p = sub.add_parser("heater-on", help="Start the heater (unless water temperature is already above the off threshold)")
    p.set_defaults(func=cmd_heater_on)

    p = sub.add_parser("ota-update", help="Have the ESP32 download and install a firmware release from GitHub")
    p.add_argument("--tag", default="latest", help="Release tag to install, e.g. v1.2.0 (default: latest)")
    p.add_argument("--force", action="store_true", help="Install even if the device already runs that version")
    p.set_defaults(func=cmd_ota_update)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
