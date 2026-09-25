#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "nvs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Weather compensated control of the radiator shunt valve.
 *
 * The valve mixes the water coming back from the radiator circuit with hot
 * water from the boiler. It is turned by a three point actuator: one output
 * makes it travel towards warmer (clockwise), the other towards colder, and
 * with neither energised it stays where it is. The two are never energised at
 * once.
 *
 * Control is open loop with respect to the house: a heating curve turns the
 * outdoor temperature into a supply ("framledning") temperature setpoint,
 * exactly like the dials on the panel this replaces. The loop that is closed
 * is the one around the supply sensor.
 *
 * That loop is deliberately crude, and for a good reason: the actuator gives
 * no position feedback, and even if it did, how much supply temperature a
 * given movement is worth changes with the boiler temperature on the other
 * side of the mixer. So nothing here tries to know where the valve is or how
 * far to move it. While the supply temperature is outside a tolerance of the
 * setpoint, the actuator is driven in short bursts in the direction that
 * corrects it, with a pause between them for the supply sensor to catch up -
 * and that is what stops it overshooting.
 *
 * An indoor temperature, which arrives over MQTT rather than from a DS18B20,
 * may trim the setpoint a few degrees either way. It is ignored whenever it is
 * stale, so a dead sensor or a broker outage cannot leave the house cold. */

typedef enum {
    SHUNT_DIR_IDLE = 0,
    SHUNT_DIR_WARMER, /* clockwise: more hot water into the radiator circuit */
    SHUNT_DIR_COLDER,
} shunt_dir_t;

typedef enum {
    SHUNT_STATE_OFF = 0, /* control disabled; the valve is left alone */
    SHUNT_STATE_RUNNING, /* following the curve */
    SHUNT_STATE_HOLDING, /* enabled, but an input is missing or stale */
    SHUNT_STATE_MANUAL,  /* a jog requested from the app is running */
} shunt_state_t;

/* Tunables, all persisted in NVS. In a value passed to shunt_set_config(), a
 * NAN float or a zero integer means "leave this one as it is", so the app and
 * the CLI can change a single setting without restating the rest. */
typedef struct {
    bool enabled;
    /* Curve: supply = target + slope * (target - outdoor) + offset + trim,
     * clamped to [min_supply, max_supply]. A slope of 0 gives a flat curve at
     * the room target, 1.0 raises the supply one degree per degree of frost. */
    float slope;
    float offset_c;      /* the panel's parallel "-0+" adjustment */
    float room_target_c; /* indoor setpoint, and the point the curve pivots about */
    float min_supply_c;
    float max_supply_c;
    /* The correction: one burst this long, then a pause this long before the
     * next decision, repeated while the supply temperature is further than
     * tolerance_c from the setpoint. */
    uint16_t burst_ms;
    uint16_t pause_s;
    float tolerance_c;
    float indoor_gain;  /* supply degrees per degree of indoor error */
    float indoor_max_c; /* clamp on the trim, in either direction */
    uint16_t indoor_stale_s; /* an indoor reading older than this is ignored */
} shunt_config_t;

typedef struct {
    shunt_state_t state;
    shunt_dir_t dir; /* which output is energised right now */
    float setpoint_c;   /* NAN while it cannot be computed */
    float supply_c;     /* NAN when never read */
    float outdoor_c;    /* NAN when never read */
    float indoor_c;     /* NAN when never received */
    uint32_t supply_age_ms;  /* UINT32_MAX when never read */
    uint32_t outdoor_age_ms; /* UINT32_MAX when never read */
    uint32_t indoor_age_ms;  /* UINT32_MAX when never received */
    bool indoor_fresh;       /* false when the trim is being ignored */
    float indoor_trim_c;     /* what the indoor reading contributes to the setpoint */
    /* Bursts made in a row without the direction changing: positive towards
     * warmer, negative towards colder, zero while the supply temperature is
     * inside the tolerance. A count that keeps climbing is the sign of a valve
     * that has reached an end stop, or of a boiler that cannot deliver. */
    int16_t bursts;
    const char *reason;      /* static string; why it is holding, NULL while running */
} shunt_status_t;

/* Claims the two actuator outputs, restores the configuration from NVS and
 * starts the control task. The handle is kept, so it must outlive the call. */
void shunt_init(nvs_handle_t nvs);

void shunt_get_config(shunt_config_t *out);

/* Applies and persists the fields of `in` that are not "leave unchanged",
 * after checking them against each other. Returns false with *error set if
 * any of them is out of range. */
bool shunt_set_config(const shunt_config_t *in, const char **error);

void shunt_get_status(shunt_status_t *out);

/* Latest readings. The controller timestamps them itself, so a sensor that
 * stops reporting is noticed without the caller tracking anything. */
void shunt_report_supply(float celsius);
void shunt_report_outdoor(float celsius);
void shunt_report_indoor(float celsius);

/* Runs the actuator for `ms` in one direction, for checking which way it is
 * wired. Automatic control resumes afterwards. */
bool shunt_jog(shunt_dir_t dir, uint32_t ms, const char **error);

const char *shunt_state_name(shunt_state_t state);
const char *shunt_dir_name(shunt_dir_t dir);

#ifdef __cplusplus
}
#endif
