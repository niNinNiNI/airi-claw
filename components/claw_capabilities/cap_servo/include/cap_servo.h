#pragma once

#include "esp_err.h"
#include "servo_mcpwm.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t cap_servo_register_group(void);

servo_mcpwm_t *cap_servo_get_servo(void);

/**
 * Notify the servo subsystem of user-facing activity.
 *
 * Call this whenever a new user request starts processing (e.g. from
 * claw_core's on_request_start callback). It cancels any pending idle
 * timer and stops any running idle breathing motion.
 *
 * Thread-safe; callable from any task.
 */
void cap_servo_on_activity(void);

/**
 * Notify the servo subsystem that an LLM response has completed.
 *
 * Call this from a claw_core completion observer. If idle motion is
 * enabled, a one-shot timer is started; after
 * CONFIG_APP_CLAW_SERVO_IDLE_DELAY_MS milliseconds of no activity,
 * the servo begins breathing motion.
 *
 * Thread-safe; callable from any task.
 */
void cap_servo_on_response_complete(void);

/**
 * Kick off the idle cooldown timer at boot time.
 *
 * Call this once during startup (after cap_servo is registered) to make
 * the servo enter breathing motion shortly after power-on, without
 * waiting for the first LLM conversation to finish.
 *
 * Respects the enabled/disabled state set via set_idle_motion.
 * Thread-safe; callable from any task.
 */
void cap_servo_start_idle(void);

#ifdef __cplusplus
}
#endif
