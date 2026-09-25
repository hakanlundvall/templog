package io.github.hakanlundvall.templog

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import io.github.hakanlundvall.templog.ble.CommandException
import io.github.hakanlundvall.templog.ble.ConnectionState
import io.github.hakanlundvall.templog.ble.Protocol
import io.github.hakanlundvall.templog.ble.SensorRole
import io.github.hakanlundvall.templog.ble.ShuntDirection
import io.github.hakanlundvall.templog.ble.TemplogBleClient
import io.github.hakanlundvall.templog.update.FirmwareRelease
import io.github.hakanlundvall.templog.update.FirmwareReleases
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.receiveAsFlow
import kotlinx.coroutines.launch
import org.json.JSONException
import org.json.JSONObject
import java.io.IOException
import java.util.zip.CRC32

/**
 * Owns the BLE client and turns UI intents into commands, serialising them so
 * the single status characteristic is never raced over by two writers.
 */
class TemplogViewModel(application: Application) : AndroidViewModel(application) {

    private companion object {
        /** The device validates the whole image before it answers. */
        const val END_TIMEOUT_MS = 30_000L
    }

    private val client = TemplogBleClient(application)

    val connectionState: StateFlow<ConnectionState> = client.connectionState
    val telemetry = client.telemetry
    val deviceAddress = client.deviceAddress
    val lastError = client.lastError

    private val _busy = MutableStateFlow(false)
    val busy: StateFlow<Boolean> = _busy.asStateFlow()

    private val _messages = Channel<String>(Channel.BUFFERED)

    /** One-shot user facing results of commands, rendered as a snackbar. */
    val messages = _messages.receiveAsFlow()

    private val _transfer = MutableStateFlow<FirmwareTransfer?>(null)

    /** Progress of an image the phone is fetching and pushing over BLE. */
    val transfer: StateFlow<FirmwareTransfer?> = _transfer.asStateFlow()

    private val _releaseCheck = MutableStateFlow<ReleaseCheck>(ReleaseCheck.NotChecked)

    /** What is known about the newest firmware release on GitHub. */
    val releaseCheck: StateFlow<ReleaseCheck> = _releaseCheck.asStateFlow()

    init {
        checkForFirmwareUpdate()
    }

    /** Called once the Bluetooth runtime permissions have been granted. */
    fun onPermissionsGranted() = client.start()

    fun reconnect() = client.reconnect()

    fun forgetDevice() = client.forgetDevice()

    fun refresh() = client.refreshTelemetry()

    fun clearError() = client.clearError()

    fun setWifi(ssid: String, password: String) =
        issue("Wi-Fi credentials updated", Protocol.setWifi(ssid, password))

    fun setMqtt(url: String) = issue("MQTT broker set to $url", Protocol.setMqtt(url))

    fun setSensorRole(romCodeHex: String, role: SensorRole) = issue(
        if (role == SensorRole.NONE) {
            "$romCodeHex no longer has a role"
        } else {
            "$romCodeHex is now the ${role.label} sensor"
        },
        Protocol.setSensorRole(romCodeHex, role),
    )

    fun setCurve(slope: Double, offsetC: Double, targetC: Double, minC: Double, maxC: Double) =
        issue("Heating curve updated", Protocol.setCurve(slope, offsetC, targetC, minC, maxC))

    fun setActuator(burstMs: Int, pauseS: Int, toleranceC: Double) =
        issue("Correction settings updated", Protocol.setActuator(burstMs, pauseS, toleranceC))

    fun setShuntEnabled(enabled: Boolean) = issue(
        if (enabled) "Shunt control started" else "Shunt control stopped",
        Protocol.setShuntEnabled(enabled),
    )

    fun setIndoor(topic: String, gain: Double, maxTrimC: Double, staleS: Int) = issue(
        if (topic.isBlank()) "Indoor trim switched off" else "Indoor temperature read from $topic",
        Protocol.setIndoor(topic, gain, maxTrimC, staleS),
    )

    fun jogShunt(dir: ShuntDirection, seconds: Int) = issue(
        "Running the actuator ${dir.wire} for ${seconds}s",
        Protocol.shuntJog(dir, seconds * 1000),
    )

    fun setThresholds(onC: Double, offC: Double) =
        issue("Thresholds set to on $onC °C / off $offC °C", Protocol.setThresholds(onC, offC))

