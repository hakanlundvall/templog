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
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.github.hakanlundvall.templog.FirmwareTransfer
import io.github.hakanlundvall.templog.ReleaseCheck
import io.github.hakanlundvall.templog.TemplogViewModel
import io.github.hakanlundvall.templog.ble.ConnectionState
import io.github.hakanlundvall.templog.ble.HeaterForceState
import io.github.hakanlundvall.templog.ble.OtaState
import io.github.hakanlundvall.templog.ble.IndoorStatus
import io.github.hakanlundvall.templog.ble.SensorReading
import io.github.hakanlundvall.templog.ble.SensorRole
import io.github.hakanlundvall.templog.ble.ShuntDirection
import io.github.hakanlundvall.templog.ble.ShuntStatus
import io.github.hakanlundvall.templog.ble.Telemetry
import io.github.hakanlundvall.templog.update.FirmwareRelease
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
    val transfer by viewModel.transfer.collectAsStateWithLifecycle()

    // A transfer over BLE takes minutes and dies with the process if Android
    // freezes the app, so hold the screen awake while one is running.
    val view = LocalView.current
    DisposableEffect(transfer != null) {
        view.keepScreenOn = transfer != null
        onDispose { view.keepScreenOn = false }
    }

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
                    snapshot.shunt?.let { shunt ->
                        item {
                            ShuntCard(
                                shunt = shunt,
                                indoor = snapshot.indoor,
                                enabled = state == ConnectionState.READY && !busy,
                                onToggle = { viewModel.setShuntEnabled(!shunt.enabled) },
                                onEditCurve = { dialog = Dialog.Curve },
                                onEditActuator = { dialog = Dialog.Actuator },
                                onEditIndoor = { dialog = Dialog.Indoor },
                                onJog = { dir -> viewModel.jogShunt(dir, JOG_SECONDS) },
                            )
                        }
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
                            transfer = transfer,
                            enabled = state == ConnectionState.READY && !busy,
                            onCheck = viewModel::checkForFirmwareUpdate,
                            onInstall = { release -> dialog = Dialog.Firmware(release) },
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
                            onSetRole = { role -> viewModel.setSensorRole(sensor.id, role) },
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

        Dialog.Curve -> CurveDialog(
            shunt = telemetry?.shunt,
            onDismiss = { dialog = null },
            onConfirm = { slope, offset, target, min, max ->
                dialog = null
                viewModel.setCurve(slope, offset, target, min, max)
            },
        )

        Dialog.Actuator -> ActuatorDialog(
            shunt = telemetry?.shunt,
            onDismiss = { dialog = null },
            onConfirm = { burst, pause, tolerance ->
                dialog = null
                viewModel.setActuator(burst, pause, tolerance)
            },
        )

        Dialog.Indoor -> IndoorDialog(
            indoor = telemetry?.indoor,
            onDismiss = { dialog = null },
            onConfirm = { topic, gain, maxTrim, stale ->
                dialog = null
                viewModel.setIndoor(topic, gain, maxTrim, stale)
            },
        )

        is Dialog.Firmware -> FirmwareDialog(
            release = current.release,
            installed = telemetry?.firmwareVersion,
            deviceOnline = telemetry?.wifiConnected == true,
            onDismiss = { dialog = null },
            onConfirm = { overBle ->
                dialog = null
                if (overBle) {
                    viewModel.installFirmwareOverBle(current.release)
                } else {
                    viewModel.installFirmware(current.release.tag)
                }
            },
        )

        null -> Unit
    }
}

private sealed interface Dialog {
    data object Wifi : Dialog
    data object Mqtt : Dialog
    data object Thresholds : Dialog
    data object Curve : Dialog
    data object Actuator : Dialog
    data object Indoor : Dialog
    data class Firmware(val release: FirmwareRelease) : Dialog
}

