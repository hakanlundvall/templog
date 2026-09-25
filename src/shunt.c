#include "shunt.h"

#include <math.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "shunt";

/* The two outputs that used to drive the diagnostic LEDs. */
#define SHUNT_GPIO_WARMER GPIO_NUM_18
#define SHUNT_GPIO_COLDER GPIO_NUM_19

/* Level that energises a relay. Flip to 0 for an active low relay board. */
#define SHUNT_ACTIVE_LEVEL 1

#define SHUNT_TICK_MS 100
/* A supply or outdoor reading older than this is treated as missing. Twice the
 * timeout the sampling loop uses before it cuts the heater, so a single slow
 * round of conversions does not stop the valve. */
#define SHUNT_SENSOR_STALE_MS 120000
#define SHUNT_MAX_JOG_MS 300000

#define SHUNT_DEFAULT_SLOPE 1.0f
#define SHUNT_DEFAULT_OFFSET_C 0.0f
#define SHUNT_DEFAULT_ROOM_TARGET_C 21.0f
#define SHUNT_DEFAULT_MIN_SUPPLY_C 20.0f
#define SHUNT_DEFAULT_MAX_SUPPLY_C 70.0f
/* Short enough that one burst cannot move the mixer far, and paused long
 * enough for a DS18B20 clamped to the pipe to show what that burst did. */
#define SHUNT_DEFAULT_BURST_MS 1000
#define SHUNT_DEFAULT_PAUSE_S 10
#define SHUNT_DEFAULT_TOLERANCE_C 1.0f
#define SHUNT_DEFAULT_INDOOR_GAIN 3.0f
#define SHUNT_DEFAULT_INDOOR_MAX_C 5.0f
#define SHUNT_DEFAULT_INDOOR_STALE_S 900

static nvs_handle_t s_nvs;
static SemaphoreHandle_t s_mutex;

static shunt_config_t s_cfg = {
    .enabled = false,
    .slope = SHUNT_DEFAULT_SLOPE,
    .offset_c = SHUNT_DEFAULT_OFFSET_C,
    .room_target_c = SHUNT_DEFAULT_ROOM_TARGET_C,
    .min_supply_c = SHUNT_DEFAULT_MIN_SUPPLY_C,
    .max_supply_c = SHUNT_DEFAULT_MAX_SUPPLY_C,
    .burst_ms = SHUNT_DEFAULT_BURST_MS,
    .pause_s = SHUNT_DEFAULT_PAUSE_S,
    .tolerance_c = SHUNT_DEFAULT_TOLERANCE_C,
    .indoor_gain = SHUNT_DEFAULT_INDOOR_GAIN,
    .indoor_max_c = SHUNT_DEFAULT_INDOOR_MAX_C,
    .indoor_stale_s = SHUNT_DEFAULT_INDOOR_STALE_S,
};

/* Inputs, each with the tick it arrived on so staleness needs no bookkeeping
 * from the callers. */
static float s_supply_c = NAN;
static TickType_t s_supply_tick;
static float s_outdoor_c = NAN;
static TickType_t s_outdoor_tick;
static float s_indoor_c = NAN;
static TickType_t s_indoor_tick;

/* Control state. */
static shunt_dir_t s_dir = SHUNT_DIR_IDLE;
static shunt_state_t s_state = SHUNT_STATE_OFF;
static const char *s_reason = "control is switched off";
static TickType_t s_burst_end;
/* Nothing is decided before this: the pause after a burst is what lets the
 * supply temperature answer it. */
static TickType_t s_settled_at;
static uint32_t s_jog_remaining_ms;
static shunt_dir_t s_jog_dir = SHUNT_DIR_IDLE;
static float s_setpoint_c = NAN;
static float s_trim_c = 0.0f;
/* Consecutive bursts in one direction; see shunt_status_t. */
static int16_t s_bursts = 0;

static float clampf(float value, float low, float high)
{
    if (value < low) {
        return low;
    }
    return value > high ? high : value;
}

static uint32_t age_ms(TickType_t stamp, bool ever)
{
    if (!ever) {
        return UINT32_MAX;
    }
    return (uint32_t)((xTaskGetTickCount() - stamp) * portTICK_PERIOD_MS);
}

static float nvs_get_float(const char *key, float fallback)
{
    int32_t centi = 0;
    if (nvs_get_i32(s_nvs, key, &centi) == ESP_OK) {
        return centi / 100.0f;
    }
    return fallback;
}

