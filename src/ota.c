#include "ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota";

/* GitHub answers a release download with one or two redirects, the last to a
 * signed URL of roughly 950 characters. The default 512 byte buffers cannot
 * hold that URL in the request line or the Location header. */
#define OTA_HTTP_RX_BUFFER 4096
#define OTA_HTTP_TX_BUFFER 4096
#define OTA_HTTP_TIMEOUT_MS 15000
#define OTA_TASK_STACK 8192
/* Leaves time for the BLE client to read the final status before the reset. */
#define OTA_REBOOT_DELAY_MS 3000

/* A phone that has finished sending simply stops writing, so a session that
 * goes quiet for this long is abandoned and its flash handle released. */
#define OTA_BLE_IDLE_TIMEOUT_MS 30000

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static ota_status_t s_status = {.state = OTA_STATE_IDLE, .percent = -1};
static bool s_pending_verify = false;
static bool s_busy = false;

/* A firmware image being pushed over BLE. Only the BLE host task writes to it
 * while a session is open, and the application task ends or times it out
 * between chunks, so no extra locking is needed beyond the status fields. */
static struct {
    bool active;
    esp_ota_handle_t handle;
    uint32_t size;      /* image size the phone announced */
    uint32_t received;  /* bytes written so far; also the next expected offset */
    uint32_t crc32;     /* CRC32 the phone announced */
    uint32_t running_crc32;
    TickType_t last_activity;
} s_ble;

typedef struct {
    char url[160];
    bool force;
} ota_request_t;

static void set_status(ota_state_t state, int percent, const char *error)
{
    portENTER_CRITICAL(&s_lock);
    s_status.state = state;
    s_status.percent = percent;
    s_status.error = error;
    portEXIT_CRITICAL(&s_lock);
}

static void set_version(const char *version)
{
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_status.version, version, sizeof(s_status.version));
    portEXIT_CRITICAL(&s_lock);
}

static void finish_task(void)
{
    portENTER_CRITICAL(&s_lock);
    s_busy = false;
    portEXIT_CRITICAL(&s_lock);
    vTaskDelete(NULL);
}

static void ota_task(void *arg)
{
    ota_request_t *req = arg;
    const char *error = NULL;

    ESP_LOGI(TAG, "Downloading %s (free heap %lu)", req->url, (unsigned long)esp_get_free_heap_size());

    esp_http_client_config_t http_cfg = {
        .url = req->url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = OTA_HTTP_RX_BUFFER,
        .buffer_size_tx = OTA_HTTP_TX_BUFFER,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .max_redirection_count = 5,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_https_ota_begin failed: %s", esp_err_to_name(err));
        error = err == ESP_ERR_NO_MEM ? "out of memory" : "could not download the release";
        goto fail;
    }

    esp_app_desc_t new_desc;
    err = esp_https_ota_get_img_desc(handle, &new_desc);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_https_ota_get_img_desc failed: %s", esp_err_to_name(err));
        error = "downloaded file is not a firmware image";
        esp_https_ota_abort(handle);
        goto fail;
    }
    set_version(new_desc.version);
    ESP_LOGI(TAG, "Release version %s, running %s", new_desc.version, ota_running_version());

    if (!req->force && strncmp(new_desc.version, ota_running_version(), sizeof(new_desc.version)) == 0)
    {
        ESP_LOGI(TAG, "Already running %s; nothing to install", new_desc.version);
        esp_https_ota_abort(handle);
        set_status(OTA_STATE_UP_TO_DATE, -1, NULL);
        free(req);
        finish_task();
        return;
    }

    int image_size = esp_https_ota_get_image_size(handle);
    int last_percent = -1;
    while (1)
    {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS)
        {
            break;
        }
        if (image_size > 0)
        {
            int percent = (int)(100LL * esp_https_ota_get_image_len_read(handle) / image_size);
            if (percent != last_percent)
            {
                last_percent = percent;
                set_status(OTA_STATE_DOWNLOADING, percent, NULL);
            }
        }
    }

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_https_ota_perform failed: %s", esp_err_to_name(err));
        error = "download interrupted";
        esp_https_ota_abort(handle);
        goto fail;
    }
    if (!esp_https_ota_is_complete_data_received(handle))
    {
        ESP_LOGE(TAG, "Image incomplete");
        error = "download incomplete";
        esp_https_ota_abort(handle);
        goto fail;
    }

    /* Validates the image and selects it for the next boot. */
    err = esp_https_ota_finish(handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_https_ota_finish failed: %s", esp_err_to_name(err));
        error = err == ESP_ERR_OTA_VALIDATE_FAILED ? "image failed validation" : "could not activate the image";
        goto fail;
    }

    ESP_LOGI(TAG, "Update to %s installed; rebooting", new_desc.version);
    set_status(OTA_STATE_REBOOTING, 100, NULL);
    free(req);
    vTaskDelay(pdMS_TO_TICKS(OTA_REBOOT_DELAY_MS));
    esp_restart();

