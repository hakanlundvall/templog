package io.github.hakanlundvall.templog.ble

import org.json.JSONObject
import java.util.UUID

/**
 * Shared BLE protocol constants matching the ESP32 firmware (src/ble_service.c)
 * and the Raspberry Pi client (rpi/templog_ble/protocol.py).
 *
 * What the device reports is split over three read characteristics rather
 * than one, because Android's GATT stack truncates a characteristic read at
 * 512 bytes without saying so, and the sensor readings alone can approach
 * that. The client reads all three and merges them into one [Telemetry].
 *  * TELEMETRY - read + notify. Live state: Wi-Fi and MQTT connection, heater
 *                output and mode, the shunt valve control, OTA progress.
 *  * SENSORS   - read + notify. The DS18B20 readings and their roles.
 *  * CONFIG    - read + notify. Settings, which only change on command, so
 *                this one is notified only when something actually changed.
 *  * COMMAND   - write (with response). Body is a JSON command object.
 *  * STATUS    - read + notify. JSON {"cmd", "ok", "error"} describing the
 *                outcome of the most recently processed command.
 *
 * Both notifications carry a fixed one byte placeholder rather than the value
 * itself, because a notification cannot exceed the negotiated ATT MTU. A
 * notification only means "something changed"; the authoritative value always
 * comes from a follow up read (Android performs the GATT long read
 * transparently).
 */
object Protocol {
    const val DEVICE_NAME = "templog"

    val SERVICE_UUID: UUID = UUID.fromString("6e400000-b5a3-f393-e0a9-e50e24dcca9e")
    val TELEMETRY_CHAR_UUID: UUID = UUID.fromString("6e400001-b5a3-f393-e0a9-e50e24dcca9e")
    val SENSORS_CHAR_UUID: UUID = UUID.fromString("6e400005-b5a3-f393-e0a9-e50e24dcca9e")
    val CONFIG_CHAR_UUID: UUID = UUID.fromString("6e400006-b5a3-f393-e0a9-e50e24dcca9e")
    val COMMAND_CHAR_UUID: UUID = UUID.fromString("6e400002-b5a3-f393-e0a9-e50e24dcca9e")
    val STATUS_CHAR_UUID: UUID = UUID.fromString("6e400003-b5a3-f393-e0a9-e50e24dcca9e")
    val FIRMWARE_CHAR_UUID: UUID = UUID.fromString("6e400004-b5a3-f393-e0a9-e50e24dcca9e")

    /** Client Characteristic Configuration descriptor, used to subscribe. */
    val CCCD_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

    const val CMD_SET_WIFI = "set_wifi"
    const val CMD_SET_MQTT = "set_mqtt"
    const val CMD_SET_SENSOR_ROLE = "set_sensor_role"
    const val CMD_SET_THRESHOLDS = "set_thresholds"
    const val CMD_HEATER_OFF = "heater_off"
    const val CMD_HEATER_ON = "heater_on"
    const val CMD_OTA_UPDATE = "ota_update"
    const val CMD_OTA_BLE_BEGIN = "ota_ble_begin"
    const val CMD_OTA_BLE_END = "ota_ble_end"
    const val CMD_OTA_BLE_ABORT = "ota_ble_abort"
    const val CMD_SET_SHUNT = "set_shunt"
    const val CMD_SHUNT_JOG = "shunt_jog"

    /** Bytes of offset that prefix every chunk on FIRMWARE_CHAR_UUID. */
    const val FIRMWARE_CHUNK_HEADER = 4

    /** The firmware rejects a write larger than this, header included. */
    const val FIRMWARE_CHUNK_MAX = 256

    const val HEATER_OFF_UNTIL_CONDITIONS = "until_conditions"
    const val HEATER_OFF_UNTIL_STARTED = "until_started"

    fun setWifi(ssid: String, password: String): JSONObject =
        JSONObject()
            .put("cmd", CMD_SET_WIFI)
            .put("ssid", ssid)
            .put("password", password)