static void nvs_put_float(const char *key, float value)
{
    nvs_set_i32(s_nvs, key, (int32_t)lroundf(value * 100.0f));
}

const char *shunt_state_name(shunt_state_t state)
{
    switch (state) {
    case SHUNT_STATE_RUNNING:
        return "running";
    case SHUNT_STATE_HOLDING:
        return "holding";
    case SHUNT_STATE_MANUAL:
        return "manual";
    case SHUNT_STATE_OFF:
    default:
        return "off";
    }
}

const char *shunt_dir_name(shunt_dir_t dir)
{
    switch (dir) {
    case SHUNT_DIR_WARMER:
        return "warmer";
    case SHUNT_DIR_COLDER:
        return "colder";
    case SHUNT_DIR_IDLE:
    default:
        return "idle";
    }
}

/* The only place the outputs are touched. Reversing always goes through idle
 * for at least one tick, so the motor is never driven both ways at once even
 * if the relays are slow to drop. */
static void set_outputs(shunt_dir_t dir)
{
    if (dir == s_dir) {
        return;
    }
    if (dir != SHUNT_DIR_IDLE && s_dir != SHUNT_DIR_IDLE) {
        dir = SHUNT_DIR_IDLE;
    }
    gpio_set_level(SHUNT_GPIO_WARMER,
                   dir == SHUNT_DIR_WARMER ? SHUNT_ACTIVE_LEVEL : !SHUNT_ACTIVE_LEVEL);
    gpio_set_level(SHUNT_GPIO_COLDER,
                   dir == SHUNT_DIR_COLDER ? SHUNT_ACTIVE_LEVEL : !SHUNT_ACTIVE_LEVEL);
    s_dir = dir;
}

/* Turns the outdoor temperature into a supply temperature setpoint, the way
 * the curve chart on the old panel does. Returns NAN, with *reason set, when
 * there is nothing to compute it from. */
static float compute_setpoint(const char **reason)
{
    uint32_t outdoor_age = age_ms(s_outdoor_tick, !isnan(s_outdoor_c));
    if (outdoor_age == UINT32_MAX) {
        *reason = "no outdoor sensor assigned or read yet";
        return NAN;
    }
    if (outdoor_age > SHUNT_SENSOR_STALE_MS) {
        *reason = "the outdoor reading is stale";
        return NAN;
    }

    float setpoint = s_cfg.room_target_c +
                     s_cfg.slope * (s_cfg.room_target_c - s_outdoor_c) +
                     s_cfg.offset_c;

    /* The indoor reading comes from MQTT, so it can go missing for reasons
     * that have nothing to do with the house: it only trims the curve while
     * it is fresh, and the curve alone carries the heating otherwise. */
    uint32_t indoor_age = age_ms(s_indoor_tick, !isnan(s_indoor_c));
    bool indoor_fresh = indoor_age != UINT32_MAX &&
                        indoor_age <= (uint32_t)s_cfg.indoor_stale_s * 1000;
    if (indoor_fresh) {
        s_trim_c = clampf(s_cfg.indoor_gain * (s_cfg.room_target_c - s_indoor_c),
                          -s_cfg.indoor_max_c, s_cfg.indoor_max_c);
        setpoint += s_trim_c;
    } else {
        s_trim_c = 0.0f;
    }

    return clampf(setpoint, s_cfg.min_supply_c, s_cfg.max_supply_c);
}

static void run_jog(void)
{
    s_state = SHUNT_STATE_MANUAL;
    s_reason = NULL;
    set_outputs(s_jog_dir);
    /* Only count time the output was actually energised: the first tick of a
     * reversal is spent idle by set_outputs(). */
    if (s_dir != s_jog_dir) {
        return;
    }
    s_jog_remaining_ms = s_jog_remaining_ms > SHUNT_TICK_MS ? s_jog_remaining_ms - SHUNT_TICK_MS : 0;
    if (s_jog_remaining_ms == 0) {
        set_outputs(SHUNT_DIR_IDLE);
        s_jog_dir = SHUNT_DIR_IDLE;
        s_bursts = 0;
        /* Let the supply temperature answer the jog before correcting it. */
        s_settled_at = xTaskGetTickCount() + pdMS_TO_TICKS((uint32_t)s_cfg.pause_s * 1000);
    }
}