    fun heaterOff(untilStarted: Boolean) = issue(
        if (untilStarted) "Heater off until started" else "Heater off until conditions are met",
        Protocol.heaterOff(untilStarted),
    )

    fun heaterOn() = issue("Heater started", Protocol.heaterOn())

    fun checkForFirmwareUpdate() {
        if (_releaseCheck.value == ReleaseCheck.Checking) return
        _releaseCheck.value = ReleaseCheck.Checking
        viewModelScope.launch {
            _releaseCheck.value = try {
                FirmwareReleases.latest()?.let { ReleaseCheck.Found(it) } ?: ReleaseCheck.NoReleases
            } catch (e: IOException) {
                ReleaseCheck.Failed(e.message ?: "network error")
            } catch (e: JSONException) {
                ReleaseCheck.Failed("unexpected reply from GitHub")
            }
        }
    }

    /**
     * Tells the device to download and install [tag] itself. Progress arrives
     * through telemetry; the device then restarts and the client reconnects.
     */
    fun installFirmware(tag: String) =
        issue("Firmware update to $tag started", Protocol.otaUpdate(tag))

    /**
     * Installs [release] without using the device's Wi-Fi: the phone fetches
     * the image from GitHub and writes it to the device over BLE, which takes
     * a couple of minutes. The device checks the CRC before it switches over.
     */
    fun installFirmwareOverBle(release: FirmwareRelease) {
        if (_busy.value) {
            _messages.trySend("Another command is still running")
            return
        }
        viewModelScope.launch {
            _busy.value = true
            _transfer.value = FirmwareTransfer.Downloading(0, release.assetSize)
            var started = false
            try {
                val image = FirmwareReleases.download(release) { bytes, total ->
                    _transfer.value = FirmwareTransfer.Downloading(bytes, total)
                }
                val crc32 = CRC32().apply { update(image) }.value

                client.sendCommand(Protocol.otaBleBegin(image.size, crc32, release.tag))
                started = true
                _transfer.value = FirmwareTransfer.Sending(0, image.size)
                client.sendFirmware(image) { sent ->
                    _transfer.value = FirmwareTransfer.Sending(sent, image.size)
                }

                // The device validates the image, then restarts into it, so
                // the link drops moments after this reply.
                client.sendCommand(Protocol.otaBleEnd(), timeoutMs = END_TIMEOUT_MS)
                _messages.trySend("${release.tag} sent; the device is restarting")
            } catch (e: CommandException) {
                if (started) runCatching { client.sendCommand(Protocol.otaBleAbort()) }
                _messages.trySend(e.message ?: "firmware transfer failed")
            } catch (e: IOException) {
                if (started) runCatching { client.sendCommand(Protocol.otaBleAbort()) }
                _messages.trySend(e.message ?: "could not download the firmware")
            } finally {
                _transfer.value = null
                _busy.value = false
            }
        }
    }

    private fun issue(successMessage: String, command: JSONObject) {
        if (_busy.value) {
            _messages.trySend("Another command is still running")
            return
        }
        viewModelScope.launch {
            _busy.value = true
            try {
                client.sendCommand(command)
                _messages.trySend(successMessage)
                // The firmware pushes fresh telemetry on its own schedule; ask
                // for it now so the UI reflects the change immediately.
                client.refreshTelemetry()
            } catch (e: CommandException) {
                _messages.trySend(e.message ?: "command failed")
            } finally {
                _busy.value = false
            }
        }
    }

    override fun onCleared() {
        client.stop()
        super.onCleared()
    }
}

/** Where an image the phone is pushing over BLE has got to. */
sealed interface FirmwareTransfer {
    val bytes: Int
    val total: Int

    val fraction: Float
        get() = if (total > 0) bytes.toFloat() / total else 0f

    /** Fetching the image from GitHub onto the phone. */
    data class Downloading(override val bytes: Int, override val total: Int) : FirmwareTransfer

    /** Writing the image to the device. */
    data class Sending(override val bytes: Int, override val total: Int) : FirmwareTransfer
}

sealed interface ReleaseCheck {
    data object NotChecked : ReleaseCheck
    data object Checking : ReleaseCheck
    data object NoReleases : ReleaseCheck
    data class Found(val release: FirmwareRelease) : ReleaseCheck
    data class Failed(val reason: String) : ReleaseCheck
}
