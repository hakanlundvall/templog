#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"

#include "owb.h"
#include "owb_rmt.h"
#include "ds18b20.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "lwip/err.h"
#include "lwip/sys.h"

#include "mqtt_client.h"
#include "esp_crt_bundle.h"
#include "freertos/queue.h"
#include <math.h>
#include <stdlib.h>
#include <strings.h>

#include "ble_service.h"
#include "ota.h"
#include "shunt.h"

uint8_t ssid[32];
uint8_t password[64];
char url[100];
#define MAXIMUM_RETRY 3

#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_BOTH
#define H2E_IDENTIFIER ""

static EventGroupHandle_t s_wifi_event_group;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static const char *TAG = "temp-logger";

static int s_retry_num = 0;
static volatile bool s_wifi_connected = false;
static volatile bool s_mqtt_connected = false;
/* Written by the Wi-Fi event handler, read when building telemetry, so the
 * cause of a dropped link can be seen over BLE without a serial console. */
static volatile uint32_t s_wifi_disconnect_count = 0;
static volatile uint8_t s_wifi_last_disc_reason = 0;
static volatile int8_t s_wifi_last_disc_rssi = 0;
static volatile TickType_t s_wifi_last_disc_tick = 0;
static esp_mqtt_client_handle_t g_mqtt_client = NULL;
/* Set by the MQTT event handler on every (re)connect so the sampling loop
 * republishes the heater state, even if it has not changed meanwhile. */
static volatile bool s_heater_state_dirty = true;

typedef enum {
    HEATER_FORCE_NONE = 0,
    HEATER_FORCE_OFF_UNTIL_CONDITIONS,
    HEATER_FORCE_OFF_UNTIL_STARTED,
    /* One heating cycle and no more: the heater runs without regard to the on
     * threshold until the water reaches the off threshold, and then the mode
     * becomes HEATER_FORCE_OFF_UNTIL_STARTED, so nothing restarts it but an
     * explicit command. */
    HEATER_RUN_ONCE,
} heater_force_state_t;

/* What each DS18B20 is used for. The water sensor drives the heater
 * thermostat; the outdoor and supply sensors feed the shunt controller, which
 * cannot run without both of them. A sensor holds at most one role. */
typedef enum {
    SENSOR_ROLE_NONE = -1,
    SENSOR_ROLE_WATER = 0,
    SENSOR_ROLE_OUTDOOR,
    SENSOR_ROLE_SUPPLY,
    SENSOR_ROLE_COUNT,
} sensor_role_t;

static const char *const role_names[SENSOR_ROLE_COUNT] = {"water", "outdoor", "supply"};
/* NVS keys, in the same order. "waterRom" predates the other two. */
static const char *const role_keys[SENSOR_ROLE_COUNT] = {"waterRom", "outRom", "supRom"};

/* The ROM code assigned to each role, empty when the role is unassigned, and
 * the index it resolves to in device_rom_codes - which stays -1 while the
 * sensor is not on the bus, so a role can be set before the sensor is wired. */
static char role_rom[SENSOR_ROLE_COUNT][BLE_ROM_CODE_HEX_LEN + 1];
static int role_index[SENSOR_ROLE_COUNT] = {-1, -1, -1};

static nvs_handle_t g_nvs_handle;
static QueueHandle_t g_ble_cmd_queue;
static OneWireBus_ROMCode device_rom_codes[8];
static int num_devices = 0;
static bool heater_on = false;
/* The water temperature the thermostat judges: the same filtered value that is
 * published on temp/<rom code>, not the single sample it came from. One
 * DS18B20 reading wobbles by a couple of tenths, which is enough to trip a
 * threshold a sample early or late, and at the top of a cycle that decides
 * whether the burner runs for another minute. NAN until the sensor in the
 * water role has been read. */
static float last_water_temp = NAN;
static heater_force_state_t heater_force_state = HEATER_FORCE_NONE;
static float heater_on_threshold = 0;
static float heater_off_threshold = 0;

#define CONNECTION_RETRY_PERIOD_MS 60000

/* How long to wait for the first Wi-Fi association before carrying on with the
 * boot. Temperature logging and heater control must not depend on the network,
 * and neither must the BLE command loop that lets the device be reconfigured. */
#define WIFI_FIRST_CONNECT_TIMEOUT_MS 30000

/* A device that has never been provisioned has no credentials in NVS. Rather
 * than refusing to boot, it comes up with BLE running so that set_wifi and
 * set_mqtt can supply them. */
static bool has_wifi_config(void)
{
    return ssid[0] != '\0';
}

static bool has_mqtt_config(void)
{
    return url[0] != '\0';
}

/* The indoor temperature is not measured by this device. Something else on the
 * network publishes it, so the shunt controller gets it by subscribing to this
 * topic; empty means the indoor trim is switched off. */
static char indoor_topic[BLE_MQTT_TOPIC_LEN];

static bool has_indoor_topic(void)
{
    return indoor_topic[0] != '\0';
}

static sensor_role_t role_from_name(const char *name)
{
    for (int i = 0; i < SENSOR_ROLE_COUNT; ++i)
    {
        if (strcasecmp(name, role_names[i]) == 0)
        {
            return (sensor_role_t)i;
        }
    }
    return SENSOR_ROLE_NONE;
}

/* The role a sensor on the bus currently holds, for telemetry. */
static sensor_role_t role_of_sensor(int index)
{
    for (int i = 0; i < SENSOR_ROLE_COUNT; ++i)
    {
        if (role_index[i] == index)
        {
            return (sensor_role_t)i;
        }
    }
    return SENSOR_ROLE_NONE;
}

static int find_sensor_index(const char *rom_code_hex)
{
    for (int i = 0; i < num_devices; ++i)
    {
        char rom_code_s[BLE_ROM_CODE_HEX_LEN + 1];
        owb_string_from_rom_code(device_rom_codes[i], rom_code_s, sizeof(rom_code_s));
        if (strcasecmp(rom_code_s, rom_code_hex) == 0)
        {
            return i;
        }
    }
    return -1;
}

/* Matches the stored ROM codes against the sensors actually found on the bus.
 * Called after the 1-Wire search and whenever an assignment changes. */
static void resolve_role_indexes(void)
{
    for (int i = 0; i < SENSOR_ROLE_COUNT; ++i)
    {
        role_index[i] = role_rom[i][0] != '\0' ? find_sensor_index(role_rom[i]) : -1;
    }
}

static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0)
    {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        /* Connecting with an empty SSID fails synchronously and raises no
         * disconnect event, which would leave the boot wait hanging. */
        if (has_wifi_config())
        {
            esp_wifi_connect();
        }
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        s_wifi_connected = false;
        s_wifi_last_disc_reason = event->reason;
        s_wifi_last_disc_rssi = event->rssi;
        s_wifi_last_disc_tick = xTaskGetTickCount();
        s_wifi_disconnect_count++;
        ESP_LOGW(TAG, "Wi-Fi disconnected, reason %d, rssi %d", event->reason, event->rssi);
        if (s_retry_num < MAXIMUM_RETRY)
        {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "connect to the AP fail");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_connected = true;
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            // .ssid = ssid,
            // .password = password,
            .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
            .sae_pwe_h2e = ESP_WIFI_SAE_MODE,
            .sae_h2e_identifier = H2E_IDENTIFIER,
        },
    };
    memcpy(wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    memcpy(wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    if (!has_wifi_config())
    {
        ESP_LOGW(TAG, "No Wi-Fi credentials stored; waiting for a set_wifi command over BLE");
        return;
    }

    /* Bounded, so that an AP that never answers cannot hold up the rest of the
     * boot; the connection monitor keeps retrying in the background either way. */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(WIFI_FIRST_CONNECT_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "connected to ap SSID:%s", ssid);
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGW(TAG, "Failed to connect to SSID:%s", ssid);
    }
    else
    {
        ESP_LOGW(TAG, "Timed out connecting to SSID:%s; continuing boot", ssid);
    }
}

