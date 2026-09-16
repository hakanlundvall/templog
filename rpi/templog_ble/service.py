"""Background service: owns the single BLE connection to the ESP32 and exposes
a local Unix-domain-socket API so the CLI (and potentially other local
clients) can query telemetry and issue commands without needing their own
BLE connection.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import os
import stat
from typing import Optional

from . import protocol
from .ble_client import BleClient, CommandError

logger = logging.getLogger("templog_ble.service")

DEFAULT_SOCKET_PATH = "/run/templog-ble.sock"


class Service:
    def __init__(self, socket_path: str, device_name: str, address: Optional[str]) -> None:
        self._socket_path = socket_path
        self._ble = BleClient(
            device_name=device_name,
            address=address,
            on_telemetry=self._on_telemetry,
            on_status=self._on_status,
        )
        self._last_telemetry: Optional[dict] = None
        self._last_status: Optional[dict] = None

    def _on_telemetry(self, telemetry: dict) -> None:
        self._last_telemetry = telemetry

    def _on_status(self, status: dict) -> None:
        self._last_status = status
        logger.info("command result: %s", status)

    async def _handle_client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        try:
            line = await reader.readline()
            if not line:
                return
            request = json.loads(line.decode("utf-8"))
            response = await self._dispatch(request)
        except Exception as exc:  # noqa: BLE001
            response = {"ok": False, "error": str(exc)}
        try:
            writer.write((json.dumps(response) + "\n").encode("utf-8"))
            await writer.drain()
        finally:
            writer.close()

    async def _dispatch(self, request: dict) -> dict:
        action = request.get("action")
        if action == "get_state":
            return {
                "ok": True,
                "connected": self._ble.is_connected,
                "telemetry": self._last_telemetry,
            }
        if action == "command":
            command = request.get("command")
            if not isinstance(command, dict):
                return {"ok": False, "error": "missing 'command' object"}
            try:
                status = await self._ble.send_command(command)
                return {"ok": True, "status": status}
            except CommandError as exc:
                return {"ok": False, "error": str(exc)}
        return {"ok": False, "error": f"unknown action {action!r}"}

    async def _serve_socket(self) -> None:
        if os.path.exists(self._socket_path):
            os.remove(self._socket_path)
        server = await asyncio.start_unix_server(self._handle_client, path=self._socket_path)
        os.chmod(self._socket_path, stat.S_IRUSR | stat.S_IWUSR | stat.S_IRGRP | stat.S_IWGRP)
        logger.info("listening on %s", self._socket_path)
        async with server:
            await server.serve_forever()

    async def run(self) -> None:
        await asyncio.gather(self._ble.run_forever(), self._serve_socket())


def main() -> None:
    parser = argparse.ArgumentParser(description="templog BLE bridge service")
    parser.add_argument("--socket", default=DEFAULT_SOCKET_PATH, help="Unix socket path for CLI IPC")
    parser.add_argument("--device-name", default=protocol.DEVICE_NAME, help="BLE advertised device name")
    parser.add_argument("--address", default=None, help="Connect directly to this BLE address instead of scanning by name")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )

    service = Service(args.socket, args.device_name, args.address)
    try:
        asyncio.run(service.run())
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