    fun setMqtt(url: String): JSONObject =
        JSONObject()
            .put("cmd", CMD_SET_MQTT)
            .put("url", url)

    /** Says what the sensor [romCodeHex] is used for; [SensorRole.NONE] frees it. */
    fun setSensorRole(romCodeHex: String, role: SensorRole): JSONObject =
        JSONObject()
            .put("cmd", CMD_SET_SENSOR_ROLE)
            .put("id", romCodeHex)
            .put("role", role.wire)

    /**
     * The heating curve, which turns the outdoor temperature into a supply
     * temperature setpoint: supply = target + slope * (target - outdoor) +
     * offset, clamped to [minC] .. [maxC].
     */
    fun setCurve(
        slope: Double,
        offsetC: Double,
        targetC: Double,
        minC: Double,
        maxC: Double,
    ): JSONObject =
        JSONObject()
            .put("cmd", CMD_SET_SHUNT)
            .put("slope", slope)
            .put("offset", offsetC)
            .put("target", targetC)
            .put("min", minC)
            .put("max", maxC)

    /**
     * What the actuator does: [travelS] is its end to end run time and
     * [authorityC] the supply temperature span that full travel covers.
     * Whether control is running is left alone.
     */
    fun setActuator(travelS: Int, authorityC: Double): JSONObject =
        JSONObject()
            .put("cmd", CMD_SET_SHUNT)
            .put("travel", travelS)
            .put("authority", authorityC)

    /** Switches shunt control on or off, leaving every other setting alone. */
    fun setShuntEnabled(enabled: Boolean): JSONObject =
        JSONObject()
            .put("cmd", CMD_SET_SHUNT)
            .put("enabled", enabled)

    /**
     * The indoor trim. The indoor temperature is not measured by the device:
     * it subscribes to [topic], and ignores a reading older than [staleS]
     * seconds so a silent sensor cannot leave the house cold. An empty topic
     * switches the trim off.
     */
    fun setIndoor(topic: String, gain: Double, maxTrimC: Double, staleS: Int): JSONObject =
        JSONObject()
            .put("cmd", CMD_SET_SHUNT)
            .put("indoorTopic", topic)
            .put("indoorGain", gain)
            .put("indoorMax", maxTrimC)
            .put("indoorStale", staleS)

    /** Runs the actuator by hand, for checking the wiring and timing its travel. */
    fun shuntJog(dir: ShuntDirection, ms: Int): JSONObject =
        JSONObject()
            .put("cmd", CMD_SHUNT_JOG)
            .put("dir", dir.wire)
            .put("ms", ms)

    fun setThresholds(onC: Double, offC: Double): JSONObject =
        JSONObject()
            .put("cmd", CMD_SET_THRESHOLDS)
            .put("on", onC)
            .put("off", offC)

    fun heaterOff(untilStarted: Boolean): JSONObject =
        JSONObject()
            .put("cmd", CMD_HEATER_OFF)
            .put("mode", if (untilStarted) HEATER_OFF_UNTIL_STARTED else HEATER_OFF_UNTIL_CONDITIONS)

    fun heaterOn(): JSONObject = JSONObject().put("cmd", CMD_HEATER_ON)

    /**
     * Asks the device to download and install the firmware.bin of the GitHub
     * release [tag] ("latest" or e.g. "v1.2.0"). Unless [force] is set, the
     * device skips a release whose version it is already running.
     */
    fun otaUpdate(tag: String, force: Boolean = false): JSONObject =
        JSONObject()
            .put("cmd", CMD_OTA_UPDATE)
            .put("tag", tag)
            .put("force", force)

    /**
     * Opens a transfer of an image the phone already has. [crc32] is a
     * standard (zlib) CRC-32 of the whole image, which the device checks
     * before it accepts the update.
     */
    fun otaBleBegin(size: Int, crc32: Long, version: String?, force: Boolean = false): JSONObject =
        JSONObject()
            .put("cmd", CMD_OTA_BLE_BEGIN)
            .put("size", size)
            .put("crc32", crc32)
            .put("force", force)
            .apply { if (version != null) put("ver", version) }

