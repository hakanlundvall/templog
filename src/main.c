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
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "lwip/err.h"
#include "lwip/sys.h"

#include "mqtt_client.h"
#include "freertos/queue.h"
#include <math.h>
#include <strings.h>

#include "ble_service.h"

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
static uint32_t level2 = 0;
static volatile bool s_wifi_connected = false;
static volatile bool s_mqtt_connected = false;
static esp_mqtt_client_handle_t g_mqtt_client = NULL;

typedef enum {
    HEATER_FORCE_NONE = 0,
    HEATER_FORCE_OFF_UNTIL_CONDITIONS,
    HEATER_FORCE_OFF_UNTIL_STARTED,
} heater_force_state_t;

static nvs_handle_t g_nvs_handle;
static QueueHandle_t g_ble_cmd_queue;
static OneWireBus_ROMCode device_rom_codes[8];
static int num_devices = 0;
static int water_sensor_index = -1;
static bool water_sensor_persisted = false;
static bool heater_on = false;
static float last_water_temp = NAN;
static heater_force_state_t heater_force_state = HEATER_FORCE_NONE;
static float heater_on_threshold = 0;
static float heater_off_threshold = 0;

#define CONNECTION_RETRY_PERIOD_MS 60000

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
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED)
    {
        level2 = 0;
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        s_wifi_connected = false;
        level2 = 1;
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

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 ssid, password);
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 ssid, password);
    }
    else
    {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
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
        // esp_mqtt_client_publish(event->client, "temp/1/status", "connected", 0, 1, 0);

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

static esp_mqtt_client_handle_t mqtt_app_start(void)
{
    static char lwt_msg[] = "disconnected";
    static char connect_msg[] = "start";
    static char status_topic[] = "temp/1/status";
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = url,
        .session.last_will.msg = lwt_msg,
        .session.last_will.msg_len = sizeof(lwt_msg) - 1,
        .session.last_will.qos = 1,
        .session.last_will.topic = status_topic,
        .session.last_will.retain = 0,
        .session.keepalive = 9,
    };
    ESP_LOGI(TAG, "Starting MQTT client URL: %s", url);
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
    esp_mqtt_client_publish(client, status_topic, connect_msg, sizeof(connect_msg) - 1, 1, 0);

    g_mqtt_client = client;
    return client;
}

