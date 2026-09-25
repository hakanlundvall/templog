#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_MAX_TEMP_SENSORS 8
#define BLE_ROM_CODE_HEX_LEN 16 /* 8 bytes -> 16 hex chars, plus NUL */
#define BLE_MQTT_URL_LEN 100    /* matches the broker URL buffer in main.c */
#define BLE_MQTT_TOPIC_LEN 80   /* topic the indoor temperature is published on */
#define BLE_FW_VERSION_LEN 32   /* esp_app_desc_t.version, including the terminator */
#define BLE_OTA_TAG_LEN 32      /* matches OTA_TAG_MAX_LEN in ota.h */

/* One temperature reading reported to the Raspberry Pi. */
typedef struct {
    char rom_code_hex[BLE_ROM_CODE_HEX_LEN + 1];
    float value_c;
    uint32_t age_ms; /* time since the last good reading of this sensor */
    bool valid;      /* false if this sensor has never produced a good reading */
    /* "water", "outdoor" or "supply"; NULL when this sensor has no role. */
    const char *role;
} ble_temp_reading_t;

/* Full telemetry snapshot pushed from the application into the BLE layer. */
typedef struct {
    ble_temp_reading_t readings[BLE_MAX_TEMP_SENSORS];
    int num_readings;
    bool wifi_connected;
    char wifi_ssid[33];
    bool wifi_rssi_valid;             /* false while not associated */
    int8_t wifi_rssi;                 /* current AP signal strength, dBm */
    uint32_t wifi_disconnect_count;   /* STA_DISCONNECTED events since boot */
    uint8_t wifi_last_disc_reason;    /* wifi_err_reason_t of the last one; 0 = none yet */
    int8_t wifi_last_disc_rssi;       /* RSSI reported with the last disconnect, dBm */
    uint32_t wifi_last_disc_age_ms;   /* time since the last disconnect; UINT32_MAX if none */
    bool mqtt_connected;
    char mqtt_url[BLE_MQTT_URL_LEN];
    bool heater_on;
    float heater_on_threshold_c;
    float heater_off_threshold_c;
    /* 0 = automatic, 1 = forced off until conditions met again, 2 = forced off until explicitly started */
    int heater_force_state;
    /* Shunt valve control: the curve settings, the actuator, and where the
     * loop currently stands. */
    bool shunt_enabled;
    const char *shunt_state;  /* "off", "running", "holding" or "manual" */
    const char *shunt_dir;    /* "idle", "warmer" or "colder" */
    const char *shunt_reason; /* why it is holding; NULL while running */
    float shunt_setpoint_c;   /* NAN while it cannot be computed */
    float shunt_supply_c;     /* NAN when the supply sensor has never been read */
    float shunt_outdoor_c;    /* NAN when the outdoor sensor has never been read */
    /* Bursts made in a row in one direction, positive towards warmer. */
    int16_t shunt_bursts;
    float curve_slope;
    float curve_offset_c;
    float curve_target_c;
    float curve_min_supply_c;
    float curve_max_supply_c;
    uint16_t burst_ms;
    uint16_t pause_s;
    float tolerance_c;
    /* Indoor temperature, which arrives over MQTT rather than from a sensor. */
    char indoor_topic[BLE_MQTT_TOPIC_LEN];
    float indoor_c;          /* NAN when nothing has been received */
    uint32_t indoor_age_ms;  /* UINT32_MAX when nothing has been received */
    bool indoor_fresh;       /* false when the trim is being ignored */
    float indoor_trim_c;
    float indoor_gain;
    float indoor_max_c;
    uint16_t indoor_stale_s;
    char fw_version[BLE_FW_VERSION_LEN];
    /* OTA progress: state name from ota_state_name(), percent (-1 if unknown),
     * the version being installed ("" if not known) and an error or NULL. */
    const char *ota_state;
    int ota_percent;
    char ota_version[BLE_FW_VERSION_LEN];
    const char *ota_error;
} ble_telemetry_t;