    fun otaBleEnd(): JSONObject = JSONObject().put("cmd", CMD_OTA_BLE_END)

    fun otaBleAbort(): JSONObject = JSONObject().put("cmd", CMD_OTA_BLE_ABORT)
}

/** Where a firmware update stands, mirroring the telemetry "ota" object. */
enum class OtaState {
    IDLE,

    /** The running firmware was just installed and is not confirmed yet. */
    VERIFYING,
    DOWNLOADING,

    /** The image is arriving over BLE from this phone. */
    RECEIVING,

    /** The requested release is the version already running. */
    UP_TO_DATE,

    /** Installed; the device is about to restart into it. */
    REBOOTING,
    FAILED,

    /** Anything the firmware may add later. */
    UNKNOWN,
    ;

    companion object {
        fun fromName(value: String): OtaState = when (value) {
            "idle" -> IDLE
            "verifying" -> VERIFYING
            "downloading" -> DOWNLOADING
            "receiving" -> RECEIVING
            "uptodate" -> UP_TO_DATE
            "rebooting" -> REBOOTING
            "failed" -> FAILED
            else -> UNKNOWN
        }
    }
}

data class OtaStatus(
    val state: OtaState,
    /** Download progress 0-100, null when not known. */
    val percent: Int?,
    /** The version being installed, once the device has read it from the image. */
    val version: String?,
    val error: String?,
) {
    val inProgress: Boolean
        get() = state == OtaState.DOWNLOADING || state == OtaState.RECEIVING ||
            state == OtaState.REBOOTING
}

/** What a sensor is used for. A sensor holds at most one role. */
enum class SensorRole(val wire: String, val label: String) {
    NONE("none", "no role"),

    /** Drives the heater thermostat. */
    WATER("water", "water"),

    /** Outdoor temperature, which the heating curve is drawn from. */
    OUTDOOR("outdoor", "outdoor"),

    /** Radiator supply temperature, the one the shunt valve regulates. */
    SUPPLY("supply", "supply"),
    ;

    companion object {
        fun fromWire(value: String?): SensorRole =
            entries.firstOrNull { it.wire == value } ?: NONE
    }
}

/** One DS18B20 reading as reported in the telemetry document's "t" array. */
data class SensorReading(
    val id: String,
    /** null when the sensor has never produced a valid reading. */
    val celsius: Double?,
    /** Milliseconds since this sensor's last good reading, null if never read. */
    val ageMs: Long?,
    val role: SensorRole,
)

/** Which way the actuator is being driven right now. */
enum class ShuntDirection(val wire: String) {
    IDLE("idle"),

    /** Clockwise: more hot water into the radiator circuit. */
    WARMER("warmer"),
    COLDER("colder"),
    ;

    companion object {
        fun fromWire(value: String?): ShuntDirection =
            entries.firstOrNull { it.wire == value } ?: IDLE
    }
}

/** What the shunt controller is doing, mirroring the telemetry "shunt" object. */
data class ShuntStatus(
    val enabled: Boolean,
    /** "off", "running", "holding" or "manual". */
    val state: String,
    val direction: ShuntDirection,
    /** Why it is holding the valve where it is; null while it is regulating. */
    val reason: String?,
    /** The supply temperature the curve asks for, null while it cannot be computed. */
    val setpointC: Double?,
    /** The measured supply temperature, null when that sensor has never been read. */
    val supplyC: Double?,
    val outdoorC: Double?,
    /** 0 = fully cold, 1 = fully warm. An estimate from run time, not a measurement. */
    val position: Double,
    val slope: Double,
    val offsetC: Double,
    val targetC: Double,
    val minSupplyC: Double,
    val maxSupplyC: Double,
    val travelS: Int,
    val authorityC: Double,
) {
    val holding: Boolean
        get() = state == "holding"
}

