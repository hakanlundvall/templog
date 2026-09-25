#include "ble_service.h"

#include <math.h>
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
static const ble_uuid128_t CHR_FIRMWARE_UUID =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                      0x93, 0xf3, 0xa3, 0xb5, 0x04, 0x00, 0x40, 0x6e);
static const ble_uuid128_t CHR_SENSORS_UUID =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                      0x93, 0xf3, 0xa3, 0xb5, 0x05, 0x00, 0x40, 0x6e);
static const ble_uuid128_t CHR_CONFIG_UUID =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                      0x93, 0xf3, 0xa3, 0xb5, 0x06, 0x00, 0x40, 0x6e);

/* What the device reports is split across three characteristics because a
 * GATT client will not read more than 512 bytes of one attribute value:
 * Android's stack truncates there, silently. Sensors are the part that grows,
 * at around 58 bytes each, so eight of them get a characteristic to
 * themselves; the live state and the settings each stay comfortably inside
 * one read. The command buffer is small on purpose: it lives on the NimBLE
 * host task's stack. */
#define MAX_SENSORS_JSON_LEN 512
#define MAX_TELEMETRY_JSON_LEN 512
#define MAX_CONFIG_JSON_LEN 512
#define MAX_STATUS_JSON_LEN 256
#define MAX_CMD_JSON_LEN 512
/* One firmware chunk: a 4 byte offset plus as much as an ATT write can carry
 * at the preferred MTU. */
#define FIRMWARE_CHUNK_HEADER_LEN 4
#define MAX_FIRMWARE_CHUNK_LEN 256

static QueueHandle_t s_command_queue;
static ble_firmware_chunk_cb s_firmware_chunk_cb;
static SemaphoreHandle_t s_state_mutex;

static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_telemetry_val_handle;
static uint16_t s_sensors_val_handle;
static uint16_t s_config_val_handle;
static uint16_t s_status_val_handle;
static bool s_telemetry_subscribed;
static bool s_sensors_subscribed;
static bool s_config_subscribed;
static bool s_status_subscribed;

static char s_telemetry_json[MAX_TELEMETRY_JSON_LEN];
static size_t s_telemetry_json_len;
static char s_sensors_json[MAX_SENSORS_JSON_LEN];
static size_t s_sensors_json_len;
static char s_config_json[MAX_CONFIG_JSON_LEN];
static size_t s_config_json_len;
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
                /* The readings, which are what grows with the number of
                 * sensors, hence their own characteristic. */
                .uuid = &CHR_SENSORS_UUID.u,
                .access_cb = gatt_svr_chr_access,
                .val_handle = &s_sensors_val_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                /* Settings, which only change when a command changes them, so
                 * this one rarely notifies. */
                .uuid = &CHR_CONFIG_UUID.u,
                .access_cb = gatt_svr_chr_access,
                .val_handle = &s_config_val_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = &CHR_STATUS_UUID.u,
                .access_cb = gatt_svr_chr_access,
                .val_handle = &s_status_val_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                /* Firmware bytes. Write-without-response keeps the transfer
                 * reasonably fast; ordering is checked by the offset each
                 * chunk carries. */
                .uuid = &CHR_FIRMWARE_UUID.u,
                .access_cb = gatt_svr_chr_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC,
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

/* A number that may be left out, in a command where every field is optional.
 * NAN and 0 are the "leave unchanged" markers the firmware looks for. */
static float opt_float(const cJSON *root, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(item) ? (float)item->valuedouble : NAN;
}

