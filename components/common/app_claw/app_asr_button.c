#include "app_asr_button.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cap_asr.h"
#include "claw_cap.h"
#include "claw_event_publisher.h"
#include "esp_log.h"
#include "video_player.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "asr_btn";

static QueueHandle_t s_asr_queue;

static void asr_button_event(void *user_data, bool pressed)
{
    (void)user_data;
    if (pressed) {
        uint8_t dummy = 1;
        xQueueSend(s_asr_queue, &dummy, 0);
    } else {
        /* Button released — stop any ongoing ASR recording immediately */
        cap_asr_stop();
    }
}

static void asr_handler_task(void *arg)
{
    (void)arg;
    uint8_t dummy;

    while (true) {
        if (xQueueReceive(s_asr_queue, &dummy, portMAX_DELAY) == pdTRUE) {
            /* 1. Set button to listening state */
            video_player_set_button_state(1);
            ESP_LOGI(TAG, "ASR started (listening)");

            /* 2. Call ASR — synchronous, blocks up to ~45s */
            char output[512];
            claw_cap_call_context_t ctx = {0};
            esp_err_t ret = claw_cap_call("start_asr", "{}", &ctx, output, sizeof(output));

            /* 3. Publish result to event router → LLM */
            if (ret == ESP_OK && output[0] && strncmp(output, "Error:", 6) != 0) {
                ESP_LOGI(TAG, "ASR result: \"%s\"", output);
                video_player_set_button_state(2);  /* success */

                static int64_t s_voice_msg_seq = 0;
                char message_id[48];
                snprintf(message_id, sizeof(message_id), "voice-%lld",
                         (long long)++s_voice_msg_seq);

                claw_event_router_publish_message(
                    "voice_gateway",       // source_cap
                    "voice",               // channel
                    "voice_default",       // chat_id
                    output,                // text
                    "voice_user",          // sender_id
                    message_id             // message_id
                );
            } else {
                ESP_LOGW(TAG, "ASR failed: %s", output[0] ? output : "unknown");
                video_player_set_button_state(3);  /* error */
            }

            /* 4. Show result for 2 seconds, then restore idle */
            vTaskDelay(pdMS_TO_TICKS(2000));
            video_player_set_button_state(0);
        }
    }
}

esp_err_t app_asr_button_init(void)
{
    s_asr_queue = xQueueCreate(1, sizeof(uint8_t));
    if (!s_asr_queue) {
        ESP_LOGE(TAG, "Failed to create ASR queue");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = video_player_set_button_callback(asr_button_event, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register button callback");
        vQueueDelete(s_asr_queue);
        s_asr_queue = NULL;
        return ret;
    }

    BaseType_t task_ret = xTaskCreate(asr_handler_task, "asr_btn",
                                      4096, NULL, 5, NULL);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create ASR handler task");
        vQueueDelete(s_asr_queue);
        s_asr_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "ASR button initialized");
    return ESP_OK;
}
