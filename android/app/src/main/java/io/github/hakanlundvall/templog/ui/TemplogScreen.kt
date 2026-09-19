package io.github.hakanlundvall.templog.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.AssistChip
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.github.hakanlundvall.templog.ReleaseCheck
import io.github.hakanlundvall.templog.TemplogViewModel
import io.github.hakanlundvall.templog.ble.ConnectionState
import io.github.hakanlundvall.templog.ble.HeaterForceState
import io.github.hakanlundvall.templog.ble.OtaState
import io.github.hakanlundvall.templog.ble.SensorReading
import io.github.hakanlundvall.templog.ble.Telemetry
import java.util.Locale

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun TemplogScreen(
    viewModel: TemplogViewModel,
    permissionsGranted: Boolean,
    onRequestPermissions: () -> Unit,
) {
    val state by viewModel.connectionState.collectAsStateWithLifecycle()
    val telemetry by viewModel.telemetry.collectAsStateWithLifecycle()
    val address by viewModel.deviceAddress.collectAsStateWithLifecycle()
    val error by viewModel.lastError.collectAsStateWithLifecycle()
    val busy by viewModel.busy.collectAsStateWithLifecycle()
    val releaseCheck by viewModel.releaseCheck.collectAsStateWithLifecycle()

    val snackbarHostState = remember { SnackbarHostState() }
    LaunchedEffect(Unit) {
        viewModel.messages.collect { snackbarHostState.showSnackbar(it) }
    }

    var dialog by remember { mutableStateOf<Dialog?>(null) }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("templog") },
                actions = {
                    TextButton(onClick = viewModel::reconnect) { Text("Reconnect") }
                },
            )
        },
        snackbarHost = { SnackbarHost(snackbarHostState) },
    ) { padding ->
        Column(Modifier.padding(padding)) {
            if (busy) {
                LinearProgressIndicator(Modifier.fillMaxWidth())
            } else {
                Spacer(Modifier.height(4.dp))
            }

            LazyColumn(
                contentPadding = PaddingValues(16.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                item {
                    ConnectionCard(
                        state = state,
                        address = address,
                        error = error,
                        permissionsGranted = permissionsGranted,
                        onRequestPermissions = onRequestPermissions,
                        onDismissError = viewModel::clearError,
                        onForget = viewModel::forgetDevice,
                    )
                }

                val snapshot = telemetry
                if (snapshot == null) {
                    item { WaitingCard(state) }
                } else {
                    item {
                        HeaterCard(
                            telemetry = snapshot,
                            enabled = state == ConnectionState.READY && !busy,
                            onHeaterOn = viewModel::heaterOn,
                            onHeaterOff = viewModel::heaterOff,
                            onEditThresholds = { dialog = Dialog.Thresholds },
                        )
                    }
                    item {
                        WifiCard(
                            telemetry = snapshot,
                            enabled = state == ConnectionState.READY && !busy,
                            onEdit = { dialog = Dialog.Wifi },
                        )
                    }
                    item {
                        MqttCard(
                            telemetry = snapshot,
                            enabled = state == ConnectionState.READY && !busy,
                            onEdit = { dialog = Dialog.Mqtt },
                        )
                    }
                    item {
                        FirmwareCard(
                            telemetry = snapshot,
                            releaseCheck = releaseCheck,
                            enabled = state == ConnectionState.READY && !busy,
                            onCheck = viewModel::checkForFirmwareUpdate,
                            onInstall = { tag -> dialog = Dialog.Firmware(tag) },
                        )
                    }
                    item {
                        Text(
                            "Temperature sensors",
                            style = MaterialTheme.typography.titleMedium,
                            modifier = Modifier.padding(top = 8.dp),
                        )
                    }
                    items(snapshot.sensors, key = { it.id }) { sensor ->
                        SensorCard(
                            sensor = sensor,
                            enabled = state == ConnectionState.READY && !busy,
                            onMakeWaterSensor = { viewModel.setWaterSensor(sensor.id) },
                        )
                    }
                    item {
                        OutlinedButton(
                            onClick = viewModel::refresh,
                            modifier = Modifier.fillMaxWidth(),
                        ) { Text("Refresh telemetry") }
                    }
                }
            }
        }
    }

    when (val current = dialog) {
        Dialog.Wifi -> WifiDialog(
            onDismiss = { dialog = null },
            onConfirm = { ssid, password ->
                dialog = null
                viewModel.setWifi(ssid, password)
            },
        )

        Dialog.Mqtt -> MqttDialog(
            initialUrl = telemetry?.mqttUrl.orEmpty(),
            onDismiss = { dialog = null },
            onConfirm = { url ->
                dialog = null
                viewModel.setMqtt(url)
            },
        )

        Dialog.Thresholds -> ThresholdDialog(
            initialOn = telemetry?.heaterOnThresholdC,
            initialOff = telemetry?.heaterOffThresholdC,
            onDismiss = { dialog = null },
            onConfirm = { onC, offC ->
                dialog = null
                viewModel.setThresholds(onC, offC)
            },
        )

        is Dialog.Firmware -> FirmwareDialog(
            tag = current.tag,
            installed = telemetry?.firmwareVersion,
            onDismiss = { dialog = null },
            onConfirm = {
                dialog = null
                viewModel.installFirmware(current.tag)
            },
        )

        null -> Unit
    }
}