#define HEATER_STATE_TOPIC "temp/1/heater"
#define HEATER_MODE_TOPIC "temp/1/heater/mode"
/* Where a mode is asked for, as against HEATER_MODE_TOPIC where it is
 * reported. The payload is one of the names below, so whatever is read off the
 * state topic can be written straight back to this one. */
#define HEATER_MODE_SET_TOPIC "temp/1/heater/mode/set"
/* The thresholds the thermostat switches on. Reported only; they are changed
 * over BLE, where they can be checked against each other. */
#define HEATER_ON_TOPIC "temp/1/heater/on_c"
#define HEATER_OFF_TOPIC "temp/1/heater/off_c"

/* How often the state topics are republished even though nothing changed.
 *
 * Publishing only on a change assumes the broker keeps the retained value and
 * hands it to whoever subscribes later. Not every broker does: the Zenoh
 * router's MQTT plugin, which this installation uses, delivers no retained
 * messages at all, so a subscriber that arrives between two changes sees
 * nothing and a topic that rarely changes - the mode, the thresholds - reads as
 * unknown indefinitely. Repeating the current state on a slow timer costs a
 * handful of small messages a minute and makes any subscriber correct within
 * that minute, whatever the broker does about retention. */
#define MQTT_REPUBLISH_PERIOD_MS 60000

static const char *heater_mode_name(heater_force_state_t state)
{
    switch (state)
    {
    case HEATER_FORCE_OFF_UNTIL_CONDITIONS:
        return "off_until_conditions";
    case HEATER_FORCE_OFF_UNTIL_STARTED:
        return "off_until_started";
    case HEATER_RUN_ONCE:
        return "heat_once";
    case HEATER_FORCE_NONE:
    default:
        return "auto";
    }
}

/* The inverse, for the command topic. False when the payload names no mode, in
 * which case nothing happens: a typo must not move the heater. */