fail:
    set_status(OTA_STATE_FAILED, -1, error);
    free(req);
    finish_task();
}

void ota_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running != NULL && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY)
    {
        ESP_LOGW(TAG, "Running %s from %s for the first time; awaiting confirmation",
                 ota_running_version(), running->label);
        s_pending_verify = true;
        set_status(OTA_STATE_VERIFYING, -1, NULL);
    }
    else
    {
        ESP_LOGI(TAG, "Running %s from %s", ota_running_version(), running ? running->label : "?");
    }
}

const char *ota_running_version(void)
{
    return esp_app_get_description()->version;
}

bool ota_is_pending_verify(void)
{
    return s_pending_verify;
}

void ota_confirm_if_pending(void)
{
    if (!s_pending_verify)
    {
        return;
    }
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Could not confirm the running image: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "Firmware %s confirmed", ota_running_version());
    s_pending_verify = false;
    set_status(OTA_STATE_IDLE, -1, NULL);
}

void ota_rollback_if_pending(void)
{
    if (!s_pending_verify)
    {
        return;
    }
    ESP_LOGE(TAG, "Firmware %s did not pass its checks; rolling back", ota_running_version());
    esp_ota_mark_app_invalid_rollback_and_reboot();
    /* Only returns if there is no previous image to go back to. */
    ESP_LOGE(TAG, "No previous image to roll back to; keeping %s", ota_running_version());
    ota_confirm_if_pending();
}

static bool valid_tag(const char *tag)
{
    size_t len = strlen(tag);
    if (len == 0 || len >= OTA_TAG_MAX_LEN)
    {
        return false;
    }
    for (size_t i = 0; i < len; ++i)
    {
        char c = tag[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '-' || c == '_';
        if (!ok)
        {
            return false;
        }
    }
    return true;
}

bool ota_start(const char *tag, bool force, const char **error)
{
    if (!valid_tag(tag))
    {
        *error = "invalid release tag";
        return false;
    }
    /* esp_ota_begin refuses to write while the running image is unconfirmed. */
    if (s_pending_verify)
    {
        *error = "current firmware not confirmed yet; try again shortly";
        return false;
    }

    portENTER_CRITICAL(&s_lock);
    bool busy = s_busy;
    s_busy = true;
    portEXIT_CRITICAL(&s_lock);
    if (busy)
    {
        *error = "an update is already running";
        return false;
    }

    ota_request_t *req = calloc(1, sizeof(*req));
    if (req == NULL)
    {
        *error = "out of memory";
        goto fail;
    }
    if (strcmp(tag, "latest") == 0)
    {
        snprintf(req->url, sizeof(req->url), "%s/latest/download/%s", OTA_RELEASE_BASE_URL, OTA_RELEASE_ASSET);
    }
    else
    {
        snprintf(req->url, sizeof(req->url), "%s/download/%s/%s", OTA_RELEASE_BASE_URL, tag, OTA_RELEASE_ASSET);
    }
    req->force = force;

    set_version("");
    set_status(OTA_STATE_DOWNLOADING, 0, NULL);
    if (xTaskCreate(ota_task, "ota", OTA_TASK_STACK, req, 2, NULL) != pdPASS)
    {
        free(req);
        set_status(OTA_STATE_FAILED, -1, "could not start the update task");
        *error = "could not start the update task";
        goto fail;
    }
    return true;

fail:
    portENTER_CRITICAL(&s_lock);
    s_busy = false;
    portEXIT_CRITICAL(&s_lock);
    return false;
}

static void ota_ble_fail(const char *error)
{
    if (s_ble.active)
    {
        esp_ota_abort(s_ble.handle);
        s_ble.active = false;
    }
    portENTER_CRITICAL(&s_lock);
    s_busy = false;
    portEXIT_CRITICAL(&s_lock);
    set_status(OTA_STATE_FAILED, -1, error);
}

bool ota_ble_begin(uint32_t size, uint32_t crc32, const char *version, bool force, const char **error)
{
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL)
    {
        *error = "no OTA partition available";
        return false;
    }
    if (size == 0 || size > target->size)
    {
        *error = "image does not fit in the OTA partition";
        return false;
    }
    if (s_pending_verify)
    {
        *error = "current firmware not confirmed yet; try again shortly";
        return false;
    }
    if (!force && version != NULL && version[0] != '\0' &&
        strncmp(version, ota_running_version(), OTA_VERSION_LEN) == 0)
    {
        set_version(version);
        set_status(OTA_STATE_UP_TO_DATE, -1, NULL);
        *error = "already running that version";
        return false;
    }

    /* A stale session from an interrupted transfer must not hold the flash
     * handle, nor the busy flag the check below looks at. */
    ota_ble_abort();

    portENTER_CRITICAL(&s_lock);
    bool busy = s_busy;
    s_busy = true;
    portEXIT_CRITICAL(&s_lock);
    if (busy)
    {
        *error = "an update is already running";
        return false;
    }

    esp_err_t err = esp_ota_begin(target, size, &s_ble.handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        portENTER_CRITICAL(&s_lock);
        s_busy = false;
        portEXIT_CRITICAL(&s_lock);
        *error = "could not open the OTA partition";
        return false;
    }

    s_ble.active = true;
    s_ble.size = size;
    s_ble.received = 0;
    s_ble.crc32 = crc32;
    s_ble.running_crc32 = 0;
    s_ble.last_activity = xTaskGetTickCount();
    set_version(version != NULL ? version : "");
    set_status(OTA_STATE_RECEIVING, 0, NULL);
    ESP_LOGI(TAG, "Receiving %lu bytes over BLE into %s", (unsigned long)size, target->label);
    return true;
}