private sealed interface Dialog {
    data object Wifi : Dialog
    data object Mqtt : Dialog
    data object Thresholds : Dialog
    data class Firmware(val tag: String) : Dialog
}

@Composable
private fun ConnectionCard(
    state: ConnectionState,
    address: String?,
    error: String?,
    permissionsGranted: Boolean,
    onRequestPermissions: () -> Unit,
    onDismissError: () -> Unit,
    onForget: () -> Unit,
) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                if (state != ConnectionState.READY && state != ConnectionState.UNAVAILABLE) {
                    CircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp)
                    Spacer(Modifier.width(12.dp))
                }
                Text(state.label(), style = MaterialTheme.typography.titleMedium)
            }
            if (address != null) {
                Text(
                    address,
                    style = MaterialTheme.typography.bodySmall,
                    fontFamily = FontFamily.Monospace,
                )
            }
            if (!permissionsGranted) {
                Text(
                    "Bluetooth permissions are required to find and control the device.",
                    style = MaterialTheme.typography.bodyMedium,
                )
                Button(onClick = onRequestPermissions) { Text("Grant permissions") }
            }
            if (error != null) {
                Text(
                    error,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.error,
                )
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    TextButton(onClick = onDismissError) { Text("Dismiss") }
                    TextButton(onClick = onForget) { Text("Scan again") }
                }
            }
        }
    }
}

@Composable
private fun WaitingCard(state: ConnectionState) {
    Card(Modifier.fillMaxWidth()) {
        Text(
            when (state) {
                ConnectionState.READY -> "Waiting for the first telemetry snapshot…"
                ConnectionState.UNAVAILABLE -> "Bluetooth is unavailable."
                else -> "No telemetry yet. " + state.label() + "."
            },
            modifier = Modifier.padding(16.dp),
            style = MaterialTheme.typography.bodyMedium,
        )
    }
}

@Composable
private fun HeaterCard(
    telemetry: Telemetry,
    enabled: Boolean,
    onHeaterOn: () -> Unit,
    onHeaterOff: (Boolean) -> Unit,
    onEditThresholds: () -> Unit,
) {
    Card(
        Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(
            containerColor = if (telemetry.heaterOn) {
                MaterialTheme.colorScheme.primaryContainer
            } else {
                MaterialTheme.colorScheme.surfaceVariant
            },
        ),
    ) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Row(
                Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text("Heater", style = MaterialTheme.typography.titleMedium)
                Text(
                    if (telemetry.heaterOn) "ON" else "OFF",
                    style = MaterialTheme.typography.titleLarge,
                )
            }

            val water = telemetry.waterSensor
            Text(
                "Water temperature: " + (water?.celsius?.formatC() ?: "no water sensor selected"),
                style = MaterialTheme.typography.bodyMedium,
            )
            Text(
                "Thresholds: on at/below ${telemetry.heaterOnThresholdC.formatC()}, " +
                    "off at/above ${telemetry.heaterOffThresholdC.formatC()}",
                style = MaterialTheme.typography.bodyMedium,
            )
            Text(
                when (telemetry.forceState) {
                    HeaterForceState.AUTOMATIC -> "Mode: automatic"
                    HeaterForceState.OFF_UNTIL_CONDITIONS ->
                        "Mode: forced off, resumes once the start conditions are met again"
                    HeaterForceState.OFF_UNTIL_STARTED ->
                        "Mode: forced off until explicitly started"
                    HeaterForceState.UNKNOWN -> "Mode: unknown"
                },
                style = MaterialTheme.typography.bodyMedium,
            )

            HorizontalDivider()

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                FilledTonalButton(
                    onClick = onHeaterOn,
                    enabled = enabled,
                    modifier = Modifier.weight(1f),
                ) { Text("Start") }
                FilledTonalButton(
                    onClick = { onHeaterOff(false) },
                    enabled = enabled,
                    modifier = Modifier.weight(1f),
                ) { Text("Off") }
            }
            OutlinedButton(
                onClick = { onHeaterOff(true) },
                enabled = enabled,
                modifier = Modifier.fillMaxWidth(),
            ) { Text("Off until started") }
            OutlinedButton(
                onClick = onEditThresholds,
                enabled = enabled,
                modifier = Modifier.fillMaxWidth(),
            ) { Text("Edit thresholds") }
        }
    }
}