static bool heater_mode_from_name(const char *name, heater_force_state_t *out)
{
    static const heater_force_state_t modes[] = {
        HEATER_FORCE_NONE,
        HEATER_FORCE_OFF_UNTIL_CONDITIONS,
        HEATER_FORCE_OFF_UNTIL_STARTED,
        HEATER_RUN_ONCE,
    };
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i)
    {
        if (strcmp(name, heater_mode_name(modes[i])) == 0)
        {
            *out = modes[i];
            return true;
        }
    }
    return false;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32 "", base, event_id);
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        s_mqtt_connected = true;
        s_heater_state_dirty = true;
        // esp_mqtt_client_publish(event->client, "temp/1/status", "connected", 0, 1, 0);

        /* Subscriptions do not survive a reconnect, so they are taken again
         * every time the session comes back. */
        if (has_indoor_topic())
        {
            esp_mqtt_client_subscribe(event->client, indoor_topic, 0);
            ESP_LOGI(TAG, "Subscribed to indoor temperature topic %s", indoor_topic);
        }
        esp_mqtt_client_subscribe(event->client, HEATER_MODE_SET_TOPIC, 1);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        s_mqtt_connected = false;
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        /* A mode asked for on the command topic. It is applied through the
         * command queue rather than here, because the heater output and its
         * mode belong to the application task; this one only decodes.
         *
         * A retained message is ignored on purpose. The broker would replay it
         * on every reconnect, which for heat_once would silently start a fresh
         * cycle each time the link bounced. */
        if ((size_t)event->topic_len == strlen(HEATER_MODE_SET_TOPIC) &&
            strncmp(event->topic, HEATER_MODE_SET_TOPIC, event->topic_len) == 0)
        {
            char payload[32];
            heater_force_state_t mode;
            if (event->retain)
            {
                ESP_LOGW(TAG, "Ignoring retained heater mode command; publish it without the retain flag");
            }
            else if ((size_t)event->data_len >= sizeof(payload))
            {
                ESP_LOGW(TAG, "Ignoring oversized heater mode command");
            }
            else
            {
                memcpy(payload, event->data, event->data_len);
                payload[event->data_len] = '\0';
                if (!heater_mode_from_name(payload, &mode))
                {
                    ESP_LOGW(TAG, "Ignoring unknown heater mode '%s'", payload);
                }
                else
                {
                    ble_command_t cmd = {
                        .type = BLE_CMD_SET_HEATER_MODE,
                        .data.heater_mode.mode = (int)mode,
                    };
                    if (g_ble_cmd_queue == NULL ||
                        xQueueSend(g_ble_cmd_queue, &cmd, 0) != pdTRUE)
                    {
                        ESP_LOGW(TAG, "Dropped heater mode command '%s': queue full", payload);
                    }
                }
            }
            break;
        }
        /* The indoor temperature, if this is it. A payload is a bare number;
         * anything else is dropped, so the reading simply goes stale and the
         * curve carries the heating on its own. */
        if (has_indoor_topic() &&
            (size_t)event->topic_len == strlen(indoor_topic) &&
            strncmp(event->topic, indoor_topic, event->topic_len) == 0)
        {
            char payload[32];
            if ((size_t)event->data_len < sizeof(payload))
            {
                memcpy(payload, event->data, event->data_len);
                payload[event->data_len] = '\0';
                char *end = NULL;
                float value = strtof(payload, &end);
                if (end != payload && isfinite(value))
                {
                    shunt_report_indoor(value);
                }
                else
                {
                    ESP_LOGW(TAG, "Ignoring unparsable indoor temperature '%s'", payload);
                }
            }
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
        {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno", event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

static char mqtt_status_topic[] = "temp/1/status";

/* The scheme of the broker URL picks the transport, and with it the port to
 * use when the URL does not spell one out. */
typedef struct
{
    const char *prefix;
    uint32_t default_port;
} mqtt_scheme_t;

static const mqtt_scheme_t mqtt_schemes[] = {
    {"mqtt://", 1883},
    {"mqtts://", 8883},
    {"ws://", 80},
    {"wss://", 443},
};

static const mqtt_scheme_t *mqtt_scheme_of(const char *broker_url)
{
    for (size_t i = 0; i < sizeof(mqtt_schemes) / sizeof(mqtt_schemes[0]); ++i)
    {
        if (strncasecmp(broker_url, mqtt_schemes[i].prefix, strlen(mqtt_schemes[i].prefix)) == 0)
        {
            return &mqtt_schemes[i];
        }
    }
    return NULL;
}

/* Whether the authority carries an explicit ":port". Userinfo is skipped first
 * so the colon in "user:pass@host" does not count as one, and the search stops
 * where the path begins so a colon there does not either. */
static bool mqtt_url_has_port(const char *broker_url, const mqtt_scheme_t *scheme)
{
    const char *authority = broker_url + strlen(scheme->prefix);
    const char *path = strpbrk(authority, "/?#");
    const char *at = strchr(authority, '@');
    if (at != NULL && (path == NULL || at < path))
    {
        authority = at + 1;
    }
    const char *colon = strchr(authority, ':');
    return colon != NULL && (path == NULL || colon < path);
}

/* Builds the client configuration for whatever `url` currently holds. Rejecting
 * an unknown scheme here is worth the check: esp-mqtt would otherwise take the
 * URL and produce a client that never connects. */
static bool mqtt_make_config(esp_mqtt_client_config_t *cfg, const char **error)
{
    static char lwt_msg[] = "disconnected";

    const mqtt_scheme_t *scheme = mqtt_scheme_of(url);
    if (scheme == NULL)
    {
        ESP_LOGE(TAG, "Broker URL has no usable scheme: %s", url);
        *error = "url must start with mqtt://, mqtts://, ws:// or wss://";
        return false;
    }

    *cfg = (esp_mqtt_client_config_t){
        .broker.address.uri = url,
        /* mqtts:// and wss:// verify the broker against the certificate bundle
         * compiled into the firmware, the same one the OTA download uses. A
         * broker presenting a privately signed certificate is not supported. */
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .session.last_will.msg = lwt_msg,
        .session.last_will.msg_len = sizeof(lwt_msg) - 1,
        .session.last_will.qos = 1,
        .session.last_will.topic = mqtt_status_topic,
        /* Retained, so a subscriber that connects after the device dropped off
         * still learns it is gone, rather than waiting for a message that will
         * not come until the device is back. The birth message below is
         * retained for the same reason, and the pair is what lets Home
         * Assistant treat this topic as an availability topic. */
        .session.last_will.retain = 1,
        .session.keepalive = 9,
    };

    /* esp-mqtt only takes a port from the URL when one is present, and keeps
     * the port of the previously configured broker otherwise - so switching
     * from mqtt://host:1883 to mqtts://host would keep talking to 1883. */
    if (!mqtt_url_has_port(url, scheme))
    {
        cfg->broker.address.port = scheme->default_port;
        ESP_LOGI(TAG, "Broker URL has no port; using %lu for %s",
                 (unsigned long)scheme->default_port, scheme->prefix);
    }
    return true;
}

static esp_mqtt_client_handle_t mqtt_app_start(const char **error)
{
    static char connect_msg[] = "start";
    if (!has_mqtt_config())
    {
        ESP_LOGW(TAG, "No MQTT broker stored; waiting for a set_mqtt command over BLE");
        *error = "no broker configured";
        return NULL;
    }

    esp_mqtt_client_config_t mqtt_cfg;
    if (!mqtt_make_config(&mqtt_cfg, error))
    {
        return NULL;
    }

    ESP_LOGI(TAG, "Starting MQTT client URL: %s", url);
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    if (client == NULL)
    {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed for URL: %s", url);
        *error = "could not start the MQTT client";
        return NULL;
    }
    /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
    esp_mqtt_client_publish(client, mqtt_status_topic, connect_msg, sizeof(connect_msg) - 1, 1, 1);

    g_mqtt_client = client;
    return client;
}

/* Publishing is a no-op until a broker has been configured and the client
 * created. An unprovisioned device still reads sensors and drives the heater. */
static void mqtt_publish(const char *topic, const char *payload, int len)
{
    esp_mqtt_client_handle_t client = g_mqtt_client;
    if (client == NULL)
    {
        return;
    }
    int msg_id = esp_mqtt_client_publish(client, topic, payload, len, 1, 0);
    ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
}

/* Publishes the heater output, its mode and the two thresholds it switches on
 * as retained messages whenever any of them changes, and again after each
 * broker reconnect, so a subscriber such as Home Assistant always sees the
 * current state. The thresholds are here rather than with the readings because
 * they move only on command, and a subscriber that can see them can say what
 * the heater is aiming for and not just whether it is running.
 *
 * Nothing is sent while the broker is unreachable; the pending change goes out
 * once it is back. */
static void publish_heater_state(void)
{
    static bool last_on = false;
    static heater_force_state_t last_mode = HEATER_FORCE_NONE;
    /* Compared in tenths, which is the resolution they are published at, so a
     * float that is merely a rounding apart does not count as a change. */
    static int last_on_decic = INT32_MIN;
    static int last_off_decic = INT32_MIN;

    static TickType_t last_published;

    int on_decic = (int)lroundf(heater_on_threshold * 10.0f);
    int off_decic = (int)lroundf(heater_off_threshold * 10.0f);

    if (heater_on != last_on || heater_force_state != last_mode ||
        on_decic != last_on_decic || off_decic != last_off_decic ||
        (xTaskGetTickCount() - last_published) >= pdMS_TO_TICKS(MQTT_REPUBLISH_PERIOD_MS))
    {
        s_heater_state_dirty = true;
    }
    esp_mqtt_client_handle_t client = g_mqtt_client;
    if (!s_heater_state_dirty || client == NULL || !s_mqtt_connected)
    {
        return;
    }

    /* The device's own "I am here", on the same timer. The last will says
     * "disconnected" and is delivered as it happens, so the pair reads as
     * liveness even on a broker that keeps nothing. */
    esp_mqtt_client_publish(client, mqtt_status_topic, "start", 0, 1, 1);

    const char *state = heater_on ? "on" : "off";
    const char *mode = heater_mode_name(heater_force_state);
    char on_buf[10];
    char off_buf[10];
    int on_len = snprintf(on_buf, sizeof(on_buf), "%.1f", (double)heater_on_threshold);
    int off_len = snprintf(off_buf, sizeof(off_buf), "%.1f", (double)heater_off_threshold);
    if (on_len <= 0 || off_len <= 0)
    {
        return;
    }
    if (esp_mqtt_client_publish(client, HEATER_STATE_TOPIC, state, 0, 1, 1) < 0 ||
        esp_mqtt_client_publish(client, HEATER_MODE_TOPIC, mode, 0, 1, 1) < 0 ||
        esp_mqtt_client_publish(client, HEATER_ON_TOPIC, on_buf, on_len, 1, 1) < 0 ||
        esp_mqtt_client_publish(client, HEATER_OFF_TOPIC, off_buf, off_len, 1, 1) < 0)
    {
        ESP_LOGW(TAG, "Failed to publish heater state; will retry");
        return;
    }
    ESP_LOGI(TAG, "Published heater state=%s mode=%s on=%s off=%s", state, mode, on_buf, off_buf);
    last_on = heater_on;
    last_mode = heater_force_state;
    last_on_decic = on_decic;
    last_off_decic = off_decic;
    last_published = xTaskGetTickCount();
    s_heater_state_dirty = false;
}

#define SHUNT_STATE_TOPIC "temp/1/shunt/state"
#define SHUNT_SETPOINT_TOPIC "temp/1/shunt/setpoint"
#define SHUNT_BURSTS_TOPIC "temp/1/shunt/bursts"

/* Mirrors what the shunt controller is doing onto MQTT, retained, so Home
 * Assistant can graph the setpoint next to the measured supply temperature.
 * Like the heater state, changes are sent as they happen and the current values
 * are repeated every MQTT_REPUBLISH_PERIOD_MS regardless, so a subscriber does
 * not have to wait for the next change to learn where things stand. Nothing at
 * all is sent while the broker is unreachable. */
static void publish_shunt_state(void)
{
    static char last_state[16];
    static int last_setpoint_decic = INT32_MIN;
    static int last_bursts = 0;
    static TickType_t last_published;

    esp_mqtt_client_handle_t client = g_mqtt_client;
    if (client == NULL || !s_mqtt_connected)
    {
        return;
    }

    shunt_status_t status;
    shunt_get_status(&status);
    const char *state = shunt_state_name(status.state);
    int setpoint_decic = isnan(status.setpoint_c) ? INT32_MIN : (int)lroundf(status.setpoint_c * 10.0f);
    int bursts = status.bursts;
    bool force = (xTaskGetTickCount() - last_published) >= pdMS_TO_TICKS(MQTT_REPUBLISH_PERIOD_MS);

    if (force || strcmp(state, last_state) != 0)
    {
        if (esp_mqtt_client_publish(client, SHUNT_STATE_TOPIC, state, 0, 1, 1) < 0)
        {
            return;
        }
        strlcpy(last_state, state, sizeof(last_state));
    }
    if ((force || setpoint_decic != last_setpoint_decic) && setpoint_decic != INT32_MIN)
    {
        char buf[10];
        int len = snprintf(buf, sizeof(buf), "%.1f", (double)status.setpoint_c);
        if (len > 0 && esp_mqtt_client_publish(client, SHUNT_SETPOINT_TOPIC, buf, len, 1, 1) < 0)
        {
            return;
        }
        last_setpoint_decic = setpoint_decic;
    }
    /* How many corrections in a row it has needed, so a valve sitting against
     * an end stop, or a boiler that cannot keep up, shows up on a graph. */
    if (force || bursts != last_bursts)
    {
        char buf[10];
        int len = snprintf(buf, sizeof(buf), "%d", bursts);
        if (len > 0 && esp_mqtt_client_publish(client, SHUNT_BURSTS_TOPIC, buf, len, 1, 1) < 0)
        {
            return;
        }
        last_bursts = bursts;
    }
    /* Only once all three are out, so a failed publish is retried next tick
     * rather than waiting a whole period. */
    if (force)
    {
        last_published = xTaskGetTickCount();
    }
}

static void connection_monitor_task(void *arg)
{
    (void)arg;

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(CONNECTION_RETRY_PERIOD_MS));

        /* An unprovisioned device has nothing to retry; it is simply waiting
         * for credentials to arrive over BLE. */
        if (!has_wifi_config())
        {
            continue;
        }

        if (!s_wifi_connected)
        {
            ESP_LOGW(TAG, "Wi-Fi is disconnected; retrying connection");
            s_retry_num = 0;
            esp_wifi_connect();
            continue;
        }

        /* Read the handle each time round: the client may not have existed at
         * boot and been created later by a set_mqtt command. */
        esp_mqtt_client_handle_t client = g_mqtt_client;
        if (client != NULL && !s_mqtt_connected)
        {
            ESP_LOGW(TAG, "MQTT is disconnected; retrying connection");
            esp_err_t ret = esp_mqtt_client_reconnect(client);
            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "MQTT reconnect failed: %s", esp_err_to_name(ret));
            }
        }
    }
}