static uint16_t opt_u16(const cJSON *root, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(item) || item->valuedouble < 1 || item->valuedouble > UINT16_MAX) {
        return 0;
    }
    return (uint16_t)item->valuedouble;
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
    } else if (strcmp(cmd, "set_sensor_role") == 0 || strcmp(cmd, "set_water_sensor") == 0) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
        const cJSON *role = cJSON_GetObjectItemCaseSensitive(root, "role");
        /* set_water_sensor is what the first version of the protocol called
         * this, and older clients still send it. */
        const char *role_s = cJSON_IsString(role) ? role->valuestring : "water";
        if (!cJSON_IsString(id)) {
            ok = false;
            error = "id required";
        } else if (strlen(role_s) >= sizeof(out.data.sensor_role.role)) {
            ok = false;
            error = "unknown role";
        } else {
            out.type = BLE_CMD_SET_SENSOR_ROLE;
            strlcpy(out.data.sensor_role.cmd, cmd, sizeof(out.data.sensor_role.cmd));
            strlcpy(out.data.sensor_role.role, role_s, sizeof(out.data.sensor_role.role));
            strlcpy(out.data.sensor_role.rom_code_hex, id->valuestring,
                    sizeof(out.data.sensor_role.rom_code_hex));
        }
    } else if (strcmp(cmd, "set_shunt") == 0) {
        const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
        const cJSON *topic = cJSON_GetObjectItemCaseSensitive(root, "indoorTopic");
        if (topic != NULL && !cJSON_IsString(topic)) {
            ok = false;
            error = "indoorTopic must be a string";
        } else if (cJSON_IsString(topic) && strlen(topic->valuestring) >= BLE_MQTT_TOPIC_LEN) {
            ok = false;
            error = "indoorTopic too long";
        } else {
            out.type = BLE_CMD_SET_SHUNT;
            out.data.shunt.enabled = cJSON_IsBool(enabled) ? (cJSON_IsTrue(enabled) ? 1 : 0) : -1;
            out.data.shunt.slope = opt_float(root, "slope");
            out.data.shunt.offset_c = opt_float(root, "offset");
            out.data.shunt.room_target_c = opt_float(root, "target");
            out.data.shunt.min_supply_c = opt_float(root, "min");
            out.data.shunt.max_supply_c = opt_float(root, "max");
            out.data.shunt.authority_c = opt_float(root, "authority");
            out.data.shunt.indoor_gain = opt_float(root, "indoorGain");
            out.data.shunt.indoor_max_c = opt_float(root, "indoorMax");
            out.data.shunt.travel_s = opt_u16(root, "travel");
            out.data.shunt.indoor_stale_s = opt_u16(root, "indoorStale");
            if (cJSON_IsString(topic)) {
                out.data.shunt.set_indoor_topic = true;
                strlcpy(out.data.shunt.indoor_topic, topic->valuestring,
                        sizeof(out.data.shunt.indoor_topic));
            }
        }
    } else if (strcmp(cmd, "shunt_jog") == 0) {
        const cJSON *dir = cJSON_GetObjectItemCaseSensitive(root, "dir");
        const cJSON *ms = cJSON_GetObjectItemCaseSensitive(root, "ms");
        if (!cJSON_IsString(dir) || strlen(dir->valuestring) >= sizeof(out.data.jog.dir)) {
            ok = false;
            error = "dir must be warmer or colder";
        } else if (!cJSON_IsNumber(ms) || ms->valuedouble < 1 || ms->valuedouble > (double)UINT32_MAX) {
            ok = false;
            error = "ms required";
        } else {
            out.type = BLE_CMD_SHUNT_JOG;
            strlcpy(out.data.jog.dir, dir->valuestring, sizeof(out.data.jog.dir));
            out.data.jog.ms = (uint32_t)ms->valuedouble;
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
    } else if (strcmp(cmd, "ota_ble_begin") == 0) {
        const cJSON *size = cJSON_GetObjectItemCaseSensitive(root, "size");
        const cJSON *crc = cJSON_GetObjectItemCaseSensitive(root, "crc32");
        const cJSON *ver = cJSON_GetObjectItemCaseSensitive(root, "ver");
        const cJSON *force = cJSON_GetObjectItemCaseSensitive(root, "force");
        if (!cJSON_IsNumber(size) || !cJSON_IsNumber(crc)) {
            ok = false;
            error = "size/crc32 required";
        } else if (size->valuedouble <= 0 || size->valuedouble > (double)UINT32_MAX) {
            ok = false;
            error = "invalid size";
        } else {
            out.type = BLE_CMD_OTA_BLE_BEGIN;
            out.data.ota_ble.size = (uint32_t)size->valuedouble;
            out.data.ota_ble.crc32 = (uint32_t)crc->valuedouble;
            out.data.ota_ble.force = cJSON_IsTrue(force);
            if (cJSON_IsString(ver)) {
                strlcpy(out.data.ota_ble.version, ver->valuestring, sizeof(out.data.ota_ble.version));
            }
        }
    } else if (strcmp(cmd, "ota_ble_end") == 0) {
        out.type = BLE_CMD_OTA_BLE_END;
    } else if (strcmp(cmd, "ota_ble_abort") == 0) {
        out.type = BLE_CMD_OTA_BLE_ABORT;
    } else if (strcmp(cmd, "ota_update") == 0) {
        const cJSON *tag = cJSON_GetObjectItemCaseSensitive(root, "tag");
        const cJSON *force = cJSON_GetObjectItemCaseSensitive(root, "force");
        const char *tag_s = cJSON_IsString(tag) ? tag->valuestring : "latest";
        if (strlen(tag_s) >= BLE_OTA_TAG_LEN) {
            ok = false;
            error = "tag too long";
        } else {
            out.type = BLE_CMD_OTA_UPDATE;
            strlcpy(out.data.ota.tag, tag_s, sizeof(out.data.ota.tag));
            out.data.ota.force = cJSON_IsTrue(force);
        }
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
    } else if (ble_uuid_cmp(ctxt->chr->uuid, &CHR_SENSORS_UUID.u) == 0) {
        if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        int rc = read_flat_value(ctxt, s_sensors_json, s_sensors_json_len);
        xSemaphoreGive(s_state_mutex);
        return rc;
    } else if (ble_uuid_cmp(ctxt->chr->uuid, &CHR_CONFIG_UUID.u) == 0) {
        if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        int rc = read_flat_value(ctxt, s_config_json, s_config_json_len);
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
    } else if (ble_uuid_cmp(ctxt->chr->uuid, &CHR_FIRMWARE_UUID.u) == 0) {
        if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len <= FIRMWARE_CHUNK_HEADER_LEN || len > MAX_FIRMWARE_CHUNK_LEN) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        uint8_t buf[MAX_FIRMWARE_CHUNK_LEN];
        int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len);
        if (rc != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        if (s_firmware_chunk_cb == NULL) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        uint32_t offset = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                          ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
        /* A rejected chunk ends the transfer; the reason reaches the client
         * through telemetry, since a write without response has no reply. */
        s_firmware_chunk_cb(offset, buf + FIRMWARE_CHUNK_HEADER_LEN,
                            len - FIRMWARE_CHUNK_HEADER_LEN);
        return 0;
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
        s_sensors_subscribed = false;
        s_config_subscribed = false;
        s_status_subscribed = false;
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_telemetry_val_handle) {
            s_telemetry_subscribed = event->subscribe.cur_notify;
        } else if (event->subscribe.attr_handle == s_sensors_val_handle) {
            s_sensors_subscribed = event->subscribe.cur_notify;
        } else if (event->subscribe.attr_handle == s_config_val_handle) {
            s_config_subscribed = event->subscribe.cur_notify;
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

void ble_service_init(QueueHandle_t command_queue, ble_firmware_chunk_cb on_firmware_chunk)
{
    s_command_queue = command_queue;
    s_firmware_chunk_cb = on_firmware_chunk;
    s_state_mutex = xSemaphoreCreateMutex();
    snprintf(s_telemetry_json, sizeof(s_telemetry_json), "{}");
    s_telemetry_json_len = strlen(s_telemetry_json);
    snprintf(s_sensors_json, sizeof(s_sensors_json), "{}");
    s_sensors_json_len = strlen(s_sensors_json);
    snprintf(s_config_json, sizeof(s_config_json), "{}");
    s_config_json_len = strlen(s_config_json);
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

/* cJSON prints a number with as many digits as it takes to read it back
 * exactly, and a float widened to a double needs seventeen of them: 62.3f
 * would go out as "62.299999237060547". Rounding in double arithmetic first
 * gives back the short form, which is what keeps these documents inside one
 * GATT read. */
static void add_rounded(cJSON *object, const char *key, float value, int decimals)
{
    double scale = decimals == 1 ? 10.0 : 100.0;
    cJSON_AddNumberToObject(object, key, round((double)value * scale) / scale);
}

/* Renders `root` and, if the result differs from what the characteristic
 * already holds, stores it and notifies. Returns nothing: a document too
 * large to fit is dropped with a warning rather than sent truncated, since a
 * half document would fail to parse on the client anyway. */
static void publish_document(cJSON *root, char *buffer, size_t buffer_len,
                              size_t *stored_len, uint16_t val_handle, bool subscribed,
                              bool notify_only_on_change, const char *what)
{
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (out == NULL) {
        return;
    }

    size_t len = strlen(out);
    if (len >= buffer_len) {
        ESP_LOGW(TAG, "%s document is %u bytes, too large to publish", what, (unsigned)len);
        cJSON_free(out);
        return;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool changed = strcmp(buffer, out) != 0;
    if (changed) {
        memcpy(buffer, out, len + 1);
        *stored_len = len;
    }
    xSemaphoreGive(s_state_mutex);
    cJSON_free(out);

    if (changed || !notify_only_on_change) {
        send_notification(val_handle, subscribed);
    }
}

void ble_service_update_telemetry(const ble_telemetry_t *telemetry)
{
    /* The readings, which are what grows with the number of sensors. */
    cJSON *sensors = cJSON_CreateObject();
    cJSON *temps = cJSON_AddArrayToObject(sensors, "t");
    for (int i = 0; i < telemetry->num_readings; ++i) {
        const ble_temp_reading_t *r = &telemetry->readings[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", r->rom_code_hex);
        if (r->valid) {
            add_rounded(item, "c", r->value_c, 1);
            cJSON_AddNumberToObject(item, "age", r->age_ms);
        }
        if (r->role != NULL) {
            cJSON_AddStringToObject(item, "role", r->role);
        }
        cJSON_AddItemToArray(temps, item);
    }
    publish_document(sensors, s_sensors_json, sizeof(s_sensors_json), &s_sensors_json_len,
                     s_sensors_val_handle, s_sensors_subscribed, false, "sensors");

    /* Live state: everything that moves on its own. */
    cJSON *root = cJSON_CreateObject();
    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddBoolToObject(wifi, "c", telemetry->wifi_connected);
    if (telemetry->wifi_rssi_valid) {
        cJSON_AddNumberToObject(wifi, "rssi", telemetry->wifi_rssi);
    }
    cJSON_AddNumberToObject(wifi, "disc", telemetry->wifi_disconnect_count);
    if (telemetry->wifi_disconnect_count > 0) {
        cJSON_AddNumberToObject(wifi, "reason", telemetry->wifi_last_disc_reason);
        cJSON_AddNumberToObject(wifi, "discRssi", telemetry->wifi_last_disc_rssi);
        cJSON_AddNumberToObject(wifi, "discAge", telemetry->wifi_last_disc_age_ms);
    }
    cJSON *mqtt = cJSON_AddObjectToObject(root, "mqtt");
    cJSON_AddBoolToObject(mqtt, "c", telemetry->mqtt_connected);
    cJSON_AddBoolToObject(root, "heater", telemetry->heater_on);
    cJSON_AddNumberToObject(root, "forceState", telemetry->heater_force_state);

    cJSON *shunt = cJSON_AddObjectToObject(root, "shunt");
    cJSON_AddBoolToObject(shunt, "en", telemetry->shunt_enabled);
    cJSON_AddStringToObject(shunt, "state", telemetry->shunt_state ? telemetry->shunt_state : "off");
    cJSON_AddStringToObject(shunt, "dir", telemetry->shunt_dir ? telemetry->shunt_dir : "idle");
    if (telemetry->shunt_reason != NULL) {
        cJSON_AddStringToObject(shunt, "why", telemetry->shunt_reason);
    }
    /* A reading that has never arrived is left out rather than sent as null,
     * so a client can tell "not known" from "zero degrees". */
    if (!isnan(telemetry->shunt_setpoint_c)) {
        add_rounded(shunt, "sp", telemetry->shunt_setpoint_c, 1);
    }
    if (!isnan(telemetry->shunt_supply_c)) {
        add_rounded(shunt, "sup", telemetry->shunt_supply_c, 1);
    }
    if (!isnan(telemetry->shunt_outdoor_c)) {
        add_rounded(shunt, "out", telemetry->shunt_outdoor_c, 1);
    }
    add_rounded(shunt, "pos", telemetry->shunt_position, 2);
    /* The indoor reading lives here rather than in an object of its own: the
     * clients merge the three documents, and the settings document already
     * has an "indoor" object. */
    if (!isnan(telemetry->indoor_c)) {
        add_rounded(shunt, "in", telemetry->indoor_c, 1);
        cJSON_AddNumberToObject(shunt, "inAge", telemetry->indoor_age_ms);
    }
    cJSON_AddBoolToObject(shunt, "inFresh", telemetry->indoor_fresh);
    add_rounded(shunt, "trim", telemetry->indoor_trim_c, 1);

    cJSON *ota = cJSON_AddObjectToObject(root, "ota");
    cJSON_AddStringToObject(ota, "state", telemetry->ota_state ? telemetry->ota_state : "idle");
    if (telemetry->ota_percent >= 0) {
        cJSON_AddNumberToObject(ota, "pct", telemetry->ota_percent);
    }
    if (telemetry->ota_version[0] != '\0') {
        cJSON_AddStringToObject(ota, "ver", telemetry->ota_version);
    }
    if (telemetry->ota_error != NULL) {
        cJSON_AddStringToObject(ota, "err", telemetry->ota_error);
    }
    publish_document(root, s_telemetry_json, sizeof(s_telemetry_json), &s_telemetry_json_len,
                     s_telemetry_val_handle, s_telemetry_subscribed, false, "telemetry");

    /* Settings, which only move when a command moves them: this one is
     * notified only when it actually changed. */
    cJSON *config = cJSON_CreateObject();
    cJSON_AddStringToObject(config, "ssid", telemetry->wifi_ssid);
    cJSON_AddStringToObject(config, "url", telemetry->mqtt_url);
    add_rounded(config, "onC", telemetry->heater_on_threshold_c, 1);
    add_rounded(config, "offC", telemetry->heater_off_threshold_c, 1);
    cJSON *curve = cJSON_AddObjectToObject(config, "curve");
    add_rounded(curve, "slope", telemetry->curve_slope, 2);
    add_rounded(curve, "offset", telemetry->curve_offset_c, 1);
    add_rounded(curve, "target", telemetry->curve_target_c, 1);
    add_rounded(curve, "min", telemetry->curve_min_supply_c, 1);
    add_rounded(curve, "max", telemetry->curve_max_supply_c, 1);
    cJSON_AddNumberToObject(curve, "travel", telemetry->actuator_travel_s);
    add_rounded(curve, "authority", telemetry->actuator_authority_c, 1);
    cJSON *indoor_cfg = cJSON_AddObjectToObject(config, "indoor");
    cJSON_AddStringToObject(indoor_cfg, "topic", telemetry->indoor_topic);
    add_rounded(indoor_cfg, "gain", telemetry->indoor_gain, 2);
    add_rounded(indoor_cfg, "maxTrim", telemetry->indoor_max_c, 1);
    cJSON_AddNumberToObject(indoor_cfg, "stale", telemetry->indoor_stale_s);
    cJSON_AddStringToObject(config, "fw", telemetry->fw_version);
    publish_document(config, s_config_json, sizeof(s_config_json), &s_config_json_len,
                     s_config_val_handle, s_config_subscribed, true, "config");
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