@Composable
private fun WifiCard(telemetry: Telemetry, enabled: Boolean, onEdit: () -> Unit) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Wi-Fi", style = MaterialTheme.typography.titleMedium)
            Text(
                if (telemetry.wifiConnected) {
                    "Connected to ${telemetry.wifiSsid.ifEmpty { "(unnamed network)" }}"
                } else {
                    "Not connected" +
                        telemetry.wifiSsid.takeIf { it.isNotEmpty() }?.let { " (configured: $it)" }
                            .orEmpty()
                },
                style = MaterialTheme.typography.bodyMedium,
            )
            telemetry.wifiRssi?.let {
                Text(
                    "Signal: $it dBm",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            telemetry.wifiDisconnects?.let { count ->
                Text(
                    "Disconnects since boot: $count",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            telemetry.lastWifiDisconnect?.let { disc ->
                val reason = disc.reasonName?.let { "${disc.reason} ($it)" } ?: "${disc.reason}"
                Text(
                    "Last: ${formatAge(disc.ageMs)} ago, reason $reason, ${disc.rssi} dBm",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            OutlinedButton(onClick = onEdit, enabled = enabled, modifier = Modifier.fillMaxWidth()) {
                Text("Change credentials")
            }
        }
    }
}

@Composable
private fun MqttCard(telemetry: Telemetry, enabled: Boolean, onEdit: () -> Unit) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("MQTT broker", style = MaterialTheme.typography.titleMedium)
            Text(
                telemetry.mqttUrl.ifEmpty { "no broker configured" },
                style = MaterialTheme.typography.bodyMedium,
                fontFamily = FontFamily.Monospace,
            )
            Text(
                if (telemetry.mqttConnected) "Connected" else "Not connected",
                style = MaterialTheme.typography.bodySmall,
                color = if (telemetry.mqttConnected) {
                    MaterialTheme.colorScheme.onSurfaceVariant
                } else {
                    MaterialTheme.colorScheme.error
                },
            )
            OutlinedButton(onClick = onEdit, enabled = enabled, modifier = Modifier.fillMaxWidth()) {
                Text("Change broker URL")
            }
        }
    }
}