/** The indoor temperature the device subscribes to, and how it trims the curve. */
data class IndoorStatus(
    /** Empty when no topic is configured, which switches the trim off. */
    val topic: String,
    val celsius: Double?,
    val ageMs: Long?,
    /** false when the reading is too old to be used, so the trim is ignored. */
    val fresh: Boolean,
    /** What the trim currently adds to the setpoint. */
    val trimC: Double,
    val gain: Double,
    val maxTrimC: Double,
    val staleS: Int,
)

/** How the heater is currently being overridden, mirroring "forceState". */
enum class HeaterForceState {
    /** 0 - thresholds decide. */
    AUTOMATIC,

    /** 1 - forced off until the start conditions are met again. */
    OFF_UNTIL_CONDITIONS,

    /** 2 - forced off until an explicit heater_on command. */
    OFF_UNTIL_STARTED,

    /** Anything the firmware may add later. */
    UNKNOWN,
    ;

    companion object {
        fun fromInt(value: Int): HeaterForceState = when (value) {
            0 -> AUTOMATIC
            1 -> OFF_UNTIL_CONDITIONS
            2 -> OFF_UNTIL_STARTED
            else -> UNKNOWN
        }
    }
}

/** The most recent Wi-Fi disconnect the ESP32 saw since it booted. */
data class WifiDisconnect(
    /** ESP-IDF `wifi_err_reason_t` code. */
    val reason: Int,
    val rssi: Int,
    val ageMs: Long,
) {
    /** A short name for the common reason codes, or null for the rest. */
    val reasonName: String?
        get() = when (reason) {
            1 -> "unspecified"
            2 -> "auth expired"
            3 -> "deauthenticated by AP"
            4 -> "disassociated, inactivity"
            5 -> "AP has too many stations"
            6 -> "not authenticated"
            7 -> "not associated"
            8 -> "disassociated by AP"
            15 -> "4-way handshake timeout"
            16 -> "group key update timeout"
            200 -> "beacon timeout"
            201 -> "no AP found"
            202 -> "auth failed"
            203 -> "association failed"
            204 -> "handshake timeout"
            205 -> "connection failed"
            else -> null
        }
}