static void connection_monitor_task(void *arg)
{
    esp_mqtt_client_handle_t client = arg;

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(CONNECTION_RETRY_PERIOD_MS));

        if (!s_wifi_connected)
        {
            ESP_LOGW(TAG, "Wi-Fi is disconnected; retrying connection");
            s_retry_num = 0;
            esp_wifi_connect();
            continue;
        }

        if (!s_mqtt_connected)
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
        *error = "MQTT client not started yet";
        return false;
    }

    esp_mqtt_client_stop(g_mqtt_client);
    s_mqtt_connected = false;

    esp_err_t ret = esp_mqtt_client_set_uri(g_mqtt_client, url);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_mqtt_client_set_uri failed: %s", esp_err_to_name(ret));
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
                 * broker back rather than persisting one that will not start. */
                const char *rollback_error = NULL;
                strlcpy(url, previous, sizeof(url));
                apply_mqtt_config(&rollback_error);
                ble_service_report_status("set_mqtt", false, error);
                break;
            }

            nvs_set_str(g_nvs_handle, "MQTT", url);
            nvs_commit(g_nvs_handle);
            ESP_LOGI(TAG, "MQTT broker updated via BLE, URL=%s", url);
            ble_service_report_status("set_mqtt", true, NULL);
            break;
        }
        case BLE_CMD_SET_WATER_SENSOR:
        {
            int idx = -1;
            for (int i = 0; i < num_devices; ++i)
            {
                char rom_code_s[17];
                owb_string_from_rom_code(device_rom_codes[i], rom_code_s, sizeof(rom_code_s));
                if (strcasecmp(rom_code_s, cmd.data.water_sensor.rom_code_hex) == 0)
                {
                    idx = i;
                    break;
                }
            }
            nvs_set_str(g_nvs_handle, "waterRom", cmd.data.water_sensor.rom_code_hex);
            nvs_commit(g_nvs_handle);
            water_sensor_persisted = true;
            water_sensor_index = idx;
            last_water_temp = NAN;
            if (idx < 0)
            {
                ESP_LOGW(TAG, "Water sensor %s not currently present; will apply once seen",
                         cmd.data.water_sensor.rom_code_hex);
                ble_service_report_status("set_water_sensor", true, "sensor not currently present; stored for later");
            }
            else
            {
                ESP_LOGI(TAG, "Water sensor set to %s (index %d)", cmd.data.water_sensor.rom_code_hex, idx);
                ble_service_report_status("set_water_sensor", true, NULL);
            }
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
            heater_force_state = cmd.data.heater_force_off.mode == BLE_HEATER_FORCE_OFF_UNTIL_STARTED
                                      ? HEATER_FORCE_OFF_UNTIL_STARTED
                                      : HEATER_FORCE_OFF_UNTIL_CONDITIONS;
            heater_on = false;
            gpio_set_level(GPIO_HEATER, heater_on);
            ESP_LOGI(TAG, "Heater forced off (mode=%d)", (int)heater_force_state);
            ble_service_report_status("heater_off", true, NULL);
            break;
        }
        case BLE_CMD_HEATER_ON:
        {
            heater_force_state = HEATER_FORCE_NONE;
            if (!isnan(last_water_temp) && last_water_temp >= heater_off_threshold)
            {
                heater_on = false;
                ESP_LOGI(TAG, "Heater start requested but water temperature %.1f is above threshold", last_water_temp);
            }
            else
            {
                heater_on = true;
                ESP_LOGI(TAG, "Heater started via BLE command");
            }
            gpio_set_level(GPIO_HEATER, heater_on);
            ble_service_report_status("heater_on", true, NULL);
            break;
        }
        }
    }
}

