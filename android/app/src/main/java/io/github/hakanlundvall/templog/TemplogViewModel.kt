package io.github.hakanlundvall.templog

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import io.github.hakanlundvall.templog.ble.CommandException
import io.github.hakanlundvall.templog.ble.ConnectionState
import io.github.hakanlundvall.templog.ble.Protocol
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

/**
 * Owns the BLE client and turns UI intents into commands, serialising them so
 * the single status characteristic is never raced over by two writers.
 */
class TemplogViewModel(application: Application) : AndroidViewModel(application) {

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

    fun setWaterSensor(romCodeHex: String) =
        issue("Water sensor set to $romCodeHex", Protocol.setWaterSensor(romCodeHex))

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

sealed interface ReleaseCheck {
    data object NotChecked : ReleaseCheck
    data object Checking : ReleaseCheck
    data object NoReleases : ReleaseCheck
    data class Found(val release: FirmwareRelease) : ReleaseCheck
    data class Failed(val reason: String) : ReleaseCheck
}