@Composable
private fun MqttDialog(
    initialUrl: String,
    onDismiss: () -> Unit,
    onConfirm: (String) -> Unit,
) {
    var url by remember { mutableStateOf(initialUrl) }
    val trimmed = url.trim()
    // The firmware buffer is 100 bytes including the terminator, and it
    // rejects an empty URL outright; say so before the round trip.
    val valid = trimmed.isNotEmpty() && trimmed.length < 100

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("MQTT broker") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(
                    value = url,
                    onValueChange = { url = it },
                    label = { Text("Broker URI") },
                    placeholder = { Text("mqtt://192.168.2.10:1883") },
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
                )
                if (trimmed.length >= 100) {
                    Text(
                        "The broker URL must be shorter than 100 characters.",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.error,
                    )
                }
            }
        },
        confirmButton = {
            TextButton(onClick = { onConfirm(trimmed) }, enabled = valid) { Text("Apply") }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

@Composable
private fun FirmwareCard(
    telemetry: Telemetry,
    releaseCheck: ReleaseCheck,
    enabled: Boolean,
    onCheck: () -> Unit,
    onInstall: (String) -> Unit,
) {
    val installed = telemetry.firmwareVersion
    val ota = telemetry.ota
    val latest = (releaseCheck as? ReleaseCheck.Found)?.release

    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Firmware", style = MaterialTheme.typography.titleMedium)
            Text(
                "Installed: " + (installed ?: "unknown"),
                style = MaterialTheme.typography.bodyMedium,
                fontFamily = FontFamily.Monospace,
            )
            Text(
                when (releaseCheck) {
                    ReleaseCheck.NotChecked -> "Latest release: not checked"
                    ReleaseCheck.Checking -> "Latest release: checking…"
                    ReleaseCheck.NoReleases -> "Latest release: none published"
                    is ReleaseCheck.Found -> "Latest release: ${releaseCheck.release.tag}" +
                        if (releaseCheck.release.tag == installed) " (installed)" else ""
                    is ReleaseCheck.Failed -> "Latest release: check failed, ${releaseCheck.reason}"
                },
                style = MaterialTheme.typography.bodySmall,
                color = if (releaseCheck is ReleaseCheck.Failed) {
                    MaterialTheme.colorScheme.error
                } else {
                    MaterialTheme.colorScheme.onSurfaceVariant
                },
            )

            if (ota != null) {
                val target = ota.version?.let { " $it" }.orEmpty()
                when (ota.state) {
                    OtaState.DOWNLOADING -> {
                        Text(
                            "Downloading$target" + (ota.percent?.let { " – $it %" } ?: "…"),
                            style = MaterialTheme.typography.bodyMedium,
                        )
                        val percent = ota.percent
                        if (percent != null) {
                            LinearProgressIndicator(
                                progress = { percent / 100f },
                                modifier = Modifier.fillMaxWidth(),
                            )
                        } else {
                            LinearProgressIndicator(Modifier.fillMaxWidth())
                        }
                    }
                    OtaState.REBOOTING -> Text(
                        "Installed$target, restarting…",
                        style = MaterialTheme.typography.bodyMedium,
                    )
                    OtaState.VERIFYING -> Text(
                        "New firmware is running but not confirmed yet. It rolls back if it " +
                            "cannot reach Wi-Fi.",
                        style = MaterialTheme.typography.bodySmall,
                    )
                    OtaState.UP_TO_DATE -> Text(
                        "Already running$target, nothing installed.",
                        style = MaterialTheme.typography.bodySmall,
                    )
                    OtaState.FAILED -> Text(
                        "Update failed: " + (ota.error ?: "unknown error"),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.error,
                    )
                    OtaState.IDLE, OtaState.UNKNOWN -> Unit
                }
            } else if (installed == null) {
                Text(
                    "This firmware does not support updates over the air; flash it over USB once.",
                    style = MaterialTheme.typography.bodySmall,
                )
            }

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(
                    onClick = onCheck,
                    enabled = releaseCheck != ReleaseCheck.Checking,
                    modifier = Modifier.weight(1f),
                ) { Text("Check") }
                FilledTonalButton(
                    onClick = { latest?.let { onInstall(it.tag) } },
                    enabled = enabled && latest != null && ota != null && !ota.inProgress &&
                        ota.state != OtaState.VERIFYING && latest.tag != installed,
                    modifier = Modifier.weight(1f),
                ) {
                    Text(
                        if (latest != null && latest.tag == installed) "Up to date" else "Install",
                        maxLines = 1,
                    )
                }
            }
        }
    }
}

@Composable
private fun FirmwareDialog(
    tag: String,
    installed: String?,
    onDismiss: () -> Unit,
    onConfirm: () -> Unit,
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Install firmware $tag?") },
        text = {
            Text(
                "The device downloads $tag from GitHub over its own Wi-Fi" +
                    (installed?.let { ", replacing $it," } ?: "") +
                    " and then restarts. The heater is off for the few seconds the " +
                    "restart takes. If the new firmware cannot reach Wi-Fi within two " +
                    "minutes, the device goes back to the current version.",
            )
        },
        confirmButton = { TextButton(onClick = onConfirm) { Text("Install") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun SensorCard(sensor: SensorReading, enabled: Boolean, onMakeWaterSensor: () -> Unit) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Row(
                Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    sensor.id,
                    style = MaterialTheme.typography.bodyMedium,
                    fontFamily = FontFamily.Monospace,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.weight(1f),
                )
                Spacer(Modifier.width(8.dp))
                Text(
                    sensor.celsius?.formatC() ?: "no reading",
                    style = MaterialTheme.typography.titleMedium,
                )
            }
            Row(verticalAlignment = Alignment.CenterVertically) {
                if (sensor.isWaterSensor) {
                    AssistChip(onClick = {}, enabled = false, label = { Text("water sensor") })
                    Spacer(Modifier.width(8.dp))
                }
                sensor.ageMs?.let {
                    Text(
                        "read ${formatAge(it)} ago",
                        style = MaterialTheme.typography.bodySmall,
                    )
                }
            }
            if (!sensor.isWaterSensor) {
                TextButton(onClick = onMakeWaterSensor, enabled = enabled) {
                    Text("Use as water sensor")
                }
            }
        }
    }
}

