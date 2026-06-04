#include "motion.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "motion";

static volatile bool s_stop_requested = false;
static volatile bool s_running = false;

static inline double clamp(double val, double lo, double hi)
{
    if (val < lo) return lo;
    if (val > hi) return hi;
    return val;
}

static double apply_ease(ease_type_t type, double t)
{
    switch (type) {
    case EASE_LINEAR:
        return t;
    case EASE_IN_QUAD:
        return t * t;
    case EASE_OUT_QUAD:
        return t * (2.0 - t);
    case EASE_IN_OUT_QUAD:
        if (t < 0.5)
            return 2.0 * t * t;
        return -1.0 + (4.0 - 2.0 * t) * t;
    case EASE_IN_OUT_CUBIC:
        if (t < 0.5)
            return 4.0 * t * t * t;
        {
            double u = -2.0 * t + 2.0;
            return 1.0 - u * u * u / 2.0;
        }
    default:
        return t;
    }
}

static void motion_player_run_internal(servo_mcpwm_t *servo, const motion_t *table,
                                       int count, bool loop)
{
    double sweep_angle[2] = {0.0, 0.0};
    int sweep_dir[2] = {1, 1};
    double current[2] = {0.0, 0.0};

    s_running = true;

    for (int idx = 0; !s_stop_requested; idx++) {
        if (idx >= count) {
            if (!loop) break;
            idx = 0;
        }

        const motion_t *m = &table[idx];
        TickType_t start = xTaskGetTickCount();

        double ease_start[2];
        for (int ch = 0; ch < 2; ch++) {
            if (m->servo[ch].mode == MOTION_SWEEP) {
                sweep_dir[ch] = 1;
            }
            if (m->servo[ch].mode == MOTION_EASE) {
                ease_start[ch] = current[ch];
            }
        }

        ESP_LOGD(TAG, "motion #%d start, duration=%lums", m->id, m->duration_ms);

        /* Ensure at least one iteration so that FIXED and SWEEP modes
         * actually set an angle even when duration_ms is 0. */
        bool first_iteration = true;
        while (!s_stop_requested) {
            TickType_t now = xTaskGetTickCount();
            int64_t elapsed_ms = (now - start) * portTICK_PERIOD_MS;
            if (!first_iteration && elapsed_ms >= (int64_t)m->duration_ms) break;
            first_iteration = false;

            for (int ch = 0; ch < 2; ch++) {
                const servo_action_t *a = &m->servo[ch];
                double target;

                switch (a->mode) {
                case MOTION_FIXED:
                    target = a->angle;
                    break;

                case MOTION_SWEEP:
                    sweep_angle[ch] += sweep_dir[ch] * a->step;
                    if (sweep_angle[ch] >= a->max_angle) {
                        sweep_angle[ch] = a->max_angle;
                        sweep_dir[ch] = -1;
                    } else if (sweep_angle[ch] <= a->min_angle) {
                        sweep_angle[ch] = a->min_angle;
                        sweep_dir[ch] = 1;
                    }
                    target = sweep_angle[ch];
                    break;

                case MOTION_EASE:
                    {
                        double t = (m->duration_ms > 0)
                                 ? (double)elapsed_ms / m->duration_ms
                                 : 1.0;
                        if (t > 1.0) t = 1.0;
                        double eased = apply_ease(a->ease_type, t);
                        target = ease_start[ch] + (a->angle - ease_start[ch]) * eased;
                    }
                    break;
                }

                servo_mcpwm_set_angle(servo, ch, target);
                current[ch] = target;
            }

            uint32_t delay = UINT32_MAX;
            bool has_sweep = false;
            for (int ch = 0; ch < 2; ch++) {
                if (m->servo[ch].mode == MOTION_SWEEP) {
                    has_sweep = true;
                    if (m->servo[ch].step_delay_ms < delay) {
                        delay = m->servo[ch].step_delay_ms;
                    }
                }
            }
            if (!has_sweep) delay = 20;
            if (delay < 20) delay = 20;  /* 20ms minimum = 50Hz servo update rate */
            vTaskDelay(pdMS_TO_TICKS(delay));
        }

        for (int ch = 0; ch < 2; ch++) {
            if (m->servo[ch].mode == MOTION_EASE) {
                servo_mcpwm_set_angle(servo, ch, m->servo[ch].angle);
                current[ch] = m->servo[ch].angle;
            }
        }

        if (!s_stop_requested) {
            ESP_LOGD(TAG, "motion #%d done", m->id);
        }
    }

    if (s_stop_requested) {
        ESP_LOGI(TAG, "motion player stopped");
    }

    s_running = false;
}

void motion_player_run_once(servo_mcpwm_t *servo, const motion_t *table, int count)
{
    motion_player_run_internal(servo, table, count, false);
}

void motion_player_run_loop(servo_mcpwm_t *servo, const motion_t *table, int count)
{
    motion_player_run_internal(servo, table, count, true);
}

void motion_player_stop(void)
{
    s_stop_requested = true;
}

bool motion_player_is_running(void)
{
    return s_running;
}

void motion_player_reset(void)
{
    s_stop_requested = false;
    s_running = false;
}