typedef enum {
    BLE_CMD_SET_WIFI,
    BLE_CMD_SET_MQTT,
    BLE_CMD_SET_SENSOR_ROLE,
    BLE_CMD_SET_THRESHOLDS,
    BLE_CMD_HEATER_FORCE_OFF,
    BLE_CMD_HEATER_ON,
    BLE_CMD_OTA_UPDATE,
    BLE_CMD_OTA_BLE_BEGIN,
    BLE_CMD_OTA_BLE_END,
    BLE_CMD_OTA_BLE_ABORT,
    BLE_CMD_SET_SHUNT,
    BLE_CMD_SHUNT_JOG,
} ble_cmd_type_t;

typedef enum {
    BLE_HEATER_FORCE_OFF_UNTIL_CONDITIONS,
    BLE_HEATER_FORCE_OFF_UNTIL_STARTED,
} ble_heater_force_mode_t;

/* A command decoded from the Raspberry Pi, delivered to the application task. */
typedef struct {
    ble_cmd_type_t type;
    union {
        struct {
            char ssid[33];
            char password[65];
        } wifi;
        struct {
            char url[BLE_MQTT_URL_LEN];
        } mqtt;
        struct {
            /* "water", "outdoor" or "supply"; "none" clears the role. */
            char role[16];
            char rom_code_hex[BLE_ROM_CODE_HEX_LEN + 1];
            /* The name the client used, so the status reply it is waiting for
             * comes back under that name and not under the canonical one. */
            char cmd[20];
        } sensor_role;
        /* Every field is optional: a NAN float, a zero integer or a false
         * set_* flag means "leave that setting as it is", so one setting can
         * be changed without the sender restating the rest. */
        struct {
            int8_t enabled; /* -1 unchanged, 0 off, 1 on */
            float slope;
            float offset_c;
            float room_target_c;
            float min_supply_c;
            float max_supply_c;
            uint16_t burst_ms;
            uint16_t pause_s;
            float tolerance_c;
            float indoor_gain;
            float indoor_max_c;
            uint16_t indoor_stale_s;
            bool set_indoor_topic;
            char indoor_topic[BLE_MQTT_TOPIC_LEN];
        } shunt;
        struct {
            char dir[8]; /* "warmer" or "colder" */
            uint32_t ms;
        } jog;
        struct {
            float on_c;
            float off_c;
        } thresholds;
        struct {
            ble_heater_force_mode_t mode;
        } heater_force_off;
        struct {
            char tag[BLE_OTA_TAG_LEN];
            bool force;
        } ota;
        struct {
            uint32_t size;
            uint32_t crc32;
            char version[BLE_FW_VERSION_LEN];
            bool force;
        } ota_ble;
    } data;
} ble_command_t;

/* Firmware bytes pushed to the firmware characteristic, delivered straight to
 * the application rather than through the command queue: an image is far too
 * large to pass as queued commands. Called from the NimBLE host task, which is
 * also what paces the sender, since the next chunk is only accepted once this
 * returns. Returning false means the transfer was dropped. */
typedef bool (*ble_firmware_chunk_cb)(uint32_t offset, const uint8_t *data, size_t len);

/* Initializes the NimBLE GATT server and starts advertising.
 * Decoded commands are pushed to command_queue (items of type ble_command_t)
 * from the NimBLE host task; the application must drain this queue itself.
 * on_firmware_chunk may be NULL if the application accepts no image over BLE. */
void ble_service_init(QueueHandle_t command_queue, ble_firmware_chunk_cb on_firmware_chunk);

/* Publishes a new snapshot. It is split across three characteristics, because
 * a GATT client will not read more than 512 bytes of one attribute value and
 * Android's stack truncates silently at that point: the sensor readings (the
 * part that grows), the live state, and the settings. Subscribers are notified
 * of the first two on every update and of the settings only when they change. */
void ble_service_update_telemetry(const ble_telemetry_t *telemetry);

/* Reports the outcome of the most recently processed command so the
 * Raspberry Pi can confirm it was applied. */
void ble_service_report_status(const char *cmd, bool ok, const char *error);

#ifdef __cplusplus
}
#endif
