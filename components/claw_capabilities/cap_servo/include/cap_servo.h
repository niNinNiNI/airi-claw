#pragma once

#include "esp_err.h"
#include "servo_mcpwm.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t cap_servo_register_group(void);

servo_mcpwm_t *cap_servo_get_servo(void);

#ifdef __cplusplus
}
#endif
