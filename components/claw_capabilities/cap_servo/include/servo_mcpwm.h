#pragma once

#include "driver/mcpwm_prelude.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    mcpwm_timer_handle_t timer;
    mcpwm_oper_handle_t oper;
    mcpwm_cmpr_handle_t comparator[2];
    mcpwm_gen_handle_t generator[2];
    uint32_t period_ticks;
    int gpio[2];
} servo_mcpwm_t;

esp_err_t servo_mcpwm_init(servo_mcpwm_t *servo, int gpio_a, int gpio_b);

esp_err_t servo_mcpwm_set_angle(servo_mcpwm_t *servo, int channel, double degree);

esp_err_t servo_mcpwm_deinit(servo_mcpwm_t *servo);

#ifdef __cplusplus
}
#endif