static void run_auto(void)
{
    TickType_t now = xTaskGetTickCount();

    if (s_dir != SHUNT_DIR_IDLE) {
        /* Signed difference, so the comparison still holds when the tick
         * counter wraps mid-burst. */
        if ((int32_t)(now - s_burst_end) >= 0) {
            set_outputs(SHUNT_DIR_IDLE);
            s_settled_at = now + pdMS_TO_TICKS((uint32_t)s_cfg.pause_s * 1000);
        }
        return;
    }

    /* The pause after a burst. Deciding anything before the supply sensor has
     * answered the last one is how a valve like this ends up hunting. */
    if ((int32_t)(now - s_settled_at) < 0) {
        return;
    }

    const char *reason = NULL;
    float setpoint = compute_setpoint(&reason);
    s_setpoint_c = setpoint;
    if (isnan(setpoint)) {
        s_state = SHUNT_STATE_HOLDING;
        s_reason = reason;
        return;
    }

    uint32_t supply_age = age_ms(s_supply_tick, !isnan(s_supply_c));
    if (supply_age == UINT32_MAX) {
        s_state = SHUNT_STATE_HOLDING;
        s_reason = "no supply sensor assigned or read yet";
        return;
    }
    if (supply_age > SHUNT_SENSOR_STALE_MS) {
        /* Without a supply reading there is no way to know when to stop, so
         * the valve stays where it is rather than being driven blind. */
        s_state = SHUNT_STATE_HOLDING;
        s_reason = "the supply reading is stale";
        return;
    }

    s_state = SHUNT_STATE_RUNNING;
    s_reason = NULL;

    float error = setpoint - s_supply_c;
    if (fabsf(error) <= s_cfg.tolerance_c) {
        s_bursts = 0;
        return;
    }

    /* One short burst, always the same length. How much supply temperature a
     * given movement is worth depends on how hot the boiler side of the mixer
     * is, so there is nothing to compute a proportional burst from; the next
     * reading decides whether another one is needed. */
    shunt_dir_t dir = error > 0 ? SHUNT_DIR_WARMER : SHUNT_DIR_COLDER;
    if (dir == SHUNT_DIR_WARMER) {
        s_bursts = s_bursts > 0 && s_bursts < INT16_MAX ? s_bursts + 1 : 1;
    } else {
        s_bursts = s_bursts < 0 && s_bursts > INT16_MIN ? s_bursts - 1 : -1;
    }

    s_burst_end = now + pdMS_TO_TICKS(s_cfg.burst_ms);
    set_outputs(dir);
    ESP_LOGI(TAG, "supply %.1f -> %.1f C, burst %s (%d in a row)",
             (double)s_supply_c, (double)setpoint, shunt_dir_name(dir), (int)s_bursts);
}

static void shunt_tick(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_jog_remaining_ms > 0) {
        run_jog();
    } else if (!s_cfg.enabled) {
        set_outputs(SHUNT_DIR_IDLE);
        s_state = SHUNT_STATE_OFF;
        s_reason = "control is switched off";
        s_setpoint_c = NAN;
        s_bursts = 0;
    } else {
        run_auto();
    }

    xSemaphoreGive(s_mutex);
}

static _Noreturn void shunt_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();
    while (1) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SHUNT_TICK_MS));
        shunt_tick();
    }
}

static void load_config(void)
{
    uint8_t enabled = 0;
    if (nvs_get_u8(s_nvs, "shEn", &enabled) == ESP_OK) {
        s_cfg.enabled = enabled != 0;
    }
    s_cfg.slope = nvs_get_float("shSlope", SHUNT_DEFAULT_SLOPE);
    s_cfg.offset_c = nvs_get_float("shOff", SHUNT_DEFAULT_OFFSET_C);
    s_cfg.room_target_c = nvs_get_float("shTarget", SHUNT_DEFAULT_ROOM_TARGET_C);
    s_cfg.min_supply_c = nvs_get_float("shMin", SHUNT_DEFAULT_MIN_SUPPLY_C);
    s_cfg.max_supply_c = nvs_get_float("shMax", SHUNT_DEFAULT_MAX_SUPPLY_C);
    s_cfg.tolerance_c = nvs_get_float("shTol", SHUNT_DEFAULT_TOLERANCE_C);
    s_cfg.indoor_gain = nvs_get_float("shIGain", SHUNT_DEFAULT_INDOOR_GAIN);
    s_cfg.indoor_max_c = nvs_get_float("shIMax", SHUNT_DEFAULT_INDOOR_MAX_C);

    uint16_t burst = 0;
    if (nvs_get_u16(s_nvs, "shBurst", &burst) == ESP_OK && burst > 0) {
        s_cfg.burst_ms = burst;
    }
    uint16_t pause = 0;
    if (nvs_get_u16(s_nvs, "shPause", &pause) == ESP_OK && pause > 0) {
        s_cfg.pause_s = pause;
    }
    uint16_t stale = 0;
    if (nvs_get_u16(s_nvs, "shIStale", &stale) == ESP_OK && stale > 0) {
        s_cfg.indoor_stale_s = stale;
    }
}