#define GPIO_DS18B20_0 (25)
#define GPIO_HEATER GPIO_NUM_27
#define MAX_DEVICES (8)
#define DS18B20_RESOLUTION (DS18B20_RESOLUTION_12_BIT)
#define SAMPLE_PERIOD (1000) // milliseconds
#define WATER_TEMP_IDENTIFICATION_THRESHOLD 80.0f
#define HEATER_OFF_THRESHOLD 80.0f
#define HEATER_ON_THRESHOLD 60.0f
#define TEMP_READING_TIMEOUT_MS 60000
/* How long a freshly installed firmware gets to reach Wi-Fi before it is
 * rolled back. Covers the bounded first connect at boot plus the connection
 * monitor's retry period. */
#define OTA_CONFIRM_TIMEOUT_MS 120000

/* Per-sensor history. At file scope because telemetry is published both from
 * the sampling loop and from the idle loop that runs when no sensor was
 * found at boot. */
static TickType_t last_good_reading[MAX_DEVICES];
static bool ever_good[MAX_DEVICES];
static float last_good_value[MAX_DEVICES];

bool read_value(nvs_handle_t handle, const char *key, uint8_t *value, size_t *length)
{
    printf("Reading  from NVS ... ");
    esp_err_t ret = nvs_get_str(handle, key, (char *)value, length);
    switch (ret)
    {
    case ESP_OK:
        printf("Done\n");
        printf("%s = %s\n", key, value);
        return true;
    case ESP_ERR_NVS_NOT_FOUND:
        printf("The value is not initialized yet!\n");
        break;
    default:
        printf("Error (%s) reading!\n", esp_err_to_name(ret));
    }
    return false;
}

static void apply_wifi_config(void)
{
    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
            .sae_pwe_h2e = ESP_WIFI_SAE_MODE,
            .sae_h2e_identifier = H2E_IDENTIFIER,
        },
    };
    memcpy(wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    memcpy(wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    esp_wifi_disconnect();
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    s_retry_num = 0;
    esp_wifi_connect();
}

/* Points the running MQTT client at whatever `url` now holds. The client
 * copies the URI at init time, so the config struct cannot simply be mutated;
 * stopping and restarting also means the new broker is used right away rather
 * than at some later reconnect. */
static bool apply_mqtt_config(const char **error)
{
    if (g_mqtt_client == NULL)
    {
        /* The device booted without a broker, so there is nothing to retarget;
         * create the client now instead. */
        if (mqtt_app_start(error) == NULL)
        {
            return false;
        }
        return true;
    }

    esp_mqtt_client_stop(g_mqtt_client);
    s_mqtt_connected = false;

    /* The whole configuration is reapplied rather than just the URI, because
     * the transport and the port both follow from the scheme. */
    esp_mqtt_client_config_t mqtt_cfg;
    if (!mqtt_make_config(&mqtt_cfg, error))
    {
        return false;
    }

    esp_err_t ret = esp_mqtt_set_config(g_mqtt_client, &mqtt_cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_mqtt_set_config failed: %s", esp_err_to_name(ret));
        *error = "broker URL rejected";
        return false;
    }

    ret = esp_mqtt_client_start(g_mqtt_client);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_mqtt_client_start failed: %s", esp_err_to_name(ret));
        *error = "could not restart the MQTT client";
        return false;
    }
    return true;
}

/* Moves the indoor subscription onto whatever `indoor_topic` now holds. The
 * previous topic is dropped first, so a renamed sensor stops feeding the
 * controller instead of quietly competing with the new one. */
static void apply_indoor_topic(const char *previous)
{
    esp_mqtt_client_handle_t client = g_mqtt_client;
    if (client == NULL || !s_mqtt_connected)
    {
        /* Nothing to move: the subscription is taken on the next connect. */
        return;
    }
    if (previous != NULL && previous[0] != '\0')
    {
        esp_mqtt_client_unsubscribe(client, previous);
    }
    if (has_indoor_topic())
    {
        esp_mqtt_client_subscribe(client, indoor_topic, 0);
    }
}

/* Why the heater must not run, or NULL when there is a water temperature to
 * judge it by. The only thing that ever turns the heater off is the water
 * sensor reaching the off threshold, so without a reading that can be trusted
 * there is nothing to stop it: it must neither be started nor left running.
 * A sensor that has stopped reporting counts as no reading at all, however
 * plausible the last value it gave was. */
static const char *heater_no_water_reason(void)
{
    int i = role_index[SENSOR_ROLE_WATER];
    if (i < 0)
    {
        return role_rom[SENSOR_ROLE_WATER][0] == '\0'
                   ? "no sensor has the water role"
                   : "the water sensor is not on the bus";
    }
    if (!ever_good[i] || isnan(last_water_temp))
    {
        return "the water sensor has not produced a reading yet";
    }
    if ((xTaskGetTickCount() - last_good_reading[i]) >= pdMS_TO_TICKS(TEMP_READING_TIMEOUT_MS))
    {
        return "the water temperature is stale";
    }
    return NULL;
}

/* The force-off state lives in NVS so that a heater stopped over BLE stays
 * stopped across a reboot instead of silently reverting to automatic control. */
static void set_heater_force_state(heater_force_state_t state)
{
    heater_force_state = state;
    esp_err_t ret = nvs_set_u8(g_nvs_handle, "heatForce", (uint8_t)state);
    if (ret == ESP_OK)
    {
        ret = nvs_commit(g_nvs_handle);
    }
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to persist heater force state: %s", esp_err_to_name(ret));
    }
}

/* Switches to `mode`, which the app, the CLI and the MQTT command topic all
 * ask for by different names. Returns false with *error set when the mode would
 * run the heater and there is nothing to stop it by, in which case nothing
 * changes at all - not even the mode, since a start that cannot be honoured
 * should leave the device exactly as it was.
 *
 * Only the application task may call this: it moves the output. */
static bool apply_heater_mode(heater_force_state_t mode, const char **error)
{
    /* Switching the heater off is always allowed; starting it is not, unless
     * the water temperature that ends the cycle can be trusted. */
    if (mode == HEATER_FORCE_NONE || mode == HEATER_RUN_ONCE)
    {
        const char *blocked = heater_no_water_reason();
        if (blocked != NULL)
        {
            ESP_LOGW(TAG, "Refusing heater mode '%s': %s", heater_mode_name(mode), blocked);
            /* Nothing moved, so nothing would be published - and a subscriber
             * that asked for this mode would be left showing it. Re-assert
             * what the mode really is instead. */
            s_heater_state_dirty = true;
            *error = blocked;
            return false;
        }
    }

    bool start = false;
    switch (mode)
    {
    case HEATER_FORCE_NONE:
        start = last_water_temp < heater_off_threshold;
        if (!start)
        {
            ESP_LOGI(TAG, "Heater start requested but water temperature %.1f is above the off threshold",
                     last_water_temp);
        }
        break;
    case HEATER_RUN_ONCE:
        start = last_water_temp < heater_off_threshold;
        if (!start)
        {
            /* Asking for one cycle when the water is already hot enough is not
             * an error, but there is no cycle to run: the stop condition is met
             * on arrival, so the mode it would have ended in is the mode it
             * starts in. */
            ESP_LOGI(TAG, "Single heating cycle requested but water temperature %.1f is already at the stop condition",
                     last_water_temp);
            mode = HEATER_FORCE_OFF_UNTIL_STARTED;
        }
        break;
    default:
        break;
    }

    set_heater_force_state(mode);
    heater_on = start;
    gpio_set_level(GPIO_HEATER, heater_on);
    ESP_LOGI(TAG, "Heater mode '%s', output %s", heater_mode_name(mode), heater_on ? "on" : "off");
    return true;
}