bool ota_ble_write(uint32_t offset, const uint8_t *data, size_t len)
{
    if (!s_ble.active)
    {
        return false;
    }
    if (offset != s_ble.received)
    {
        ESP_LOGE(TAG, "Chunk at %lu, expected %lu", (unsigned long)offset, (unsigned long)s_ble.received);
        ota_ble_fail("chunk out of order");
        return false;
    }
    if (len == 0 || s_ble.received + len > s_ble.size)
    {
        ota_ble_fail("more data than announced");
        return false;
    }

    esp_err_t err = esp_ota_write(s_ble.handle, data, len);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
        ota_ble_fail("could not write to flash");
        return false;
    }

    s_ble.running_crc32 = esp_rom_crc32_le(s_ble.running_crc32, data, len);
    s_ble.received += len;
    s_ble.last_activity = xTaskGetTickCount();
    set_status(OTA_STATE_RECEIVING, (int)(100ULL * s_ble.received / s_ble.size), NULL);
    return true;
}

bool ota_ble_end(const char **error)
{
    if (!s_ble.active)
    {
        *error = "no transfer in progress";
        return false;
    }
    if (s_ble.received != s_ble.size)
    {
        ESP_LOGE(TAG, "Got %lu of %lu bytes", (unsigned long)s_ble.received, (unsigned long)s_ble.size);
        *error = "transfer incomplete";
        ota_ble_fail(*error);
        return false;
    }
    if (s_ble.running_crc32 != s_ble.crc32)
    {
        ESP_LOGE(TAG, "CRC32 %08lx, expected %08lx", (unsigned long)s_ble.running_crc32,
                 (unsigned long)s_ble.crc32);
        *error = "checksum mismatch";
        ota_ble_fail(*error);
        return false;
    }

    esp_err_t err = esp_ota_end(s_ble.handle);
    s_ble.active = false;
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        *error = err == ESP_ERR_OTA_VALIDATE_FAILED ? "image failed validation" : "could not finish the update";
        ota_ble_fail(*error);
        return false;
    }

    err = esp_ota_set_boot_partition(esp_ota_get_next_update_partition(NULL));
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        *error = "could not activate the image";
        ota_ble_fail(*error);
        return false;
    }

    ESP_LOGI(TAG, "Image received over BLE; rebooting");
    set_status(OTA_STATE_REBOOTING, 100, NULL);
    return true;
}

void ota_ble_abort(void)
{
    if (!s_ble.active)
    {
        return;
    }
    ESP_LOGW(TAG, "BLE transfer aborted after %lu bytes", (unsigned long)s_ble.received);
    esp_ota_abort(s_ble.handle);
    s_ble.active = false;
    portENTER_CRITICAL(&s_lock);
    s_busy = false;
    portEXIT_CRITICAL(&s_lock);
    set_status(OTA_STATE_IDLE, -1, NULL);
}

void ota_ble_tick(void)
{
    if (!s_ble.active)
    {
        return;
    }
    if ((xTaskGetTickCount() - s_ble.last_activity) >= pdMS_TO_TICKS(OTA_BLE_IDLE_TIMEOUT_MS))
    {
        ESP_LOGW(TAG, "BLE transfer stalled at %lu of %lu bytes",
                 (unsigned long)s_ble.received, (unsigned long)s_ble.size);
        ota_ble_fail("transfer stalled");
    }
}

void ota_get_status(ota_status_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_lock);
}

const char *ota_state_name(ota_state_t state)
{
    switch (state)
    {
    case OTA_STATE_VERIFYING:
        return "verifying";
    case OTA_STATE_DOWNLOADING:
        return "downloading";
    case OTA_STATE_RECEIVING:
        return "receiving";
    case OTA_STATE_UP_TO_DATE:
        return "uptodate";
    case OTA_STATE_REBOOTING:
        return "rebooting";
    case OTA_STATE_FAILED:
        return "failed";
    case OTA_STATE_IDLE:
    default:
        return "idle";
    }
}
