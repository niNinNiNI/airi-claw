#pragma once

#include "servo_mcpwm.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MOTION_FIXED,
    MOTION_SWEEP,
    MOTION_EASE,
} motion_mode_t;

typedef enum {
    EASE_LINEAR,
    EASE_IN_QUAD,
    EASE_OUT_QUAD,
    EASE_IN_OUT_QUAD,
    EASE_IN_OUT_CUBIC,
} ease_type_t;

typedef struct {
    motion_mode_t mode;
    double angle;
    double min_angle;
    double max_angle;
    double step;
    uint32_t step_delay_ms;
    ease_type_t ease_type;
} servo_action_t;

typedef struct {
    uint8_t id;
    uint32_t duration_ms;
    servo_action_t servo[2];
} motion_t;

/**
 * Run motion table once and return. Safe to call from a FreeRTOS task.
 * Checks s_motion_stop_requested flag each frame; call motion_player_stop()
 * from another context to abort early.
 */
void motion_player_run_once(servo_mcpwm_t *servo, const motion_t *table, int count);

/**
 * Run motion table in a loop until stopped. Never returns.
 */
void motion_player_run_loop(servo_mcpwm_t *servo, const motion_t *table, int count);

/**
 * Signal the currently running motion player to stop at the next frame
 * boundary. Thread-safe — can be called from any task.
 */
void motion_player_stop(void);

/**
 * Returns true if a motion player is currently executing.
 */
bool motion_player_is_running(void);

/**
 * Reset the stop flag so a new motion sequence can start.
 */
void motion_player_reset(void);

typedef struct {
    servo_mcpwm_t *servo;
    motion_t *table;
    int count;
    bool loop;
} motion_task_args_t;

#ifdef __cplusplus
}
#endif
