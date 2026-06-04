#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t cap_asr_register_group(void);

/**
 * @brief Stop an ongoing ASR recording immediately.
 * Safe to call from any task context — just sets a flag checked by the capture loop.
 */
void cap_asr_stop(void);

#ifdef __cplusplus
}
#endif
