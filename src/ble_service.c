#include "ble_service.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_nimble_hci.h"
#include "freertos/semphr.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "cJSON.h"

/* Not exposed via a public header; declared the same way esp-idf's own
 * NimBLE examples do. */
extern void ble_store_config_init(void);

static const char *TAG = "ble_service";

/* 6e400000-b5a3-f393-e0a9-e50e24dcca9e style custom 128-bit UUIDs */
static const ble_uuid128_t SVC_UUID =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                      0x93, 0xf3, 0xa3, 0xb5, 0x00, 0x00, 0x40, 0x6e);
static const ble_uuid128_t CHR_TELEMETRY_UUID =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                      0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t CHR_COMMAND_UUID =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                      0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t CHR_STATUS_UUID =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                      0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

/* Sized separately: telemetry is by far the largest document, while an
 * incoming command is at most a Wi-Fi or broker URL payload. Keeping the
 * command buffer small matters because it lives on the NimBLE host stack. */
#define MAX_TELEMETRY_JSON_LEN 1024
#define MAX_STATUS_JSON_LEN 256
#define MAX_CMD_JSON_LEN 512

static QueueHandle_t s_command_queue;
static SemaphoreHandle_t s_state_mutex;

static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_telemetry_val_handle;
static uint16_t s_status_val_handle;
static bool s_telemetry_subscribed;
static bool s_status_subscribed;

static char s_telemetry_json[MAX_TELEMETRY_JSON_LEN];
static size_t s_telemetry_json_len;
static char s_status_json[MAX_STATUS_JSON_LEN];
static size_t s_status_json_len;

static uint8_t s_own_addr_type;

static int gatt_svr_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg);

static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &CHR_TELEMETRY_UUID.u,
                .access_cb = gatt_svr_chr_access,
                .val_handle = &s_telemetry_val_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = &CHR_COMMAND_UUID.u,
                .access_cb = gatt_svr_chr_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC,
            },
            {
                .uuid = &CHR_STATUS_UUID.u,
                .access_cb = gatt_svr_chr_access,
                .val_handle = &s_status_val_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_NOTIFY,
            },
            {0}, /* terminator */
        },
    },
    {0}, /* terminator */
};

static void send_notification(uint16_t val_handle, bool subscribed)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || !subscribed) {
        return;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat("x", 1);
    if (om == NULL) {
        return;
    }
    /* content of the notification is not used by the client; the client
     * always follows up with a read to get the authoritative value, which
     * is not truncated by the negotiated ATT MTU. */
    ble_gatts_notify_custom(s_conn_handle, val_handle, om);
}