@Composable
private fun WifiDialog(onDismiss: () -> Unit, onConfirm: (String, String) -> Unit) {
    var ssid by remember { mutableStateOf("") }
    var password by remember { mutableStateOf("") }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Wi-Fi credentials") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(
                    value = ssid,
                    onValueChange = { ssid = it },
                    label = { Text("SSID") },
                    singleLine = true,
                )
                OutlinedTextField(
                    value = password,
                    onValueChange = { password = it },
                    label = { Text("Password") },
                    singleLine = true,
                    visualTransformation = PasswordVisualTransformation(),
                )
            }
        },
        confirmButton = {
            TextButton(
                onClick = { onConfirm(ssid, password) },
                enabled = ssid.isNotBlank(),
            ) { Text("Apply") }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

@Composable
private fun ThresholdDialog(
    initialOn: Double?,
    initialOff: Double?,
    onDismiss: () -> Unit,
    onConfirm: (Double, Double) -> Unit,
) {
    var on by remember { mutableStateOf(initialOn?.takeIf { !it.isNaN() }?.formatPlain().orEmpty()) }
    var off by remember { mutableStateOf(initialOff?.takeIf { !it.isNaN() }?.formatPlain().orEmpty()) }

    val onValue = on.replace(',', '.').toDoubleOrNull()
    val offValue = off.replace(',', '.').toDoubleOrNull()
    // The firmware rejects on >= off, so say so before the round trip.
    val valid = onValue != null && offValue != null && onValue < offValue

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Heater thresholds") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(
                    value = on,
                    onValueChange = { on = it },
                    label = { Text("Turn on at/below (°C)") },
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                )
                OutlinedTextField(
                    value = off,
                    onValueChange = { off = it },
                    label = { Text("Turn off at/above (°C)") },
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                )
                if (!valid) {
                    Text(
                        "The on threshold must be lower than the off threshold.",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.error,
                    )
                }
            }
        },
        confirmButton = {
            TextButton(
                onClick = { onConfirm(onValue ?: 0.0, offValue ?: 0.0) },
                enabled = valid,
            ) { Text("Apply") }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

@Composable
fun PermissionRationale(onRequestPermissions: () -> Unit) {
    Box(Modifier.fillMaxWidth().padding(32.dp), contentAlignment = Alignment.Center) {
        Column(
            verticalArrangement = Arrangement.spacedBy(16.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Text(
                "templog talks to the heater controller over Bluetooth Low Energy, " +
                    "so it needs permission to scan for and connect to nearby devices.",
                style = MaterialTheme.typography.bodyLarge,
            )
            Button(onClick = onRequestPermissions) { Text("Grant Bluetooth permissions") }
        }
    }
}

private fun ConnectionState.label(): String = when (this) {
    ConnectionState.UNAVAILABLE -> "Bluetooth unavailable"
    ConnectionState.IDLE -> "Waiting to reconnect"
    ConnectionState.SCANNING -> "Scanning for templog"
    ConnectionState.CONNECTING -> "Connecting"
    ConnectionState.PAIRING -> "Pairing"
    ConnectionState.DISCOVERING -> "Setting up"
    ConnectionState.READY -> "Connected"
}

private fun Double.formatC(): String =
    if (isNaN()) "—" else String.format(Locale.US, "%.1f °C", this)

private fun Double.formatPlain(): String = String.format(Locale.US, "%.1f", this)

private fun formatAge(ms: Long): String {
    val seconds = ms / 1000
    return when {
        seconds < 60 -> "${seconds}s"
        seconds < 3600 -> "${seconds / 60}m ${seconds % 60}s"
        else -> "${seconds / 3600}h ${(seconds % 3600) / 60}m"
    }
}
