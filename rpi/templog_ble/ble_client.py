"""Robust BLE client for talking to the templog ESP32 device.

Designed to run unattended on a Raspberry Pi and recover automatically from
poor BLE connectivity: connection attempts are retried with exponential
backoff, GATT operations are wrapped with timeouts, and a disconnect simply
re-enters the connect loop instead of raising out of the service.
"""

from __future__ import annotations

import asyncio
import json
import logging
from typing import Any, Callable, Optional

from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

from . import protocol

logger = logging.getLogger("templog_ble.client")

TelemetryCallback = Callable[[dict], None]
StatusCallback = Callable[[dict], None]

MIN_BACKOFF_S = 1.0
MAX_BACKOFF_S = 30.0
COMMAND_TIMEOUT_S = 10.0
SCAN_TIMEOUT_S = 10.0


class CommandError(RuntimeError):
    """Raised when the ESP32 reports a command failure or none is received in time."""


class BleClient:
    def __init__(
        self,
        device_name: str = protocol.DEVICE_NAME,
        address: Optional[str] = None,
        on_telemetry: Optional[TelemetryCallback] = None,
        on_status: Optional[StatusCallback] = None,
    ) -> None:
        self._device_name = device_name
        self._address = address
        self._on_telemetry = on_telemetry
        self._on_status = on_status

        self._client: Optional[BleakClient] = None
        self._connected_event = asyncio.Event()
        self._stop = False
        self._pending_status: "asyncio.Queue[dict]" = asyncio.Queue()
        self.last_telemetry: Optional[dict] = None

    async def run_forever(self) -> None:
        """Maintains a connection to the device, reconnecting with backoff."""
        backoff = MIN_BACKOFF_S
        while not self._stop:
            try:
                await self._connect_and_serve()
                backoff = MIN_BACKOFF_S
            except asyncio.CancelledError:
                raise
            except Exception as exc:  # noqa: BLE001 - top level resilience loop
                logger.warning("BLE session failed: %s", exc)
            finally:
                self._connected_event.clear()

            if self._stop:
                break
            logger.info("Reconnecting in %.1fs", backoff)
            await asyncio.sleep(backoff)
            backoff = min(backoff * 2, MAX_BACKOFF_S)

    async def stop(self) -> None:
        self._stop = True
        if self._client is not None:
            try:
                await self._client.disconnect()
            except Exception:  # noqa: BLE001
                pass

    async def wait_connected(self, timeout: Optional[float] = None) -> bool:
        try:
            await asyncio.wait_for(self._connected_event.wait(), timeout)
            return True
        except asyncio.TimeoutError:
            return False

    @property
    def is_connected(self) -> bool:
        return self._client is not None and self._client.is_connected

    async def send_command(self, command: dict, timeout: float = COMMAND_TIMEOUT_S) -> dict:
        """Writes a JSON command and waits for the matching status notification."""
        if not self.is_connected:
            raise CommandError("not connected to device")

        payload = json.dumps(command).encode("utf-8")
        # Drain stale status entries so we don't match against an old reply.
        while not self._pending_status.empty():
            self._pending_status.get_nowait()

        await self._client.write_gatt_char(protocol.COMMAND_CHAR_UUID, payload, response=True)

        deadline = asyncio.get_event_loop().time() + timeout
        while True:
            remaining = deadline - asyncio.get_event_loop().time()
            if remaining <= 0:
                raise CommandError(f"timed out waiting for status reply to {command.get('cmd')}")
            status = await asyncio.wait_for(self._pending_status.get(), timeout=remaining)
            if status.get("cmd") == command.get("cmd"):
                if not status.get("ok", False):
                    raise CommandError(status.get("error") or "command rejected")
                return status

    async def _connect_and_serve(self) -> None:
        address = self._address
        if address is None:
            logger.info("Scanning for device %r ...", self._device_name)
            device = await BleakScanner.find_device_by_name(
                self._device_name, timeout=SCAN_TIMEOUT_S
            )
            if device is None:
                raise BleakError(f"device {self._device_name!r} not found during scan")
            address = device.address

        logger.info("Connecting to %s ...", address)
        async with BleakClient(address, timeout=SCAN_TIMEOUT_S) as client:
            self._client = client
            try:
                await client.pair()
            except (NotImplementedError, BleakError) as exc:
                # Some backends auto-pair on first encrypted access, or are
                # already bonded; this is not fatal.
                logger.debug("pair() skipped/failed (may already be bonded): %s", exc)

            await client.start_notify(protocol.TELEMETRY_CHAR_UUID, self._on_state_notify)
            await client.start_notify(protocol.SENSORS_CHAR_UUID, self._on_sensors_notify)
            await client.start_notify(protocol.CONFIG_CHAR_UUID, self._on_config_notify)
            await client.start_notify(protocol.STATUS_CHAR_UUID, self._on_status_notify)

            # Prime state with initial reads rather than waiting for notifications.
            # The settings characteristic in particular only notifies when it
            # changes, so without this it would stay empty until it did.
            await self._read_telemetry()

            logger.info("Connected to %s", address)
            self._connected_event.set()

            while client.is_connected and not self._stop:
                await asyncio.sleep(1)

    async def _read_document(self, uuid: str, what: str) -> Optional[dict]:
        try:
            data = await self._client.read_gatt_char(uuid)
            return json.loads(bytes(data).decode("utf-8"))
        except Exception as exc:  # noqa: BLE001
            logger.warning("failed to read %s: %s", what, exc)
            return None

    async def _read_telemetry(self, *uuids: str) -> None:
        """Re-reads the named documents, or all of them, and republishes.

        The device splits what it reports over three characteristics so that
        no single value exceeds what a GATT client will read; they are merged
        back into one telemetry dict here, which is what the rest of the
        service and the CLI see.
        """
        wanted = uuids or (
            protocol.TELEMETRY_CHAR_UUID,
            protocol.SENSORS_CHAR_UUID,
            protocol.CONFIG_CHAR_UUID,
        )
        names = {
            protocol.TELEMETRY_CHAR_UUID: "state",
            protocol.SENSORS_CHAR_UUID: "sensors",
            protocol.CONFIG_CHAR_UUID: "config",
        }
        merged = dict(self.last_telemetry or {})
        changed = False
        for uuid in wanted:
            document = await self._read_document(uuid, names[uuid])
            if document is not None:
                merged.update(document)
                changed = True
        if not changed:
            return

        self.last_telemetry = merged
        if self._on_telemetry is not None:
            self._on_telemetry(merged)

    # The notification payload itself is not authoritative (it is a one byte
    # placeholder); always re-read the characteristic that changed.
    def _on_state_notify(self, _handle: int, _data: bytearray) -> None:
        asyncio.create_task(self._read_telemetry(protocol.TELEMETRY_CHAR_UUID))

    def _on_sensors_notify(self, _handle: int, _data: bytearray) -> None:
        asyncio.create_task(self._read_telemetry(protocol.SENSORS_CHAR_UUID))

    def _on_config_notify(self, _handle: int, _data: bytearray) -> None:
        asyncio.create_task(self._read_telemetry(protocol.CONFIG_CHAR_UUID))

    def _on_status_notify(self, _handle: int, data: bytearray) -> None:
        try:
            status = json.loads(bytes(data).decode("utf-8"))
        except Exception as exc:  # noqa: BLE001
            logger.warning("failed to parse status notification: %s", exc)
            return
        self._pending_status.put_nowait(status)
        if self._on_status is not None:
            self._on_status(status)
