#include "servo_mcpwm.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "servo_mcpwm";

#define SERVO_RESOLUTION_HZ  1000000U
#define SERVO_FREQUENCY_HZ   50U
#define SERVO_PERIOD_TICKS   (SERVO_RESOLUTION_HZ / SERVO_FREQUENCY_HZ)

#ifndef CONFIG_APP_CLAW_SERVO_PULSE_MIN_US
#define CONFIG_APP_CLAW_SERVO_PULSE_MIN_US 405
#endif
#ifndef CONFIG_APP_CLAW_SERVO_PULSE_MAX_US
#define CONFIG_APP_CLAW_SERVO_PULSE_MAX_US 2595
#endif
#ifndef CONFIG_APP_CLAW_SERVO_MAX_ANGLE
#define CONFIG_APP_CLAW_SERVO_MAX_ANGLE 135
#endif

static inline uint32_t angle_to_ticks(double degree)
{
    double max_angle = (double)CONFIG_APP_CLAW_SERVO_MAX_ANGLE;
    if (degree < -max_angle) degree = -max_angle;
    if (degree >  max_angle) degree =  max_angle;
    double center = (CONFIG_APP_CLAW_SERVO_PULSE_MIN_US + CONFIG_APP_CLAW_SERVO_PULSE_MAX_US) / 2.0;
    double half_range = (CONFIG_APP_CLAW_SERVO_PULSE_MAX_US - CONFIG_APP_CLAW_SERVO_PULSE_MIN_US) / 2.0;
    double pulse_us = center + (degree / max_angle) * half_range;
    return (uint32_t)(pulse_us);
}

esp_err_t servo_mcpwm_init(servo_mcpwm_t *servo, int gpio_a, int gpio_b)
{
    esp_err_t ret;

    mcpwm_timer_config_t timer_cfg = {
        .group_id = 0,
        .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = SERVO_RESOLUTION_HZ,
        .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
        .period_ticks = SERVO_PERIOD_TICKS,
    };
    ESP_RETURN_ON_ERROR(mcpwm_new_timer(&timer_cfg, &servo->timer), TAG, "new timer");

    mcpwm_operator_config_t oper_cfg = { .group_id = 0 };
    ESP_GOTO_ON_ERROR(mcpwm_new_operator(&oper_cfg, &servo->oper), fail, TAG, "new oper");
    ESP_GOTO_ON_ERROR(mcpwm_operator_connect_timer(servo->oper, servo->timer),
                      fail, TAG, "connect timer");

    servo->gpio[0] = gpio_a;
    servo->gpio[1] = gpio_b;

    for (int i = 0; i < 2; i++) {
        mcpwm_comparator_config_t cmp_cfg = {
            .flags = { .update_cmp_on_tez = 1 },
        };
        ESP_GOTO_ON_ERROR(mcpwm_new_comparator(servo->oper, &cmp_cfg,
                                                &servo->comparator[i]),
                          fail, TAG, "new comparator %d", i);

        mcpwm_generator_config_t gen_cfg = {
            .gen_gpio_num = servo->gpio[i],
        };
        ESP_GOTO_ON_ERROR(mcpwm_new_generator(servo->oper, &gen_cfg,
                                                &servo->generator[i]),
                          fail, TAG, "new generator %d", i);

        ESP_GOTO_ON_ERROR(
            mcpwm_generator_set_action_on_timer_event(
                servo->generator[i],
                MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP,
                                             MCPWM_TIMER_EVENT_EMPTY,
                                             MCPWM_GEN_ACTION_HIGH)),
            fail, TAG, "set timer action %d", i);

        ESP_GOTO_ON_ERROR(
            mcpwm_generator_set_action_on_compare_event(
                servo->generator[i],
                MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP,
                                               servo->comparator[i],
                                               MCPWM_GEN_ACTION_LOW)),
            fail, TAG, "set compare action %d", i);
    }

    servo->period_ticks = SERVO_PERIOD_TICKS;

    ESP_GOTO_ON_ERROR(servo_mcpwm_set_angle(servo, 0, 0.0), fail, TAG, "init angle 0");
    ESP_GOTO_ON_ERROR(servo_mcpwm_set_angle(servo, 1, 60.0), fail, TAG, "init angle 1");

    ESP_GOTO_ON_ERROR(mcpwm_timer_enable(servo->timer), fail, TAG, "enable timer");
    ESP_GOTO_ON_ERROR(mcpwm_timer_start_stop(servo->timer, MCPWM_TIMER_START_NO_STOP),
                      fail, TAG, "start timer");

    ESP_LOGI(TAG, "initialized: servo1=GPIO%d, servo2=GPIO%d", gpio_a, gpio_b);
    return ESP_OK;

fail:
    servo_mcpwm_deinit(servo);
    return ret;
}

esp_err_t servo_mcpwm_set_angle(servo_mcpwm_t *servo, int channel, double degree)
{
    uint32_t ticks = angle_to_ticks(degree);
    ESP_RETURN_ON_FALSE(channel >= 0 && channel <= 1, ESP_ERR_INVALID_ARG, TAG, "channel");
    ESP_RETURN_ON_ERROR(
        mcpwm_comparator_set_compare_value(servo->comparator[channel], ticks),
        TAG, "set compare ch%d", channel);
    return ESP_OK;
}

esp_err_t servo_mcpwm_deinit(servo_mcpwm_t *servo)
{
    if (servo->timer) {
        mcpwm_timer_start_stop(servo->timer, MCPWM_TIMER_STOP_EMPTY);
        mcpwm_timer_disable(servo->timer);
    }
    for (int i = 0; i < 2; i++) {
        if (servo->generator[i]) mcpwm_del_generator(servo->generator[i]);
        if (servo->comparator[i]) mcpwm_del_comparator(servo->comparator[i]);
    }
    if (servo->oper) mcpwm_del_operator(servo->oper);
    if (servo->timer) mcpwm_del_timer(servo->timer);
    return ESP_OK;
}