void shunt_init(nvs_handle_t nvs)
{
    s_nvs = nvs;
    s_mutex = xSemaphoreCreateMutex();

    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << SHUNT_GPIO_WARMER) | (1ULL << SHUNT_GPIO_COLDER),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    gpio_config(&io_conf);
    gpio_set_level(SHUNT_GPIO_WARMER, !SHUNT_ACTIVE_LEVEL);
    gpio_set_level(SHUNT_GPIO_COLDER, !SHUNT_ACTIVE_LEVEL);

    load_config();
    s_settled_at = xTaskGetTickCount();
    ESP_LOGI(TAG, "curve: %.1f C at target, slope %.2f, offset %.1f, limits %.0f-%.0f C, %s",
             (double)s_cfg.room_target_c, (double)s_cfg.slope, (double)s_cfg.offset_c,
             (double)s_cfg.min_supply_c, (double)s_cfg.max_supply_c,
             s_cfg.enabled ? "enabled" : "disabled");

    xTaskCreate(shunt_task, "shunt", 3072, NULL, 6, NULL);
}

void shunt_get_config(shunt_config_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_mutex);
}

/* Takes the incoming value unless it is the "leave unchanged" marker. */
static float pick_float(float incoming, float current)
{
    return isnan(incoming) ? current : incoming;
}

static uint16_t pick_u16(uint16_t incoming, uint16_t current)
{
    return incoming == 0 ? current : incoming;
}

static bool in_range(float value, float low, float high)
{
    return value >= low && value <= high;
}

