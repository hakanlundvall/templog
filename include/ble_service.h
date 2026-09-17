#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_MAX_TEMP_SENSORS 8
#define BLE_ROM_CODE_HEX_LEN 16 /* 8 bytes -> 16 hex chars, plus NUL */
#define BLE_MQTT_URL_LEN 100    /* matches the broker URL buffer in main.c */

/* One temperature reading reported to the Raspberry Pi. */
typedef struct {
    char rom_code_hex[BLE_ROM_CODE_HEX_LEN + 1];
    float value_c;
    uint32_t age_ms; /* time since the last good reading of this sensor */
    bool valid;      /* false if this sensor has never produced a good reading */
    bool is_water_sensor;
} ble_temp_reading_t;

/* Full telemetry snapshot pushed from the application into the BLE layer. */
typedef struct {
    ble_temp_reading_t readings[BLE_MAX_TEMP_SENSORS];
    int num_readings;
    bool wifi_connected;
    char wifi_ssid[33];
    bool mqtt_connected;
    char mqtt_url[BLE_MQTT_URL_LEN];
    bool heater_on;
    float heater_on_threshold_c;
    float heater_off_threshold_c;
    /* 0 = automatic, 1 = forced off until conditions met again, 2 = forced off until explicitly started */
    int heater_force_state;
} ble_telemetry_t;

typedef enum {
    BLE_CMD_SET_WIFI,
    BLE_CMD_SET_MQTT,
    BLE_CMD_SET_WATER_SENSOR,
    BLE_CMD_SET_THRESHOLDS,
    BLE_CMD_HEATER_FORCE_OFF,
    BLE_CMD_HEATER_ON,
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
            char rom_code_hex[BLE_ROM_CODE_HEX_LEN + 1];
        } water_sensor;
        struct {
            float on_c;
            float off_c;
        } thresholds;
        struct {
            ble_heater_force_mode_t mode;
        } heater_force_off;
    } data;
} ble_command_t;

/* Initializes the NimBLE GATT server and starts advertising.
 * Decoded commands are pushed to command_queue (items of type ble_command_t)
 * from the NimBLE host task; the application must drain this queue itself. */
void ble_service_init(QueueHandle_t command_queue);

/* Publishes a new telemetry snapshot and notifies any subscribed client. */
void ble_service_update_telemetry(const ble_telemetry_t *telemetry);

/* Reports the outcome of the most recently processed command so the
 * Raspberry Pi can confirm it was applied. */
void ble_service_report_status(const char *cmd, bool ok, const char *error);

#ifdef __cplusplus
}
#endif