static int read_flat_value(struct ble_gatt_access_ctxt *ctxt, const char *buf, size_t len)
{
    int rc = os_mbuf_append(ctxt->om, buf, len);
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static void handle_command_json(const char *json, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (root == NULL) {
        ble_service_report_status("unknown", false, "invalid JSON");
        return;
    }

    const cJSON *cmd_item = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    const char *cmd = cJSON_IsString(cmd_item) ? cmd_item->valuestring : NULL;
    if (cmd == NULL) {
        ble_service_report_status("unknown", false, "missing cmd field");
        cJSON_Delete(root);
        return;
    }

    ble_command_t out = {0};
    bool ok = true;
    const char *error = NULL;

    if (strcmp(cmd, "set_wifi") == 0) {
        const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
        const cJSON *pw = cJSON_GetObjectItemCaseSensitive(root, "password");
        if (!cJSON_IsString(ssid) || !cJSON_IsString(pw)) {
            ok = false;
            error = "ssid/password required";
        } else {
            out.type = BLE_CMD_SET_WIFI;
            strlcpy(out.data.wifi.ssid, ssid->valuestring, sizeof(out.data.wifi.ssid));
            strlcpy(out.data.wifi.password, pw->valuestring, sizeof(out.data.wifi.password));
        }
    } else if (strcmp(cmd, "set_mqtt") == 0) {
        const cJSON *url = cJSON_GetObjectItemCaseSensitive(root, "url");
        if (!cJSON_IsString(url) || url->valuestring[0] == '\0') {
            ok = false;
            error = "url required";
        } else if (strlen(url->valuestring) >= BLE_MQTT_URL_LEN) {
            ok = false;
            error = "url too long";
        } else {
            out.type = BLE_CMD_SET_MQTT;
            strlcpy(out.data.mqtt.url, url->valuestring, sizeof(out.data.mqtt.url));
        }
    } else if (strcmp(cmd, "set_water_sensor") == 0) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
        if (!cJSON_IsString(id)) {
            ok = false;
            error = "id required";
        } else {
            out.type = BLE_CMD_SET_WATER_SENSOR;
            strlcpy(out.data.water_sensor.rom_code_hex, id->valuestring,
                    sizeof(out.data.water_sensor.rom_code_hex));
        }
    } else if (strcmp(cmd, "set_thresholds") == 0) {
        const cJSON *on = cJSON_GetObjectItemCaseSensitive(root, "on");
        const cJSON *off = cJSON_GetObjectItemCaseSensitive(root, "off");
        if (!cJSON_IsNumber(on) || !cJSON_IsNumber(off)) {
            ok = false;
            error = "on/off required";
        } else if (on->valuedouble >= off->valuedouble) {
            ok = false;
            error = "on threshold must be lower than off threshold";
        } else {
            out.type = BLE_CMD_SET_THRESHOLDS;
            out.data.thresholds.on_c = (float)on->valuedouble;
            out.data.thresholds.off_c = (float)off->valuedouble;
        }
    } else if (strcmp(cmd, "heater_off") == 0) {
        const cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
        const char *mode_s = cJSON_IsString(mode) ? mode->valuestring : "until_conditions";
        out.type = BLE_CMD_HEATER_FORCE_OFF;
        if (strcmp(mode_s, "until_started") == 0) {
            out.data.heater_force_off.mode = BLE_HEATER_FORCE_OFF_UNTIL_STARTED;
        } else {
            out.data.heater_force_off.mode = BLE_HEATER_FORCE_OFF_UNTIL_CONDITIONS;
        }
    } else if (strcmp(cmd, "heater_on") == 0) {
        out.type = BLE_CMD_HEATER_ON;
    } else {
        ok = false;
        error = "unknown cmd";
    }

    cJSON_Delete(root);

    if (!ok) {
        ble_service_report_status(cmd, false, error);
        return;
    }

    if (s_command_queue == NULL || xQueueSend(s_command_queue, &out, 0) != pdTRUE) {
        ble_service_report_status(cmd, false, "command queue full");
    }
    /* Successful application-level status is reported later by the
     * application task via ble_service_report_status once processed. */
}

static int gatt_svr_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)arg;

    if (ble_uuid_cmp(ctxt->chr->uuid, &CHR_TELEMETRY_UUID.u) == 0) {
        if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        int rc = read_flat_value(ctxt, s_telemetry_json, s_telemetry_json_len);
        xSemaphoreGive(s_state_mutex);
        return rc;
    } else if (ble_uuid_cmp(ctxt->chr->uuid, &CHR_STATUS_UUID.u) == 0) {
        if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        int rc = read_flat_value(ctxt, s_status_json, s_status_json_len);
        xSemaphoreGive(s_state_mutex);
        return rc;
    } else if (ble_uuid_cmp(ctxt->chr->uuid, &CHR_COMMAND_UUID.u) == 0) {
        if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len == 0 || len >= MAX_CMD_JSON_LEN) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        char buf[MAX_CMD_JSON_LEN];
        int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf) - 1, &len);
        if (rc != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        buf[len] = '\0';
        handle_command_json(buf, len);
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static void start_advertising(void);

static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "connection %s; status=%d",
                 event->connect.status == 0 ? "established" : "failed",
                 event->connect.status);
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
        } else {
            start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnect; reason=%d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_telemetry_subscribed = false;
        s_status_subscribed = false;
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_telemetry_val_handle) {
            s_telemetry_subscribed = event->subscribe.cur_notify;
        } else if (event->subscribe.attr_handle == s_status_val_handle) {
            s_status_subscribed = event->subscribe.cur_notify;
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu update: conn=%d mtu=%d", event->mtu.conn_handle, event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* Forget the old bond so re-pairing after e.g. a factory reset works. */
        struct ble_gap_conn_desc desc;
        ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    default:
        return 0;
    }
}