/** Long enough to see the actuator move, short enough to undo by hand. */
private const val JOG_SECONDS = 5

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
private fun ShuntCard(
    shunt: ShuntStatus,
    indoor: IndoorStatus?,
    enabled: Boolean,
    onToggle: () -> Unit,
    onEditCurve: () -> Unit,
    onEditActuator: () -> Unit,
    onEditIndoor: () -> Unit,
    onJog: (ShuntDirection) -> Unit,
) {
    Card(
        Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(
            containerColor = if (shunt.direction == ShuntDirection.IDLE) {
                MaterialTheme.colorScheme.surfaceVariant
            } else {
                MaterialTheme.colorScheme.tertiaryContainer
            },
        ),
    ) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Row(
                Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text("Shunt valve", style = MaterialTheme.typography.titleMedium)
                Text(
                    when (shunt.direction) {
                        ShuntDirection.WARMER -> "opening"
                        ShuntDirection.COLDER -> "closing"
                        ShuntDirection.IDLE -> shunt.state
                    },
                    style = MaterialTheme.typography.titleMedium,
                )
            }

            Text(
                "Supply " + (shunt.supplyC?.formatC() ?: "—") +
                    ", aiming for " + (shunt.setpointC?.formatC() ?: "—"),
                style = MaterialTheme.typography.bodyMedium,
            )
            Text(
                "Outdoor " + (shunt.outdoorC?.formatC() ?: "—") + " · " + burstSummary(shunt),
                style = MaterialTheme.typography.bodySmall,
            )
            shunt.reason?.let {
                Text(
                    "Holding: $it",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.error,
                )
            }

            Text(
                "Curve: ${shunt.targetC.formatPlain()} °C indoors, slope " +
                    shunt.slope.formatPlain() + ", offset " + shunt.offsetC.formatSigned() +
                    " °C, between ${shunt.minSupplyC.formatPlain()} and " +
                    "${shunt.maxSupplyC.formatPlain()} °C",
                style = MaterialTheme.typography.bodySmall,
            )
            Text(indoorSummary(indoor), style = MaterialTheme.typography.bodySmall)

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                FilledTonalButton(
                    onClick = onToggle,
                    enabled = enabled,
                    modifier = Modifier.weight(1f),
                ) { Text(if (shunt.enabled) "Stop" else "Start") }
                OutlinedButton(
                    onClick = onEditCurve,
                    enabled = enabled,
                    modifier = Modifier.weight(1f),
                ) { Text("Curve") }
            }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(
                    onClick = onEditActuator,
                    enabled = enabled,
                    modifier = Modifier.weight(1f),
                ) { Text("Bursts") }
                OutlinedButton(
                    onClick = onEditIndoor,
                    enabled = enabled,
                    modifier = Modifier.weight(1f),
                ) { Text("Indoor") }
            }
            // Moving the valve by hand is how the wiring gets checked, so it
            // stays available while automatic control is switched off.
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(
                    onClick = { onJog(ShuntDirection.COLDER) },
                    enabled = enabled,
                    modifier = Modifier.weight(1f),
                ) { Text("Colder ${JOG_SECONDS}s") }
                OutlinedButton(
                    onClick = { onJog(ShuntDirection.WARMER) },
                    enabled = enabled,
                    modifier = Modifier.weight(1f),
                ) { Text("Warmer ${JOG_SECONDS}s") }
            }
        }
    }
}

private fun indoorSummary(indoor: IndoorStatus?): String {
    if (indoor == null || indoor.topic.isEmpty()) {
        return "No indoor topic set, so the curve runs untrimmed."
    }
    val celsius = indoor.celsius
        ?: return "Waiting for an indoor temperature on ${indoor.topic}."
    if (!indoor.fresh) {
        return "Indoor " + celsius.formatC() + " is too old to use" +
            (indoor.ageMs?.let { " (${formatAge(it)})" } ?: "") + "; the curve runs untrimmed."
    }
    return "Indoor " + celsius.formatC() + ", trimming the supply by " +
        indoor.trimC.formatSigned() + " °C."
}