static void process_ble_commands(void)
{
    ble_command_t cmd;
    while (xQueueReceive(g_ble_cmd_queue, &cmd, 0) == pdTRUE)
    {
        switch (cmd.type)
        {
        case BLE_CMD_SET_WIFI:
        {
            memset(ssid, 0, sizeof(ssid));
            memset(password, 0, sizeof(password));
            strlcpy((char *)ssid, cmd.data.wifi.ssid, sizeof(ssid));
            strlcpy((char *)password, cmd.data.wifi.password, sizeof(password));
            nvs_set_str(g_nvs_handle, "SSID", (char *)ssid);
            nvs_set_str(g_nvs_handle, "PW", (char *)password);
            nvs_commit(g_nvs_handle);
            apply_wifi_config();
            ESP_LOGI(TAG, "Wi-Fi credentials updated via BLE, SSID=%s", ssid);
            ble_service_report_status("set_wifi", true, NULL);
            break;
        }
        case BLE_CMD_SET_MQTT:
        {
            char previous[sizeof(url)];
            strlcpy(previous, url, sizeof(previous));

            memset(url, 0, sizeof(url));
            strlcpy(url, cmd.data.mqtt.url, sizeof(url));

            const char *error = NULL;
            if (!apply_mqtt_config(&error))
            {
                /* Keep NVS and the running client consistent: put the old
                 * broker back rather than persisting one that will not start.
                 * A device that booted unprovisioned simply stays that way. */
                strlcpy(url, previous, sizeof(url));
                if (has_mqtt_config())
                {
                    const char *rollback_error = NULL;
                    apply_mqtt_config(&rollback_error);
                }
                ble_service_report_status("set_mqtt", false, error);
                break;
            }

            nvs_set_str(g_nvs_handle, "MQTT", url);
            nvs_commit(g_nvs_handle);
            ESP_LOGI(TAG, "MQTT broker updated via BLE, URL=%s", url);
            ble_service_report_status("set_mqtt", true, NULL);
            break;
        }
        case BLE_CMD_SET_SENSOR_ROLE:
        {
            const char *reply = cmd.data.sensor_role.cmd;
            const char *id = cmd.data.sensor_role.rom_code_hex;
            sensor_role_t role = role_from_name(cmd.data.sensor_role.role);
            if (role == SENSOR_ROLE_NONE && strcasecmp(cmd.data.sensor_role.role, "none") != 0)
            {
                ble_service_report_status(reply, false, "role must be water, outdoor, supply or none");
                break;
            }

            /* A sensor does one job at a time, so taking it for this role
             * releases it from whichever one it held before. */
            for (int i = 0; i < SENSOR_ROLE_COUNT; ++i)
            {
                if (strcasecmp(role_rom[i], id) == 0)
                {
                    role_rom[i][0] = '\0';
                    nvs_set_str(g_nvs_handle, role_keys[i], "");
                }
            }
            if (role != SENSOR_ROLE_NONE)
            {
                strlcpy(role_rom[role], id, sizeof(role_rom[role]));
                nvs_set_str(g_nvs_handle, role_keys[role], role_rom[role]);
            }
            nvs_commit(g_nvs_handle);
            resolve_role_indexes();
            last_water_temp = NAN;

            if (role == SENSOR_ROLE_NONE)
            {
                ESP_LOGI(TAG, "Sensor %s no longer has a role", id);
                ble_service_report_status(reply, true, NULL);
            }
            else if (role_index[role] < 0)
            {
                ESP_LOGW(TAG, "Sensor %s is not on the bus; the %s role is stored for later",
                         id, role_names[role]);
                ble_service_report_status(reply, true, "sensor not currently present; stored for later");
            }
            else
            {
                ESP_LOGI(TAG, "Sensor %s is now the %s sensor (index %d)", id, role_names[role],
                         role_index[role]);
                ble_service_report_status(reply, true, NULL);
            }
            break;
        }
        case BLE_CMD_SET_SHUNT:
        {
            shunt_config_t cfg;
            shunt_get_config(&cfg);
            if (cmd.data.shunt.enabled >= 0)
            {
                cfg.enabled = cmd.data.shunt.enabled != 0;
            }
            cfg.slope = cmd.data.shunt.slope;
            cfg.offset_c = cmd.data.shunt.offset_c;
            cfg.room_target_c = cmd.data.shunt.room_target_c;
            cfg.min_supply_c = cmd.data.shunt.min_supply_c;
            cfg.max_supply_c = cmd.data.shunt.max_supply_c;
            cfg.burst_ms = cmd.data.shunt.burst_ms;
            cfg.pause_s = cmd.data.shunt.pause_s;
            cfg.tolerance_c = cmd.data.shunt.tolerance_c;
            cfg.indoor_gain = cmd.data.shunt.indoor_gain;
            cfg.indoor_max_c = cmd.data.shunt.indoor_max_c;
            cfg.indoor_stale_s = cmd.data.shunt.indoor_stale_s;

            const char *error = NULL;
            if (!shunt_set_config(&cfg, &error))
            {
                ble_service_report_status("set_shunt", false, error);
                break;
            }

            if (cmd.data.shunt.set_indoor_topic)
            {
                char previous[sizeof(indoor_topic)];
                strlcpy(previous, indoor_topic, sizeof(previous));
                strlcpy(indoor_topic, cmd.data.shunt.indoor_topic, sizeof(indoor_topic));
                nvs_set_str(g_nvs_handle, "indTopic", indoor_topic);
                nvs_commit(g_nvs_handle);
                apply_indoor_topic(previous);
                ESP_LOGI(TAG, "Indoor temperature topic set to '%s'", indoor_topic);
            }
            ble_service_report_status("set_shunt", true, NULL);
            break;
        }
        case BLE_CMD_SHUNT_JOG:
        {
            shunt_dir_t dir = SHUNT_DIR_IDLE;
            if (strcasecmp(cmd.data.jog.dir, "warmer") == 0)
            {
                dir = SHUNT_DIR_WARMER;
            }
            else if (strcasecmp(cmd.data.jog.dir, "colder") == 0)
            {
                dir = SHUNT_DIR_COLDER;
            }
            const char *error = NULL;
            if (!shunt_jog(dir, cmd.data.jog.ms, &error))
            {
                ble_service_report_status("shunt_jog", false, error);
                break;
            }
            ble_service_report_status("shunt_jog", true, NULL);
            break;
        }
        case BLE_CMD_SET_THRESHOLDS:
        {
            heater_on_threshold = cmd.data.thresholds.on_c;
            heater_off_threshold = cmd.data.thresholds.off_c;
            nvs_set_i32(g_nvs_handle, "heatOnC", (int32_t)lroundf(heater_on_threshold * 100.0f));
            nvs_set_i32(g_nvs_handle, "heatOffC", (int32_t)lroundf(heater_off_threshold * 100.0f));
            nvs_commit(g_nvs_handle);
            ESP_LOGI(TAG, "Heater thresholds updated: on=%.1f off=%.1f", heater_on_threshold, heater_off_threshold);
            ble_service_report_status("set_thresholds", true, NULL);
            break;
        }
        case BLE_CMD_HEATER_FORCE_OFF:
        {
            const char *error = NULL;
            bool ok = apply_heater_mode(cmd.data.heater_force_off.mode == BLE_HEATER_FORCE_OFF_UNTIL_STARTED
                                            ? HEATER_FORCE_OFF_UNTIL_STARTED
                                            : HEATER_FORCE_OFF_UNTIL_CONDITIONS,
                                        &error);
            ble_service_report_status("heater_off", ok, error);
            break;
        }
        case BLE_CMD_HEATER_ON:
        {
            const char *error = NULL;
            bool ok = apply_heater_mode(HEATER_FORCE_NONE, &error);
            ble_service_report_status("heater_on", ok, error);
            break;
        }
        case BLE_CMD_HEATER_ONCE:
        {
            const char *error = NULL;
            bool ok = apply_heater_mode(HEATER_RUN_ONCE, &error);
            ble_service_report_status("heater_once", ok, error);
            break;
        }
        case BLE_CMD_SET_HEATER_MODE:
        {
            /* From the MQTT command topic, so there is no status
             * characteristic to answer on: the outcome is the mode that
             * publish_heater_state() puts back on the state topic, and a
             * refusal is logged by apply_heater_mode(). */
            const char *error = NULL;
            (void)apply_heater_mode((heater_force_state_t)cmd.data.heater_mode.mode, &error);
            break;
        }
        case BLE_CMD_OTA_UPDATE:
        {
            const char *error = NULL;
            if (!s_wifi_connected)
            {
                ble_service_report_status("ota_update", false, "Wi-Fi not connected");
                break;
            }
            if (!ota_start(cmd.data.ota.tag, cmd.data.ota.force, &error))
            {
                ESP_LOGW(TAG, "Firmware update to %s not started: %s", cmd.data.ota.tag, error);
                ble_service_report_status("ota_update", false, error);
                break;
            }
            ESP_LOGI(TAG, "Firmware update to %s started via BLE", cmd.data.ota.tag);
            ble_service_report_status("ota_update", true, NULL);
            break;
        }
        case BLE_CMD_OTA_BLE_BEGIN:
        {
            const char *error = NULL;
            if (!ota_ble_begin(cmd.data.ota_ble.size, cmd.data.ota_ble.crc32,
                               cmd.data.ota_ble.version, cmd.data.ota_ble.force, &error))
            {
                ESP_LOGW(TAG, "Firmware transfer not started: %s", error);
                ble_service_report_status("ota_ble_begin", false, error);
                break;
            }
            ble_service_report_status("ota_ble_begin", true, NULL);
            break;
        }
        case BLE_CMD_OTA_BLE_END:
        {
            const char *error = NULL;
            if (!ota_ble_end(&error))
            {
                ESP_LOGW(TAG, "Firmware transfer rejected: %s", error);
                ble_service_report_status("ota_ble_end", false, error);
                break;
            }
            ble_service_report_status("ota_ble_end", true, NULL);
            /* Long enough for the status notification to go out, and for the
             * phone to read it, before the link drops with the reset. */
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();
            break;
        }
        case BLE_CMD_OTA_BLE_ABORT:
        {
            ota_ble_abort();
            ble_service_report_status("ota_ble_abort", true, NULL);
            break;
        }
        }
    }
}