/** A decoded snapshot, merged from the three documents the device publishes. */
data class Telemetry(
    val sensors: List<SensorReading>,
    val wifiConnected: Boolean,
    val wifiSsid: String,
    /** Current signal strength in dBm; null while not associated or on older firmware. */
    val wifiRssi: Int?,
    /** Disconnect events since boot; null on firmware that does not report it. */
    val wifiDisconnects: Long?,
    val lastWifiDisconnect: WifiDisconnect?,
    val mqttConnected: Boolean,
    val mqttUrl: String,
    val heaterOn: Boolean,
    val heaterOnThresholdC: Double,
    val heaterOffThresholdC: Double,
    val forceState: HeaterForceState,
    /** Version of the running firmware; null on firmware that does not report it. */
    val firmwareVersion: String?,
    /** null on firmware without OTA support. */
    val ota: OtaStatus?,
    /** null on firmware that does not control a shunt valve. */
    val shunt: ShuntStatus?,
    /** null on firmware that does not control a shunt valve. */
    val indoor: IndoorStatus?,
) {
    val waterSensor: SensorReading?
        get() = sensors.firstOrNull { it.role == SensorRole.WATER }

    companion object {
        fun parse(json: String): Telemetry {
            val root = JSONObject(json)
            val array = root.optJSONArray("t")
            val sensors = buildList {
                for (i in 0 until (array?.length() ?: 0)) {
                    val item = array!!.getJSONObject(i)
                    add(
                        SensorReading(
                            id = item.optString("id"),
                            celsius = if (item.has("c")) item.getDouble("c") else null,
                            ageMs = if (item.has("age")) item.getLong("age") else null,
                            role = if (item.has("role")) {
                                SensorRole.fromWire(item.optString("role"))
                            } else if (item.optBoolean("water", false)) {
                                // Firmware from before roles existed.
                                SensorRole.WATER
                            } else {
                                SensorRole.NONE
                            },
                        ),
                    )
                }
            }
            val wifi = root.optJSONObject("wifi")
            val mqtt = root.optJSONObject("mqtt")
            val curve = root.optJSONObject("curve")
            val ota = root.optJSONObject("ota")
            val shunt = root.optJSONObject("shunt")
            val indoor = root.optJSONObject("indoor")
            return Telemetry(
                sensors = sensors,
                wifiConnected = wifi?.optBoolean("c", false) ?: false,
                wifiSsid = root.optString("ssid"),
                wifiRssi = if (wifi?.has("rssi") == true) wifi.getInt("rssi") else null,
                wifiDisconnects = if (wifi?.has("disc") == true) wifi.getLong("disc") else null,
                lastWifiDisconnect = if (wifi?.has("reason") == true) {
                    WifiDisconnect(
                        reason = wifi.getInt("reason"),
                        rssi = wifi.optInt("discRssi", 0),
                        ageMs = wifi.optLong("discAge", 0),
                    )
                } else {
                    null
                },
                mqttConnected = mqtt?.optBoolean("c", false) ?: false,
                mqttUrl = root.optString("url"),
                heaterOn = root.optBoolean("heater", false),
                heaterOnThresholdC = root.optDouble("onC", Double.NaN),
                heaterOffThresholdC = root.optDouble("offC", Double.NaN),
                forceState = HeaterForceState.fromInt(root.optInt("forceState", 0)),
                firmwareVersion = root.optString("fw").takeIf { it.isNotEmpty() },
                ota = ota?.let {
                    OtaStatus(
                        state = OtaState.fromName(it.optString("state")),
                        percent = if (it.has("pct")) it.getInt("pct") else null,
                        version = it.optString("ver").takeIf { v -> v.isNotEmpty() },
                        error = it.optString("err").takeIf { e -> e.isNotEmpty() },
                    )
                },
                // The live state and the settings arrive in different
                // documents; both are needed to describe the controller.
                shunt = shunt?.let {
                    ShuntStatus(
                        enabled = it.optBoolean("en", false),
                        state = it.optString("state", "off"),
                        direction = ShuntDirection.fromWire(it.optString("dir")),
                        reason = it.optString("why").takeIf { w -> w.isNotEmpty() },
                        setpointC = if (it.has("sp")) it.getDouble("sp") else null,
                        supplyC = if (it.has("sup")) it.getDouble("sup") else null,
                        outdoorC = if (it.has("out")) it.getDouble("out") else null,
                        position = it.optDouble("pos", 0.0),
                        slope = curve?.optDouble("slope", 1.0) ?: 1.0,
                        offsetC = curve?.optDouble("offset", 0.0) ?: 0.0,
                        targetC = curve?.optDouble("target", 21.0) ?: 21.0,
                        minSupplyC = curve?.optDouble("min", 20.0) ?: 20.0,
                        maxSupplyC = curve?.optDouble("max", 70.0) ?: 70.0,
                        travelS = curve?.optInt("travel", 120) ?: 120,
                        authorityC = curve?.optDouble("authority", 50.0) ?: 50.0,
                    )
                },
                indoor = indoor?.let {
                    IndoorStatus(
                        topic = it.optString("topic"),
                        celsius = if (shunt?.has("in") == true) shunt.getDouble("in") else null,
                        ageMs = if (shunt?.has("inAge") == true) shunt.getLong("inAge") else null,
                        fresh = shunt?.optBoolean("inFresh", false) ?: false,
                        trimC = shunt?.optDouble("trim", 0.0) ?: 0.0,
                        gain = it.optDouble("gain", 0.0),
                        maxTrimC = it.optDouble("maxTrim", 0.0),
                        staleS = it.optInt("stale", 900),
                    )
                },
            )
        }
    }
}

/** The result of the most recently processed command. */
data class CommandStatus(
    val cmd: String,
    val ok: Boolean,
    val error: String?,
) {
    companion object {
        fun parse(json: String): CommandStatus {
            val root = JSONObject(json)
            return CommandStatus(
                cmd = root.optString("cmd"),
                ok = root.optBoolean("ok", false),
                error = root.optString("error").takeIf { it.isNotEmpty() },
            )
        }
    }
}