bool shunt_set_config(const shunt_config_t *in, const char **error)
{
    shunt_config_t next;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    next = s_cfg;
    xSemaphoreGive(s_mutex);

    next.enabled = in->enabled;
    next.slope = pick_float(in->slope, next.slope);
    next.offset_c = pick_float(in->offset_c, next.offset_c);
    next.room_target_c = pick_float(in->room_target_c, next.room_target_c);
    next.min_supply_c = pick_float(in->min_supply_c, next.min_supply_c);
    next.max_supply_c = pick_float(in->max_supply_c, next.max_supply_c);
    next.tolerance_c = pick_float(in->tolerance_c, next.tolerance_c);
    next.indoor_gain = pick_float(in->indoor_gain, next.indoor_gain);
    next.indoor_max_c = pick_float(in->indoor_max_c, next.indoor_max_c);
    next.burst_ms = pick_u16(in->burst_ms, next.burst_ms);
    next.pause_s = pick_u16(in->pause_s, next.pause_s);
    next.indoor_stale_s = pick_u16(in->indoor_stale_s, next.indoor_stale_s);

    if (!in_range(next.slope, 0.0f, 10.0f)) {
        *error = "slope must be between 0 and 10";
        return false;
    }
    if (!in_range(next.offset_c, -20.0f, 20.0f)) {
        *error = "offset must be between -20 and 20 C";
        return false;
    }
    if (!in_range(next.room_target_c, 5.0f, 30.0f)) {
        *error = "room target must be between 5 and 30 C";
        return false;
    }
    if (!in_range(next.min_supply_c, 10.0f, 90.0f) || !in_range(next.max_supply_c, 10.0f, 95.0f)) {
        *error = "supply limits must be between 10 and 95 C";
        return false;
    }
    if (next.min_supply_c >= next.max_supply_c) {
        *error = "the minimum supply temperature must be below the maximum";
        return false;
    }
    if (next.burst_ms < 100 || next.burst_ms > 30000) {
        *error = "burst length must be between 100 and 30000 ms";
        return false;
    }
    if (next.pause_s < 1 || next.pause_s > 600) {
        *error = "pause must be between 1 and 600 s";
        return false;
    }
    if (!in_range(next.tolerance_c, 0.1f, 10.0f)) {
        *error = "tolerance must be between 0.1 and 10 C";
        return false;
    }
    if (!in_range(next.indoor_gain, 0.0f, 10.0f)) {
        *error = "indoor gain must be between 0 and 10";
        return false;
    }
    if (!in_range(next.indoor_max_c, 0.0f, 20.0f)) {
        *error = "indoor trim limit must be between 0 and 20 C";
        return false;
    }
    if (next.indoor_stale_s < 60 || next.indoor_stale_s > 86400) {
        *error = "indoor staleness limit must be between 60 and 86400 s";
        return false;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool was_enabled = s_cfg.enabled;
    s_cfg = next;
    if (!next.enabled && was_enabled) {
        set_outputs(SHUNT_DIR_IDLE);
        s_jog_remaining_ms = 0;
    }
    xSemaphoreGive(s_mutex);

    nvs_set_u8(s_nvs, "shEn", next.enabled ? 1 : 0);
    nvs_put_float("shSlope", next.slope);
    nvs_put_float("shOff", next.offset_c);
    nvs_put_float("shTarget", next.room_target_c);
    nvs_put_float("shMin", next.min_supply_c);
    nvs_put_float("shMax", next.max_supply_c);
    nvs_put_float("shTol", next.tolerance_c);
    nvs_put_float("shIGain", next.indoor_gain);
    nvs_put_float("shIMax", next.indoor_max_c);
    nvs_set_u16(s_nvs, "shBurst", next.burst_ms);
    nvs_set_u16(s_nvs, "shPause", next.pause_s);
    nvs_set_u16(s_nvs, "shIStale", next.indoor_stale_s);
    esp_err_t ret = nvs_commit(s_nvs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist the shunt configuration: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "configuration updated: %s, target %.1f C, slope %.2f, offset %.1f",
             next.enabled ? "enabled" : "disabled", (double)next.room_target_c,
             (double)next.slope, (double)next.offset_c);
    return true;
}

void shunt_get_status(shunt_status_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    out->state = s_state;
    out->dir = s_dir;
    out->setpoint_c = s_setpoint_c;
    out->supply_c = s_supply_c;
    out->outdoor_c = s_outdoor_c;
    out->indoor_c = s_indoor_c;
    out->supply_age_ms = age_ms(s_supply_tick, !isnan(s_supply_c));
    out->outdoor_age_ms = age_ms(s_outdoor_tick, !isnan(s_outdoor_c));
    out->indoor_age_ms = age_ms(s_indoor_tick, !isnan(s_indoor_c));
    /* Worked out here rather than taken from the last control cycle, so that
     * a reading which has gone stale is reported as such even while the
     * controller is switched off or holding for some other reason. */
    out->indoor_fresh = out->indoor_age_ms != UINT32_MAX &&
                        out->indoor_age_ms <= (uint32_t)s_cfg.indoor_stale_s * 1000;
    out->indoor_trim_c = out->indoor_fresh ? s_trim_c : 0.0f;
    out->bursts = s_bursts;
    out->reason = s_reason;
    xSemaphoreGive(s_mutex);
}

void shunt_report_supply(float celsius)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_supply_c = celsius;
    s_supply_tick = xTaskGetTickCount();
    xSemaphoreGive(s_mutex);
}

void shunt_report_outdoor(float celsius)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_outdoor_c = celsius;
    s_outdoor_tick = xTaskGetTickCount();
    xSemaphoreGive(s_mutex);
}

void shunt_report_indoor(float celsius)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_indoor_c = celsius;
    s_indoor_tick = xTaskGetTickCount();
    xSemaphoreGive(s_mutex);
}

bool shunt_jog(shunt_dir_t dir, uint32_t ms, const char **error)
{
    if (dir != SHUNT_DIR_WARMER && dir != SHUNT_DIR_COLDER) {
        *error = "direction must be warmer or colder";
        return false;
    }
    if (ms == 0 || ms > SHUNT_MAX_JOG_MS) {
        *error = "jog time must be between 1 and 300000 ms";
        return false;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_jog_dir = dir;
    s_jog_remaining_ms = ms;
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "jogging %s for %u ms", shunt_dir_name(dir), (unsigned)ms);
    return true;
}