static void start_advertising(void)
{
    struct ble_gap_adv_params adv_params = {0};
    struct ble_hs_adv_fields fields = {0};

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: %d", rc);
        return;
    }

    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params,
                            gap_event_handler, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
    }
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr failed: %d", rc);
        return;
    }

    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: %d", rc);
        return;
    }

    start_advertising();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "nimble host reset; reason=%d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_service_init(QueueHandle_t command_queue)
{
    s_command_queue = command_queue;
    s_state_mutex = xSemaphoreCreateMutex();
    snprintf(s_telemetry_json, sizeof(s_telemetry_json), "{}");
    s_telemetry_json_len = strlen(s_telemetry_json);
    snprintf(s_status_json, sizeof(s_status_json), "{}");
    s_status_json_len = strlen(s_status_json);

    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.gatts_register_cb = NULL;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* Encrypted, bonded link. "Just works" pairing (no MITM protection) since
     * neither the ESP32 nor a headless Raspberry Pi has a display/keyboard for
     * passkey entry. Bonds are persisted in flash via ble_store_config. */
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_att_set_preferred_mtu(247);

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(gatt_svr_svcs);
    ESP_ERROR_CHECK(rc == 0 ? ESP_OK : ESP_FAIL);
    rc = ble_gatts_add_svcs(gatt_svr_svcs);
    ESP_ERROR_CHECK(rc == 0 ? ESP_OK : ESP_FAIL);

    ble_svc_gap_device_name_set("templog");

    ble_store_config_init();

    nimble_port_freertos_init(host_task);
}

void ble_service_update_telemetry(const ble_telemetry_t *telemetry)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *temps = cJSON_AddArrayToObject(root, "t");
    for (int i = 0; i < telemetry->num_readings; ++i) {
        const ble_temp_reading_t *r = &telemetry->readings[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", r->rom_code_hex);
        if (r->valid) {
            cJSON_AddNumberToObject(item, "c", r->value_c);
            cJSON_AddNumberToObject(item, "age", r->age_ms);
        }
        cJSON_AddBoolToObject(item, "water", r->is_water_sensor);
        cJSON_AddItemToArray(temps, item);
    }
    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddBoolToObject(wifi, "c", telemetry->wifi_connected);
    cJSON_AddStringToObject(wifi, "ssid", telemetry->wifi_ssid);
    cJSON *mqtt = cJSON_AddObjectToObject(root, "mqtt");
    cJSON_AddBoolToObject(mqtt, "c", telemetry->mqtt_connected);
    cJSON_AddStringToObject(mqtt, "url", telemetry->mqtt_url);
    cJSON_AddBoolToObject(root, "heater", telemetry->heater_on);
    cJSON_AddNumberToObject(root, "onC", telemetry->heater_on_threshold_c);
    cJSON_AddNumberToObject(root, "offC", telemetry->heater_off_threshold_c);
    cJSON_AddNumberToObject(root, "forceState", telemetry->heater_force_state);

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    char *out = cJSON_PrintUnformatted(root);
    if (out != NULL) {
        strlcpy(s_telemetry_json, out, sizeof(s_telemetry_json));
        s_telemetry_json_len = strlen(s_telemetry_json);
        cJSON_free(out);
    }
    xSemaphoreGive(s_state_mutex);
    cJSON_Delete(root);

    send_notification(s_telemetry_val_handle, s_telemetry_subscribed);
}

void ble_service_report_status(const char *cmd, bool ok, const char *error)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", cmd ? cmd : "");
    cJSON_AddBoolToObject(root, "ok", ok);
    if (error != NULL) {
        cJSON_AddStringToObject(root, "error", error);
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    char *out = cJSON_PrintUnformatted(root);
    if (out != NULL) {
        strlcpy(s_status_json, out, sizeof(s_status_json));
        s_status_json_len = strlen(s_status_json);
        cJSON_free(out);
    }
    xSemaphoreGive(s_state_mutex);
    cJSON_Delete(root);

    send_notification(s_status_val_handle, s_status_subscribed);
}