_Noreturn void app_main()
{
    // Override global log level
    esp_log_level_set("*", ESP_LOG_INFO);
    uint32_t level = 1;
    gpio_config_t io_conf = {};

    // disable interrupt
    io_conf.intr_type = GPIO_INTR_DISABLE;
    // set as output mode
    io_conf.mode = GPIO_MODE_OUTPUT;
    // bit mask of the pins that you want to set,e.g.GPIO18/19
    io_conf.pin_bit_mask = (1ULL << GPIO_NUM_27) | (1ULL << GPIO_NUM_18) | (1ULL << GPIO_NUM_19);
    // disable pull-down mode
    io_conf.pull_down_en = 0;
    // disable pull-up mode
    io_conf.pull_up_en = 0;
    // configure GPIO with the given settings
    gpio_config(&io_conf);
    gpio_set_level(GPIO_HEATER, false);
    gpio_set_level(GPIO_NUM_18, level2);
    gpio_set_level(GPIO_NUM_19, !level);
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

        // Read
        printf("Reading  from NVS ... ");
        size_t length = sizeof(ssid);
        if (!read_value(my_handle, "SSID", ssid, &length))
        {
            printf("Restarting now.\n");
            fflush(stdout);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            esp_restart();
        }
        // memcpy(ssid, "Grythem464", 11);
        length = sizeof(password);
        if (!read_value(my_handle, "PW", password, &length))
        {
            printf("Restarting now.\n");
            fflush(stdout);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            esp_restart();
        }
        length = sizeof(url);
        if (!read_value(my_handle, "MQTT", (uint8_t *)url, &length))
        {
            printf("Restarting now.\n");
            fflush(stdout);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            esp_restart();
        }

        g_nvs_handle = my_handle;

        char water_rom[17] = {0};
        size_t water_rom_len = sizeof(water_rom);
        if (nvs_get_str(my_handle, "waterRom", water_rom, &water_rom_len) == ESP_OK && water_rom[0] != '\0')
        {
            water_sensor_persisted = true;
            printf("Persisted water sensor: %s\n", water_rom);
        }

        int32_t on_centi = 0, off_centi = 0;
        heater_on_threshold = (nvs_get_i32(my_handle, "heatOnC", &on_centi) == ESP_OK) ? on_centi / 100.0f : HEATER_ON_THRESHOLD;
        heater_off_threshold = (nvs_get_i32(my_handle, "heatOffC", &off_centi) == ESP_OK) ? off_centi / 100.0f : HEATER_OFF_THRESHOLD;
        printf("Heater thresholds: on=%.1f off=%.1f\n", heater_on_threshold, heater_off_threshold);
    }

    g_ble_cmd_queue = xQueueCreate(8, sizeof(ble_command_t));
    ble_service_init(g_ble_cmd_queue);

    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();
    esp_mqtt_client_handle_t client = mqtt_app_start();
    xTaskCreate(connection_monitor_task, "connection_monitor", 4096, client, 5, NULL);
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
    bool has_good_temp_reading = false;
    TickType_t last_good_temp_reading = 0;
    TickType_t last_good_reading[MAX_DEVICES] = {0};
    bool ever_good[MAX_DEVICES] = {0};
    float last_good_value[MAX_DEVICES] = {0};
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

    if (water_sensor_persisted)
    {
        char water_rom[17] = {0};
        size_t water_rom_len = sizeof(water_rom);
        if (nvs_get_str(g_nvs_handle, "waterRom", water_rom, &water_rom_len) == ESP_OK)
        {
            for (int i = 0; i < num_devices; ++i)
            {
                char rom_code_s[17];
                owb_string_from_rom_code(device_rom_codes[i], rom_code_s, sizeof(rom_code_s));
                if (strcasecmp(rom_code_s, water_rom) == 0)
                {
                    water_sensor_index = i;
                    ESP_LOGI(TAG, "Persisted water sensor %s found at index %d", water_rom, i);
                    break;
                }
            }
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

            bool good_temp_reading = false;
            for (int i = 0; i < num_devices; ++i)
            {
                if (errors[i] == DS18B20_OK)
                {
                    good_temp_reading = true;
                    break;
                }
            }

            process_ble_commands();

            if (good_temp_reading)
            {
                has_good_temp_reading = true;
                last_good_temp_reading = xTaskGetTickCount();
                if (water_sensor_index < 0 && !water_sensor_persisted)
                {
                    heater_on = (heater_force_state == HEATER_FORCE_NONE);
                }
            }

            for (int i = 0; i < num_devices; ++i)
            {
                if (errors[i] != DS18B20_OK)
                {
                    continue;
                }

                last_good_reading[i] = xTaskGetTickCount();
                ever_good[i] = true;
                last_good_value[i] = readings[i];

                if (water_sensor_index < 0 && !water_sensor_persisted && readings[i] >= WATER_TEMP_IDENTIFICATION_THRESHOLD)
                {
                    water_sensor_index = i;
                    heater_on = false;
                    ESP_LOGI(TAG, "Sensor %d identified as water temperature; heater off at %.1f C", i, readings[i]);
                }

                if (i == water_sensor_index)
                {
                    last_water_temp = readings[i];
                    bool want_on = heater_on;
                    if (heater_on && readings[i] >= heater_off_threshold)
                    {
                        want_on = false;
                        ESP_LOGI(TAG, "Water temperature reached %.1f C; heater off", readings[i]);
                    }
                    else if (!heater_on && readings[i] <= heater_on_threshold)
                    {
                        want_on = true;
                        ESP_LOGI(TAG, "Water temperature dropped to %.1f C; heater on", readings[i]);
                    }

                    if (heater_force_state == HEATER_FORCE_OFF_UNTIL_STARTED)
                    {
                        heater_on = false;
                    }
                    else if (heater_force_state == HEATER_FORCE_OFF_UNTIL_CONDITIONS)
                    {
                        if (want_on)
                        {
                            heater_force_state = HEATER_FORCE_NONE;
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
            }

            if (!has_good_temp_reading ||
                (xTaskGetTickCount() - last_good_temp_reading) >= pdMS_TO_TICKS(TEMP_READING_TIMEOUT_MS))
            {
                if (heater_on)
                {
                    ESP_LOGW(TAG, "No valid temperature reading for %d seconds; heater off", TEMP_READING_TIMEOUT_MS / 1000);
                }
                heater_on = false;
            }
            gpio_set_level(GPIO_HEATER, heater_on);

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
                    telemetry.readings[i].is_water_sensor = (i == water_sensor_index);
                }
                telemetry.wifi_connected = s_wifi_connected;
                strlcpy(telemetry.wifi_ssid, (char *)ssid, sizeof(telemetry.wifi_ssid));
                telemetry.mqtt_connected = s_mqtt_connected;
                strlcpy(telemetry.mqtt_url, url, sizeof(telemetry.mqtt_url));
                telemetry.heater_on = heater_on;
                telemetry.heater_on_threshold_c = heater_on_threshold;
                telemetry.heater_off_threshold_c = heater_off_threshold;
                telemetry.heater_force_state = (int)heater_force_state;
                ble_service_update_telemetry(&telemetry);
            }

            // Print results in a separate loop, after all have been read
            printf("\nTemperature readings (degrees C): sample %d\n", ++sample_count);
            for (int i = 0; i < num_devices; ++i)
            {
                char rom_code_s[17];
                bool publish_error = false;
                owb_string_from_rom_code(devices[i]->rom_code, rom_code_s, sizeof(rom_code_s));

                if (errors[i] != DS18B20_OK)
                {
                    ++errors_count[i];
                    meas[i][current] = meas[i][(current + AVG_COUNT - 1) % AVG_COUNT];
                    publish_error = true;
                }
                else
                {
                    meas[i][current] = readings[i];
                }
                char buf[10];
                char topic[100];
                printf("  %s: %.1f    %d errors\n", rom_code_s, readings[i], errors_count[i]);
                float sum = 0;
                for (int j = 0; j < 10 && j < count; ++j)
                    sum += meas[i][j];
                if (sample_count % AVG_COUNT == 0)
                {
                    int len = snprintf(buf, 10, "%.2f", sum / (float)count);
                    snprintf(topic, 100, "temp/%s", rom_code_s);
                    if (len > 0)
                    {
                        int msg_id = esp_mqtt_client_publish(client, topic, buf, len, 1, 0);
                        ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
                    }
                }
                if (publish_error)
                {
                    int len = snprintf(buf, 10, "%d", errors_count[i]);
                    snprintf(topic, 100, "temp/errors/%s", rom_code_s);
                    if (len > 0)
                    {
                        int msg_id = esp_mqtt_client_publish(client, topic, buf, len, 1, 0);
                        ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
                    }
                }
            }
            current++;
            current %= AVG_COUNT;
            level = level ? 0 : 1;
            gpio_set_level(GPIO_NUM_18, level2);
            gpio_set_level(GPIO_NUM_19, !level);

            vTaskDelayUntil(&last_wake_time, SAMPLE_PERIOD / portTICK_PERIOD_MS);
        }
    }
    else
    {
        printf("\nNo DS18B20 devices detected!\n");
    }

    while (1)
    {
        level = level ? 0 : 1;
        gpio_set_level(GPIO_HEATER, heater_on);
        gpio_set_level(GPIO_NUM_18, level2);
        gpio_set_level(GPIO_NUM_19, !level);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
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