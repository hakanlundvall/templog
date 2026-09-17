package io.github.hakanlundvall.templog.ble

import org.json.JSONObject
import java.util.UUID

/**
 * Shared BLE protocol constants matching the ESP32 firmware (src/ble_service.c)
 * and the Raspberry Pi client (rpi/templog_ble/protocol.py).
 *
 * The GATT service exposes three characteristics:
 *  * TELEMETRY - read + notify. Value is a compact JSON document describing
 *                temperature sensors, Wi-Fi status and heater state.
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
    val COMMAND_CHAR_UUID: UUID = UUID.fromString("6e400002-b5a3-f393-e0a9-e50e24dcca9e")
    val STATUS_CHAR_UUID: UUID = UUID.fromString("6e400003-b5a3-f393-e0a9-e50e24dcca9e")

    /** Client Characteristic Configuration descriptor, used to subscribe. */
    val CCCD_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

    const val CMD_SET_WIFI = "set_wifi"
    const val CMD_SET_MQTT = "set_mqtt"
    const val CMD_SET_WATER_SENSOR = "set_water_sensor"
    const val CMD_SET_THRESHOLDS = "set_thresholds"
    const val CMD_HEATER_OFF = "heater_off"
    const val CMD_HEATER_ON = "heater_on"

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

    fun setWaterSensor(romCodeHex: String): JSONObject =
        JSONObject()
            .put("cmd", CMD_SET_WATER_SENSOR)
            .put("id", romCodeHex)

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
}

/** One DS18B20 reading as reported in the telemetry document's "t" array. */
data class SensorReading(
    val id: String,
    /** null when the sensor has never produced a valid reading. */
    val celsius: Double?,
    /** Milliseconds since this sensor's last good reading, null if never read. */
    val ageMs: Long?,
    val isWaterSensor: Boolean,
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

/** A decoded telemetry snapshot. */
data class Telemetry(
    val sensors: List<SensorReading>,
    val wifiConnected: Boolean,
    val wifiSsid: String,
    val mqttConnected: Boolean,
    val mqttUrl: String,
    val heaterOn: Boolean,
    val heaterOnThresholdC: Double,
    val heaterOffThresholdC: Double,
    val forceState: HeaterForceState,
) {
    val waterSensor: SensorReading?
        get() = sensors.firstOrNull { it.isWaterSensor }

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
                            isWaterSensor = item.optBoolean("water", false),
                        ),
                    )
                }
            }
            val wifi = root.optJSONObject("wifi")
            val mqtt = root.optJSONObject("mqtt")
            return Telemetry(
                sensors = sensors,
                wifiConnected = wifi?.optBoolean("c", false) ?: false,
                wifiSsid = wifi?.optString("ssid").orEmpty(),
                mqttConnected = mqtt?.optBoolean("c", false) ?: false,
                mqttUrl = mqtt?.optString("url").orEmpty(),
                heaterOn = root.optBoolean("heater", false),
                heaterOnThresholdC = root.optDouble("onC", Double.NaN),
                heaterOffThresholdC = root.optDouble("offC", Double.NaN),
                forceState = HeaterForceState.fromInt(root.optInt("forceState", 0)),
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