@Composable
private fun CurveDialog(
    shunt: ShuntStatus?,
    onDismiss: () -> Unit,
    onConfirm: (slope: Double, offset: Double, target: Double, min: Double, max: Double) -> Unit,
) {
    var slope by remember { mutableStateOf(shunt?.slope?.formatPlain().orEmpty()) }
    var offset by remember { mutableStateOf(shunt?.offsetC?.formatPlain().orEmpty()) }
    var target by remember { mutableStateOf(shunt?.targetC?.formatPlain().orEmpty()) }
    var min by remember { mutableStateOf(shunt?.minSupplyC?.formatPlain().orEmpty()) }
    var max by remember { mutableStateOf(shunt?.maxSupplyC?.formatPlain().orEmpty()) }

    val slopeValue = slope.toTemperature()
    val offsetValue = offset.toTemperature()
    val targetValue = target.toTemperature()
    val minValue = min.toTemperature()
    val maxValue = max.toTemperature()
    val valid = slopeValue != null && offsetValue != null && targetValue != null &&
        minValue != null && maxValue != null && minValue < maxValue

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Heating curve") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(
                    "The supply temperature is set from the outdoor temperature alone: " +
                        "supply = target + slope × (target − outdoor) + offset. A steeper " +
                        "slope adds more heat as it gets colder outside; the offset shifts " +
                        "the whole curve up or down.",
                    style = MaterialTheme.typography.bodySmall,
                )
                OutlinedTextField(
                    value = target,
                    onValueChange = { target = it },
                    label = { Text("Indoor target (°C)") },
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                )
                OutlinedTextField(
                    value = slope,
                    onValueChange = { slope = it },
                    label = { Text("Slope") },
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                )
                OutlinedTextField(
                    value = offset,
                    onValueChange = { offset = it },
                    label = { Text("Offset (°C)") },
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                )
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedTextField(
                        value = min,
                        onValueChange = { min = it },
                        label = { Text("Min supply") },
                        singleLine = true,
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                        modifier = Modifier.weight(1f),
                    )
                    OutlinedTextField(
                        value = max,
                        onValueChange = { max = it },
                        label = { Text("Max supply") },
                        singleLine = true,
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                        modifier = Modifier.weight(1f),
                    )
                }
                if (valid) {
                    // The two ends of a Swedish winter, so the numbers can be
                    // sanity checked before they are sent.
                    Text(
                        "At +5 °C outside: " + curvePreview(slopeValue, offsetValue, targetValue, minValue, maxValue, 5.0) +
                            ", at −15 °C: " + curvePreview(slopeValue, offsetValue, targetValue, minValue, maxValue, -15.0),
                        style = MaterialTheme.typography.bodySmall,
                    )
                } else {
                    Text(
                        "Every field must be a number, and the minimum supply " +
                            "temperature must be below the maximum.",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.error,
                    )
                }
            }
        },
        confirmButton = {
            TextButton(
                onClick = {
                    onConfirm(slopeValue ?: 0.0, offsetValue ?: 0.0, targetValue ?: 0.0, minValue ?: 0.0, maxValue ?: 0.0)
                },
                enabled = valid,
            ) { Text("Apply") }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

private fun curvePreview(
    slope: Double,
    offset: Double,
    target: Double,
    min: Double,
    max: Double,
    outdoor: Double,
): String = (target + slope * (target - outdoor) + offset).coerceIn(min, max).formatC()

private fun burstSummary(shunt: ShuntStatus): String = when {
    shunt.bursts > 0 -> "${shunt.bursts} correction${plural(shunt.bursts)} warmer in a row"
    shunt.bursts < 0 -> "${-shunt.bursts} correction${plural(-shunt.bursts)} colder in a row"
    else -> "within tolerance"
}

private fun plural(count: Int): String = if (count == 1) "" else "s"

@Composable
private fun ActuatorDialog(
    shunt: ShuntStatus?,
    onDismiss: () -> Unit,
    onConfirm: (burstMs: Int, pauseS: Int, toleranceC: Double) -> Unit,
) {
    var burst by remember { mutableStateOf(shunt?.burstMs?.toString().orEmpty()) }
    var pause by remember { mutableStateOf(shunt?.pauseS?.toString().orEmpty()) }
    var tolerance by remember { mutableStateOf(shunt?.toleranceC?.formatPlain().orEmpty()) }

    val burstValue = burst.trim().toIntOrNull()
    val pauseValue = pause.trim().toIntOrNull()
    val toleranceValue = tolerance.toTemperature()
    val valid = burstValue != null && burstValue in 100..30000 &&
        pauseValue != null && pauseValue in 1..600 &&
        toleranceValue != null && toleranceValue >= 0.1 && toleranceValue <= 10.0

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Corrections") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(
                    "While the supply temperature is further than the tolerance from " +
                        "what the curve asks for, the valve is nudged in short bursts " +
                        "with a pause between them. The pause is what lets the pipe " +
                        "sensor show what the last burst did, so too short a one makes " +
                        "it overshoot and hunt.",
                    style = MaterialTheme.typography.bodySmall,
                )
                OutlinedTextField(
                    value = burst,
                    onValueChange = { burst = it },
                    label = { Text("Burst length (ms)") },
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                )
                OutlinedTextField(
                    value = pause,
                    onValueChange = { pause = it },
                    label = { Text("Pause between bursts (s)") },
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                )
                OutlinedTextField(
                    value = tolerance,
                    onValueChange = { tolerance = it },
                    label = { Text("Tolerance (°C)") },
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                )
                if (!valid) {
                    Text(
                        "Burst must be 100–30000 ms, pause 1–600 s and tolerance " +
                            "0.1–10 °C.",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.error,
                    )
                }
            }
        },
        confirmButton = {
            TextButton(
                onClick = { onConfirm(burstValue ?: 0, pauseValue ?: 0, toleranceValue ?: 0.0) },
                enabled = valid,
            ) { Text("Apply") }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

@Composable
private fun IndoorDialog(
    indoor: IndoorStatus?,
    onDismiss: () -> Unit,
    onConfirm: (topic: String, gain: Double, maxTrim: Double, staleS: Int) -> Unit,
) {
    var topic by remember { mutableStateOf(indoor?.topic.orEmpty()) }
    var gain by remember { mutableStateOf(indoor?.gain?.formatPlain().orEmpty()) }
    var maxTrim by remember { mutableStateOf(indoor?.maxTrimC?.formatPlain().orEmpty()) }
    var stale by remember { mutableStateOf(indoor?.staleS?.toString().orEmpty()) }

    val gainValue = gain.toTemperature()
    val maxTrimValue = maxTrim.toTemperature()
    val staleValue = stale.trim().toIntOrNull()
    val valid = gainValue != null && gainValue >= 0 && maxTrimValue != null &&
        maxTrimValue >= 0 && staleValue != null && staleValue >= 60

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Indoor trim") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(
                    "The device does not measure indoors itself; it subscribes to a " +
                        "topic on the broker. A reading older than the limit below is " +
                        "ignored, so a sensor that goes quiet cannot leave the house " +
                        "cold. Leave the topic empty to switch the trim off.",
                    style = MaterialTheme.typography.bodySmall,
                )
                OutlinedTextField(
                    value = topic,
                    onValueChange = { topic = it },
                    label = { Text("MQTT topic") },
                    placeholder = { Text("home/livingroom/temperature") },
                    singleLine = true,
                )
                OutlinedTextField(
                    value = gain,
                    onValueChange = { gain = it },
                    label = { Text("Supply degrees per degree indoors") },
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                )
                OutlinedTextField(
                    value = maxTrim,
                    onValueChange = { maxTrim = it },
                    label = { Text("Largest trim either way (°C)") },
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                )
                OutlinedTextField(
                    value = stale,
                    onValueChange = { stale = it },
                    label = { Text("Ignore readings older than (s)") },
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                )
                if (!valid) {
                    Text(
                        "Gain and trim limit cannot be negative, and the staleness " +
                            "limit must be at least 60 s.",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.error,
                    )
                }
            }
        },
        confirmButton = {
            TextButton(
                onClick = {
                    onConfirm(topic.trim(), gainValue ?: 0.0, maxTrimValue ?: 0.0, staleValue ?: 0)
                },
                enabled = valid,
            ) { Text("Apply") }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
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
    transfer: FirmwareTransfer?,
    enabled: Boolean,
    onCheck: () -> Unit,
    onInstall: (FirmwareRelease) -> Unit,
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

            if (transfer != null) {
                Text(
                    when (transfer) {
                        is FirmwareTransfer.Downloading ->
                            "Downloading to the phone: ${transfer.bytes / 1024} of " +
                                "${transfer.total / 1024} kB"
                        is FirmwareTransfer.Sending ->
                            "Sending over Bluetooth: ${transfer.bytes / 1024} of " +
                                "${transfer.total / 1024} kB"
                    },
                    style = MaterialTheme.typography.bodyMedium,
                )
                LinearProgressIndicator(
                    progress = { transfer.fraction },
                    modifier = Modifier.fillMaxWidth(),
                )
            } else if (ota != null) {
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
                    OtaState.RECEIVING -> {
                        Text(
                            "Receiving over Bluetooth" + (ota.percent?.let { " – $it %" } ?: "…"),
                            style = MaterialTheme.typography.bodyMedium,
                        )
                        ota.percent?.let { percent ->
                            LinearProgressIndicator(
                                progress = { percent / 100f },
                                modifier = Modifier.fillMaxWidth(),
                            )
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
                    onClick = { latest?.let(onInstall) },
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
    release: FirmwareRelease,
    installed: String?,
    deviceOnline: Boolean,
    onDismiss: () -> Unit,
    onConfirm: (overBle: Boolean) -> Unit,
) {
    // Over Wi-Fi the device fetches the image itself, which is much faster, so
    // it is the default whenever the device is actually online.
    var overBle by remember { mutableStateOf(!deviceOnline) }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Install firmware ${release.tag}?") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(
                    "${release.tag} replaces " + (installed ?: "the running firmware") +
                        ". The device restarts once the image is in place, and the heater " +
                        "is off for the few seconds that takes. If the new firmware cannot " +
                        "reach Wi-Fi within two minutes, the device goes back to the " +
                        "current version.",
                )
                Column(Modifier.selectableGroup()) {
                    TransferOption(
                        selected = !overBle,
                        enabled = deviceOnline,
                        title = "Over Wi-Fi",
                        subtitle = if (deviceOnline) {
                            "The device downloads it from GitHub, about a minute."
                        } else {
                            "Unavailable: the device is not on Wi-Fi."
                        },
                        onSelect = { overBle = false },
                    )
                    TransferOption(
                        selected = overBle,
                        enabled = true,
                        title = "Over Bluetooth",
                        subtitle = "This phone downloads it and sends it to the device, " +
                            "a few minutes. Keep the app open and stay nearby.",
                        onSelect = { overBle = true },
                    )
                }
            }
        },
        confirmButton = { TextButton(onClick = { onConfirm(overBle) }) { Text("Install") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

@Composable
private fun TransferOption(
    selected: Boolean,
    enabled: Boolean,
    title: String,
    subtitle: String,
    onSelect: () -> Unit,
) {
    Row(
        Modifier
            .fillMaxWidth()
            .selectable(selected = selected, enabled = enabled, role = Role.RadioButton, onClick = onSelect)
            .padding(vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        RadioButton(selected = selected, onClick = null, enabled = enabled)
        Spacer(Modifier.width(8.dp))
        Column {
            Text(title, style = MaterialTheme.typography.bodyMedium)
            Text(
                subtitle,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun SensorCard(
    sensor: SensorReading,
    enabled: Boolean,
    onSetRole: (SensorRole) -> Unit,
) {
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
            sensor.ageMs?.let {
                Text(
                    "read ${formatAge(it)} ago",
                    style = MaterialTheme.typography.bodySmall,
                )
            }
            // Which job this sensor does. Picking the role it already has
            // clears it, so a sensor can be taken out of service too.
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                for (role in listOf(SensorRole.WATER, SensorRole.OUTDOOR, SensorRole.SUPPLY)) {
                    FilterChip(
                        selected = sensor.role == role,
                        onClick = {
                            onSetRole(if (sensor.role == role) SensorRole.NONE else role)
                        },
                        enabled = enabled,
                        label = { Text(role.label) },
                    )
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

/** For a correction, where the sign is the point: "+1.5", "-0.5", "0.0". */
private fun Double.formatSigned(): String = String.format(Locale.US, "%+.1f", this)

/** Accepts a comma as the decimal separator, which a Swedish keyboard gives. */
private fun String.toTemperature(): Double? = trim().replace(',', '.').toDoubleOrNull()

private fun formatAge(ms: Long): String {
    val seconds = ms / 1000
    return when {
        seconds < 60 -> "${seconds}s"
        seconds < 3600 -> "${seconds / 60}m ${seconds % 60}s"
        else -> "${seconds / 3600}h ${(seconds % 3600) / 60}m"
    }
}
