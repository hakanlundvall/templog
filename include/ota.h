#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Firmware releases are fetched from this repository's GitHub Releases. Only
 * the tag is chosen at runtime, so a BLE client cannot point the device at an
 * arbitrary server. */
#define OTA_RELEASE_BASE_URL "https://github.com/hakanlundvall/templog/releases"
#define OTA_RELEASE_ASSET "firmware.bin"
#define OTA_TAG_MAX_LEN 32 /* including the terminator */
#define OTA_VERSION_LEN 32 /* esp_app_desc_t.version, including the terminator */

typedef enum {
    OTA_STATE_IDLE = 0,
    /* The running image was just installed and has not been confirmed yet. */
    OTA_STATE_VERIFYING,
    OTA_STATE_DOWNLOADING,
    /* An image is arriving over BLE from a phone instead of over Wi-Fi. */
    OTA_STATE_RECEIVING,
    /* The release is the version already running; nothing was written. */
    OTA_STATE_UP_TO_DATE,
    /* The new image is written and selected for boot. */
    OTA_STATE_REBOOTING,
    OTA_STATE_FAILED,
} ota_state_t;

typedef struct {
    ota_state_t state;
    int percent;                          /* download progress, -1 if unknown */
    char version[OTA_VERSION_LEN];        /* version being installed, "" if not known yet */
    const char *error;                    /* static string; NULL unless FAILED */
} ota_status_t;

/* Checks whether the running image is a freshly installed update awaiting
 * confirmation. Call once at boot, before ota_confirm_if_pending(). */
void ota_init(void);

/* The version of the running firmware, from its app description. */
const char *ota_running_version(void);

/* True while the running image still has to be confirmed. */
bool ota_is_pending_verify(void);

/* Marks the running image as good so the bootloader stops considering a
 * rollback. Does nothing unless the image is pending verification. */
void ota_confirm_if_pending(void);

/* Gives up on a pending image: the bootloader goes back to the previous one.
 * Does not return if the image was pending. */
void ota_rollback_if_pending(void);

/* Starts downloading `tag` ("latest" or a release tag such as "v1.2.0") in a
 * background task. Unless `force` is set, an image whose version matches the
 * running one is not installed. Returns false with *error set if the update
 * could not be started. */
bool ota_start(const char *tag, bool force, const char **error);

void ota_get_status(ota_status_t *out);

/* Receiving an image over BLE, for a device with no usable Wi-Fi. The phone
 * downloads the release itself and pushes it here in order.
 *
 * ota_ble_begin() opens a session, chunks are written at increasing offsets,
 * and ota_ble_end() validates the image and selects it for the next boot; the
 * caller reboots. A session is dropped if a chunk arrives out of order, if the
 * CRC does not match, or if it stalls (see ota_ble_tick). */
bool ota_ble_begin(uint32_t size, uint32_t crc32, const char *version, bool force, const char **error);

/* Writes the next chunk. Returns false once the session has been dropped; the
 * reason is in the status, since a chunk write carries no reply. Called from
 * the BLE host task. */
bool ota_ble_write(uint32_t offset, const uint8_t *data, size_t len);

/* Finishes the session and selects the new image for boot. */
bool ota_ble_end(const char **error);

void ota_ble_abort(void);

/* Drops a session that has gone quiet. Call periodically. */
void ota_ble_tick(void);

const char *ota_state_name(ota_state_t state);

#ifdef __cplusplus
}
#endif