static void publish_telemetry(void)
{
    ble_telemetry_t telemetry = {0};
    telemetry.num_readings = num_devices < BLE_MAX_TEMP_SENSORS ? num_devices : BLE_MAX_TEMP_SENSORS;
    for (int i = 0; i < telemetry.num_readings; ++i)
    {
        owb_string_from_rom_code(device_rom_codes[i], telemetry.readings[i].rom_code_hex,
                                  sizeof(telemetry.readings[i].rom_code_hex));
        telemetry.readings[i].valid = ever_good[i];
        telemetry.readings[i].value_c = last_good_value[i];
        telemetry.readings[i].age_ms = ever_good[i]
                                            ? (uint32_t)((xTaskGetTickCount() - last_good_reading[i]) * portTICK_PERIOD_MS)
                                            : UINT32_MAX;
        sensor_role_t role = role_of_sensor(i);
        telemetry.readings[i].role = role == SENSOR_ROLE_NONE ? NULL : role_names[role];
    }
    telemetry.wifi_connected = s_wifi_connected;
    strlcpy(telemetry.wifi_ssid, (char *)ssid, sizeof(telemetry.wifi_ssid));
    wifi_ap_record_t ap_info;
    if (s_wifi_connected && esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
    {
        telemetry.wifi_rssi_valid = true;
        telemetry.wifi_rssi = ap_info.rssi;
    }
    telemetry.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    telemetry.wifi_disconnect_count = s_wifi_disconnect_count;
    telemetry.wifi_last_disc_reason = s_wifi_last_disc_reason;
    telemetry.wifi_last_disc_rssi = s_wifi_last_disc_rssi;
    telemetry.wifi_last_disc_age_ms = s_wifi_disconnect_count > 0
                                          ? (uint32_t)((xTaskGetTickCount() - s_wifi_last_disc_tick) * portTICK_PERIOD_MS)
                                          : UINT32_MAX;
    telemetry.mqtt_connected = s_mqtt_connected;
    strlcpy(telemetry.mqtt_url, url, sizeof(telemetry.mqtt_url));
    telemetry.heater_on = heater_on;
    telemetry.heater_on_threshold_c = heater_on_threshold;
    telemetry.heater_off_threshold_c = heater_off_threshold;
    telemetry.heater_force_state = (int)heater_force_state;

    shunt_config_t shunt_cfg;
    shunt_status_t shunt;
    shunt_get_config(&shunt_cfg);
    shunt_get_status(&shunt);
    telemetry.shunt_enabled = shunt_cfg.enabled;
    telemetry.shunt_state = shunt_state_name(shunt.state);
    telemetry.shunt_dir = shunt_dir_name(shunt.dir);
    telemetry.shunt_reason = shunt.reason;
    telemetry.shunt_setpoint_c = shunt.setpoint_c;
    telemetry.shunt_supply_c = shunt.supply_c;
    telemetry.shunt_outdoor_c = shunt.outdoor_c;
    telemetry.shunt_bursts = shunt.bursts;
    telemetry.curve_slope = shunt_cfg.slope;
    telemetry.curve_offset_c = shunt_cfg.offset_c;
    telemetry.curve_target_c = shunt_cfg.room_target_c;
    telemetry.curve_min_supply_c = shunt_cfg.min_supply_c;
    telemetry.curve_max_supply_c = shunt_cfg.max_supply_c;
    telemetry.burst_ms = shunt_cfg.burst_ms;
    telemetry.pause_s = shunt_cfg.pause_s;
    telemetry.tolerance_c = shunt_cfg.tolerance_c;
    strlcpy(telemetry.indoor_topic, indoor_topic, sizeof(telemetry.indoor_topic));
    telemetry.indoor_c = shunt.indoor_c;
    telemetry.indoor_age_ms = shunt.indoor_age_ms;
    telemetry.indoor_fresh = shunt.indoor_fresh;
    telemetry.indoor_trim_c = shunt.indoor_trim_c;
    telemetry.indoor_gain = shunt_cfg.indoor_gain;
    telemetry.indoor_max_c = shunt_cfg.indoor_max_c;
    telemetry.indoor_stale_s = shunt_cfg.indoor_stale_s;

    strlcpy(telemetry.fw_version, ota_running_version(), sizeof(telemetry.fw_version));
    ota_status_t ota;
    ota_get_status(&ota);
    telemetry.ota_state = ota_state_name(ota.state);
    telemetry.ota_percent = ota.percent;
    strlcpy(telemetry.ota_version, ota.version, sizeof(telemetry.ota_version));
    telemetry.ota_error = ota.error;
    ble_service_update_telemetry(&telemetry);
}

/* Called once per loop pass, so getting here means BLE is up and the control
 * loop runs. A new image also has to reach Wi-Fi if it is configured, since
 * one that cannot would never be updated remotely again; if it does not get
 * there in time, the bootloader goes back to the previous image. */
static void check_pending_firmware(void)
{
    /* Also the point where a transfer that died with the phone is cleaned up. */
    ota_ble_tick();

    if (!ota_is_pending_verify())
    {
        return;
    }
    if (!has_wifi_config() || s_wifi_connected)
    {
        ota_confirm_if_pending();
    }
    else if (xTaskGetTickCount() * portTICK_PERIOD_MS >= OTA_CONFIRM_TIMEOUT_MS)
    {
        ota_rollback_if_pending();
    }
}

_Noreturn void app_main()
{
    // Override global log level
    esp_log_level_set("*", ESP_LOG_INFO);
    gpio_config_t io_conf = {};

    // disable interrupt
    io_conf.intr_type = GPIO_INTR_DISABLE;
    // set as output mode
    io_conf.mode = GPIO_MODE_OUTPUT;
    // bit mask of the pins that you want to set. GPIO18 and GPIO19 used to
    // drive diagnostic LEDs; they now run the shunt actuator, and shunt_init()
    // claims them.
    io_conf.pin_bit_mask = (1ULL << GPIO_NUM_27);
    // disable pull-down mode
    io_conf.pull_down_en = 0;
    // disable pull-up mode
    io_conf.pull_up_en = 0;
    // configure GPIO with the given settings
    gpio_config(&io_conf);
    gpio_set_level(GPIO_HEATER, false);
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    printf("\n");
    printf("Opening Non-Volatile Storage (NVS) handle... ");
    nvs_handle_t my_handle;
    ret = nvs_open("connection", NVS_READWRITE, &my_handle);
    if (ret != ESP_OK)
    {
        printf("Error (%s) opening NVS handle!\n", esp_err_to_name(ret));
    }
    else
    {
        printf("Done\n");

        /* A value that has never been written leaves its buffer empty. The
         * device then boots unconfigured with BLE advertising, so set_wifi and
         * set_mqtt can provision it; rebooting instead would be a loop, because
         * BLE never starts early enough to receive the commands that would fix
         * it. */
        size_t length = sizeof(ssid);
        if (!read_value(my_handle, "SSID", ssid, &length))
        {
            ssid[0] = '\0';
        }
        length = sizeof(password);
        if (!read_value(my_handle, "PW", password, &length))
        {
            password[0] = '\0';
        }
        length = sizeof(url);
        if (!read_value(my_handle, "MQTT", (uint8_t *)url, &length))
        {
            url[0] = '\0';
        }
        if (!has_wifi_config() || !has_mqtt_config())
        {
            ESP_LOGW(TAG, "Device is not fully provisioned (wifi=%s, mqtt=%s); "
                          "configure it over BLE",
                     has_wifi_config() ? "set" : "missing",
                     has_mqtt_config() ? "set" : "missing");
        }

        g_nvs_handle = my_handle;

        for (int i = 0; i < SENSOR_ROLE_COUNT; ++i)
        {
            size_t length = sizeof(role_rom[i]);
            if (nvs_get_str(my_handle, role_keys[i], role_rom[i], &length) != ESP_OK)
            {
                role_rom[i][0] = '\0';
            }
            if (role_rom[i][0] != '\0')
            {
                printf("Persisted %s sensor: %s\n", role_names[i], role_rom[i]);
            }
        }

        size_t topic_len = sizeof(indoor_topic);
        if (nvs_get_str(my_handle, "indTopic", indoor_topic, &topic_len) != ESP_OK)
        {
            indoor_topic[0] = '\0';
        }

        int32_t on_centi = 0, off_centi = 0;
        heater_on_threshold = (nvs_get_i32(my_handle, "heatOnC", &on_centi) == ESP_OK) ? on_centi / 100.0f : HEATER_ON_THRESHOLD;
        heater_off_threshold = (nvs_get_i32(my_handle, "heatOffC", &off_centi) == ESP_OK) ? off_centi / 100.0f : HEATER_OFF_THRESHOLD;
        printf("Heater thresholds: on=%.1f off=%.1f\n", heater_on_threshold, heater_off_threshold);

        uint8_t force = HEATER_FORCE_NONE;
        if (nvs_get_u8(my_handle, "heatForce", &force) == ESP_OK && force <= HEATER_RUN_ONCE)
        {
            heater_force_state = (heater_force_state_t)force;
        }
        printf("Heater force state: %s\n", heater_mode_name(heater_force_state));
    }

    shunt_init(g_nvs_handle);
    ota_init();

    g_ble_cmd_queue = xQueueCreate(8, sizeof(ble_command_t));
    ble_service_init(g_ble_cmd_queue, ota_ble_write);

    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();
    const char *mqtt_error = NULL;
    if (mqtt_app_start(&mqtt_error) == NULL && has_mqtt_config())
    {
        ESP_LOGE(TAG, "MQTT client not started: %s", mqtt_error);
    }
    xTaskCreate(connection_monitor_task, "connection_monitor", 4096, NULL, 5, NULL);
    // Stable readings require a brief period before communication
    vTaskDelay(2000.0 / portTICK_PERIOD_MS);

    // Create a 1-Wire bus, using the RMT timeslot driver
    OneWireBus *owb;
    owb_rmt_driver_info rmt_driver_info;
    owb = owb_rmt_initialize(&rmt_driver_info, GPIO_DS18B20_0, RMT_CHANNEL_1, RMT_CHANNEL_0);
    owb_use_crc(owb, true); // enable CRC check for ROM code

    // Find all connected devices
    printf("Find devices:\n");
    const int AVG_COUNT = 10;
    float meas[MAX_DEVICES][AVG_COUNT];
    for (int i = 0; i < AVG_COUNT; ++i)
        for (int j = 0; j < MAX_DEVICES; ++j)
            meas[j][i] = 0;
    int current = 0;
    int count = 0;
    OneWireBus_SearchState search_state = {0};
    bool found = false;
    owb_search_first(owb, &search_state, &found);
    while (found)
    {
        char rom_code_s[17];
        owb_string_from_rom_code(search_state.rom_code, rom_code_s, sizeof(rom_code_s));
        printf("  %d : %s\n", num_devices, rom_code_s);
        device_rom_codes[num_devices] = search_state.rom_code;
        ++num_devices;
        owb_search_next(owb, &search_state, &found);
    }
    printf("Found %d device%s\n", num_devices, num_devices == 1 ? "" : "s");

    resolve_role_indexes();
    for (int i = 0; i < SENSOR_ROLE_COUNT; ++i)
    {
        if (role_rom[i][0] == '\0')
        {
            continue;
        }
        if (role_index[i] >= 0)
        {
            ESP_LOGI(TAG, "%s sensor %s found at index %d", role_names[i], role_rom[i], role_index[i]);
        }
        else
        {
            ESP_LOGW(TAG, "%s sensor %s is not on the bus", role_names[i], role_rom[i]);
        }
    }

    // Create DS18B20 devices on the 1-Wire bus
    DS18B20_Info *devices[MAX_DEVICES] = {0};
    for (int i = 0; i < num_devices; ++i)
    {
        DS18B20_Info *ds18b20_info = ds18b20_malloc(); // heap allocation
        devices[i] = ds18b20_info;

        if (num_devices == 1)
        {
            printf("Single device optimisations enabled\n");
            ds18b20_init_solo(ds18b20_info, owb); // only one device on bus
        }
        else
        {
            ds18b20_init(ds18b20_info, owb, device_rom_codes[i]); // associate with bus and device
        }
        ds18b20_use_crc(ds18b20_info, true); // enable CRC check on all reads
        ds18b20_set_resolution(ds18b20_info, DS18B20_RESOLUTION);
    }

    // Check for parasitic-powered devices
    bool parasitic_power = false;
    ds18b20_check_for_parasite_power(owb, &parasitic_power);
    if (parasitic_power)
    {
        printf("Parasitic-powered devices detected");
    }

    // In parasitic-power mode, devices cannot indicate when conversions are complete,
    // so waiting for a temperature conversion must be done by waiting a prescribed duration
    owb_use_parasitic_power(owb, parasitic_power);

    // Read temperatures more efficiently by starting conversions on all devices at the same time
    int errors_count[MAX_DEVICES] = {0};
    int sample_count = 0;
    if (num_devices > 0)
    {
        TickType_t last_wake_time = xTaskGetTickCount();

        while (1)
        {
            if (count < AVG_COUNT)
                ++count;
            ds18b20_convert_all(owb);

            // In this application all devices use the same resolution,
            // so use the first device to determine the delay
            ds18b20_wait_for_conversion(devices[0]);

            // Read the results immediately after conversion otherwise it may fail
            // (using printf before reading may take too long)
            float readings[MAX_DEVICES] = {0};
            DS18B20_ERROR errors[MAX_DEVICES] = {0};

            for (int i = 0; i < num_devices; ++i)
            {
                errors[i] = ds18b20_read_temp(devices[i], &readings[i]);
            }

            /* Fold this sample into each sensor's history and average it,
             * before anything acts on it: both the commands processed below and
             * the thermostat after them judge the filtered value, so they must
             * not be a sample behind. */
            float filtered[MAX_DEVICES];
            for (int i = 0; i < num_devices; ++i)
            {
                if (errors[i] == DS18B20_OK)
                {
                    last_good_reading[i] = xTaskGetTickCount();
                    ever_good[i] = true;
                    last_good_value[i] = readings[i];
                    meas[i][current] = readings[i];
                }
                else
                {
                    ++errors_count[i];
                    /* Repeat the previous value rather than let a dropped read
                     * drag the average towards nothing. A sensor that keeps
                     * failing is caught by its reading going stale, which is
                     * what stops the heater. */
                    meas[i][current] = meas[i][(current + AVG_COUNT - 1) % AVG_COUNT];
                }
                float sum = 0;
                for (int j = 0; j < count; ++j)
                {
                    sum += meas[i][j];
                }
                filtered[i] = sum / (float)count;
            }
            if (role_index[SENSOR_ROLE_WATER] >= 0)
            {
                last_water_temp = filtered[role_index[SENSOR_ROLE_WATER]];
            }

            process_ble_commands();

            for (int i = 0; i < num_devices; ++i)
            {
                if (errors[i] != DS18B20_OK)
                {
                    continue;
                }

                /* The shunt controller runs in its own task and only needs the
                 * two readings it regulates on. */
                if (i == role_index[SENSOR_ROLE_OUTDOOR])
                {
                    shunt_report_outdoor(readings[i]);
                }
                if (i == role_index[SENSOR_ROLE_SUPPLY])
                {
                    shunt_report_supply(readings[i]);
                }

                if (role_index[SENSOR_ROLE_WATER] < 0 && role_rom[SENSOR_ROLE_WATER][0] == '\0' &&
                    readings[i] >= WATER_TEMP_IDENTIFICATION_THRESHOLD)
                {
                    /* Not persisted: this is a guess from the temperature
                     * itself, which a later assignment from the app replaces. */
                    role_index[SENSOR_ROLE_WATER] = i;
                    last_water_temp = filtered[i];
                    heater_on = false;
                    ESP_LOGI(TAG, "Sensor %d identified as water temperature; heater off at %.1f C", i, readings[i]);
                }
            }

            /* The thermostat. It runs once per sample rather than per sensor,
             * and on the filtered water temperature, so a sample the water
             * sensor failed to produce no longer skips the decision - it ages
             * the reading out instead, and that turns the heater off. */
            const char *no_water = heater_no_water_reason();
            if (no_water != NULL)
            {
                if (heater_on)
                {
                    ESP_LOGW(TAG, "Heater off: %s", no_water);
                }
                /* Only the output; the mode is left alone, so a single cycle
                 * interrupted by a dead sensor resumes when it comes back. */
                heater_on = false;
            }
            else
            {
                bool want_on = heater_on;
                if (heater_on && last_water_temp >= heater_off_threshold)
                {
                    want_on = false;
                    ESP_LOGI(TAG, "Water temperature reached %.1f C; heater off", last_water_temp);
                }
                else if (!heater_on && last_water_temp <= heater_on_threshold)
                {
                    want_on = true;
                    ESP_LOGI(TAG, "Water temperature dropped to %.1f C; heater on", last_water_temp);
                }

                if (heater_force_state == HEATER_FORCE_OFF_UNTIL_STARTED)
                {
                    heater_on = false;
                }
                else if (heater_force_state == HEATER_RUN_ONCE)
                {
                    /* The on threshold has no say here: the point of the mode
                     * is to finish the cycle it was started for, and then to
                     * stay off until somebody says otherwise. */
                    if (last_water_temp >= heater_off_threshold)
                    {
                        set_heater_force_state(HEATER_FORCE_OFF_UNTIL_STARTED);
                        heater_on = false;
                        ESP_LOGI(TAG, "Single heating cycle finished at %.1f C; off until explicitly started", last_water_temp);
                    }
                    else
                    {
                        heater_on = true;
                    }
                }
                else if (heater_force_state == HEATER_FORCE_OFF_UNTIL_CONDITIONS)
                {
                    if (want_on)
                    {
                        set_heater_force_state(HEATER_FORCE_NONE);
                        heater_on = true;
                        ESP_LOGI(TAG, "Start conditions met again; resuming automatic heater control");
                    }
                    else
                    {
                        heater_on = false;
                    }
                }
                else
                {
                    heater_on = want_on;
                }
            }
            gpio_set_level(GPIO_HEATER, heater_on);

            publish_telemetry();
            publish_heater_state();
            publish_shunt_state();
            check_pending_firmware();

            // Print results in a separate loop, after all have been read
            printf("\nTemperature readings (degrees C): sample %d\n", ++sample_count);
            for (int i = 0; i < num_devices; ++i)
            {
                char rom_code_s[17];
                owb_string_from_rom_code(devices[i]->rom_code, rom_code_s, sizeof(rom_code_s));

                char buf[10];
                char topic[100];
                printf("  %s: %.1f    %d errors\n", rom_code_s, readings[i], errors_count[i]);
                if (sample_count % AVG_COUNT == 0)
                {
                    int len = snprintf(buf, 10, "%.2f", filtered[i]);
                    snprintf(topic, 100, "temp/%s", rom_code_s);
                    if (len > 0)
                    {
                        mqtt_publish(topic, buf, len);
                    }
                }
                if (errors[i] != DS18B20_OK)
                {
                    int len = snprintf(buf, 10, "%d", errors_count[i]);
                    snprintf(topic, 100, "temp/errors/%s", rom_code_s);
                    if (len > 0)
                    {
                        mqtt_publish(topic, buf, len);
                    }
                }
            }
            current++;
            current %= AVG_COUNT;

            vTaskDelayUntil(&last_wake_time, SAMPLE_PERIOD / portTICK_PERIOD_MS);
        }
    }
    else
    {
        printf("\nNo DS18B20 devices detected!\n");
    }

    /* Reached only when the 1-Wire search found nothing. BLE still has to be
     * serviced here, otherwise the device would advertise and accept writes
     * while silently ignoring every command - which would make an unprovisioned
     * board with no sensors wired impossible to configure. */
    while (1)
    {
        process_ble_commands();

        /* A start command is already refused here, since no sensor can hold the
         * water role when none was found; this is the same backstop the
         * sampling loop keeps, in case the output was left on some other way. */
        if (heater_on)
        {
            ESP_LOGW(TAG, "No temperature sensors; refusing to run the heater");
            heater_on = false;
        }
        gpio_set_level(GPIO_HEATER, heater_on);

        publish_telemetry();
        publish_heater_state();
        publish_shunt_state();
        check_pending_firmware();

        vTaskDelay(SAMPLE_PERIOD / portTICK_PERIOD_MS);
    }

    // clean up dynamically allocated data
    for (int i = 0; i < num_devices; ++i)
    {
        ds18b20_free(&devices[i]);
    }
    owb_uninitialize(owb);

    printf("Restarting now.\n");
    fflush(stdout);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    esp_restart();
}