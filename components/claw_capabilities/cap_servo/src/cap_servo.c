#include "cap_servo.h"
#include "motion.h"

#include "claw_cap.h"
#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cap_servo";

#ifndef CONFIG_APP_CLAW_SERVO_GPIO_A
#define CONFIG_APP_CLAW_SERVO_GPIO_A 4
#endif
#ifndef CONFIG_APP_CLAW_SERVO_GPIO_B
#define CONFIG_APP_CLAW_SERVO_GPIO_B 5
#endif

#define SERVO_HOME_ANGLE_0  0.0
#define SERVO_HOME_ANGLE_1  60.0

/* ---------- global state ---------- */

static servo_mcpwm_t s_servo = {0};
static bool s_servo_initialized = false;
static TaskHandle_t s_motion_task = NULL;
static double s_last_angle[2] = {SERVO_HOME_ANGLE_0, SERVO_HOME_ANGLE_1};

/* ---------- helpers ---------- */

static esp_err_t ensure_servo(void)
{
    if (s_servo_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = servo_mcpwm_init(&s_servo,
                                     CONFIG_APP_CLAW_SERVO_GPIO_A,
                                     CONFIG_APP_CLAW_SERVO_GPIO_B);
    if (ret == ESP_OK) {
        s_servo_initialized = true;
        s_last_angle[0] = SERVO_HOME_ANGLE_0;
        s_last_angle[1] = SERVO_HOME_ANGLE_1;
    }
    return ret;
}

servo_mcpwm_t *cap_servo_get_servo(void)
{
    return &s_servo;
}

static double clamp_angle(double val, double max_angle)
{
    if (val < -max_angle) return -max_angle;
    if (val > max_angle) return max_angle;
    return val;
}

/**
 * Ease a single channel from its current angle to a target over duration_ms.
 */
static void servo_ease_to(servo_mcpwm_t *servo, int channel,
                          double target, uint32_t duration_ms)
{
    double max_angle = (double)CONFIG_APP_CLAW_SERVO_MAX_ANGLE;
    target = clamp_angle(target, max_angle);

    if (duration_ms == 0) {
        servo_mcpwm_set_angle(servo, channel, target);
        s_last_angle[channel] = target;
        return;
    }

    double current = s_last_angle[channel];
    double delta = target - current;
    uint32_t steps = duration_ms / 20;  /* 20ms per frame = 50Hz update rate */
    if (steps < 1) steps = 1;

    for (uint32_t i = 1; i <= steps; i++) {
        double t = (double)i / steps;
        /* ease-in-out: 3t² - 2t³ */
        double eased = t * t * (3.0 - 2.0 * t);
        double angle = current + delta * eased;
        servo_mcpwm_set_angle(servo, channel, angle);
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    servo_mcpwm_set_angle(servo, channel, target);
    s_last_angle[channel] = target;
}

static ease_type_t parse_ease_type(const char *str)
{
    if (!str) return EASE_IN_OUT_CUBIC;
    if (strcmp(str, "linear") == 0) return EASE_LINEAR;
    if (strcmp(str, "ease_in") == 0) return EASE_IN_QUAD;
    if (strcmp(str, "ease_out") == 0) return EASE_OUT_QUAD;
    if (strcmp(str, "ease_in_out") == 0) return EASE_IN_OUT_QUAD;
    if (strcmp(str, "ease_in_out_cubic") == 0) return EASE_IN_OUT_CUBIC;
    return EASE_IN_OUT_CUBIC;
}

static motion_mode_t parse_motion_mode(const char *str)
{
    if (!str) return MOTION_FIXED;
    if (strcmp(str, "sweep") == 0) return MOTION_SWEEP;
    if (strcmp(str, "ease") == 0) return MOTION_EASE;
    return MOTION_FIXED;
}

static void stop_motion_task(void)
{
    if (s_motion_task) {
        motion_player_stop();
        /* Save handle locally — motion_task_fn may NULL s_motion_task
         * and delete itself once motion_player_run_* returns. */
        TaskHandle_t task = s_motion_task;
        /* Wait up to 500ms for the task to exit */
        TickType_t start = xTaskGetTickCount();
        while (motion_player_is_running() &&
               (xTaskGetTickCount() - start) < pdMS_TO_TICKS(500)) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (eTaskGetState(task) != eDeleted) {
            vTaskDelete(task);
        }
        s_motion_task = NULL;
    }
    motion_player_reset();
}

static void motion_task_fn(void *arg)
{
    motion_task_args_t *args = (motion_task_args_t *)arg;
    if (args->loop) {
        motion_player_run_loop(args->servo, args->table, args->count);
    } else {
        motion_player_run_once(args->servo, args->table, args->count);
        servo_mcpwm_set_angle(args->servo, 0, SERVO_HOME_ANGLE_0);
        servo_mcpwm_set_angle(args->servo, 1, SERVO_HOME_ANGLE_1);
        s_last_angle[0] = SERVO_HOME_ANGLE_0;
        s_last_angle[1] = SERVO_HOME_ANGLE_1;
    }
    free(args->table);
    free(args);
    s_motion_task = NULL;
    vTaskDelete(NULL);
}

/* ---------- execute callbacks ---------- */

static esp_err_t cap_servo_set_angle_execute(const char *input_json,
                                             const claw_cap_call_context_t *ctx,
                                             char *output, size_t output_size)
{
    (void)ctx;

    cJSON *root = cJSON_Parse(input_json ? input_json : "{}");
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *ch_item = cJSON_GetObjectItem(root, "channel");
    cJSON *ang_item = cJSON_GetObjectItem(root, "angle");
    cJSON *dur_item = cJSON_GetObjectItem(root, "duration_ms");

    if (!cJSON_IsNumber(ch_item) || !cJSON_IsNumber(ang_item)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: channel and angle are required");
        return ESP_ERR_INVALID_ARG;
    }

    int channel = ch_item->valueint;
    if (channel < 0 || channel > 1) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: channel must be 0 or 1");
        return ESP_ERR_INVALID_ARG;
    }

    double angle = ang_item->valuedouble;
    uint32_t duration_ms = cJSON_IsNumber(dur_item) ? (uint32_t)dur_item->valueint : 1000;

    cJSON_Delete(root);

    ESP_RETURN_ON_ERROR(ensure_servo(), TAG, "servo init");
    stop_motion_task();

    servo_ease_to(&s_servo, channel, angle, duration_ms);

    snprintf(output, output_size, "Servo %d set to %.1f degrees", channel, angle);
    return ESP_OK;
}

static esp_err_t cap_servo_play_motion_execute(const char *input_json,
                                               const claw_cap_call_context_t *ctx,
                                               char *output, size_t output_size)
{
    (void)ctx;

    cJSON *root = cJSON_Parse(input_json ? input_json : "{}");
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *actions = cJSON_GetObjectItem(root, "actions");
    if (!cJSON_IsArray(actions)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: actions array is required");
        return ESP_ERR_INVALID_ARG;
    }

    int count = cJSON_GetArraySize(actions);
    if (count == 0 || count > 32) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: actions must have 1-32 items");
        return ESP_ERR_INVALID_ARG;
    }

    bool loop = false;
    cJSON *loop_item = cJSON_GetObjectItem(root, "loop");
    if (cJSON_IsBool(loop_item)) {
        loop = cJSON_IsTrue(loop_item);
    }

    motion_t *table = calloc(count, sizeof(motion_t));
    if (!table) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < count; i++) {
        cJSON *action = cJSON_GetArrayItem(actions, i);
        if (!action) continue;

        motion_t *m = &table[i];
        m->id = (uint8_t)(i + 1);

        cJSON *dur = cJSON_GetObjectItem(action, "duration_ms");
        m->duration_ms = cJSON_IsNumber(dur) ? (uint32_t)dur->valueint : 1000;

        const char *ch_keys[] = {"servo0", "servo1"};
        for (int ch = 0; ch < 2; ch++) {
            cJSON *sv = cJSON_GetObjectItem(action, ch_keys[ch]);
            if (!sv) {
                m->servo[ch].mode = MOTION_FIXED;
                m->servo[ch].angle = 0;
                continue;
            }

            cJSON *mode = cJSON_GetObjectItem(sv, "mode");
            m->servo[ch].mode = parse_motion_mode(
                cJSON_IsString(mode) ? mode->valuestring : "fixed");

            cJSON *ang = cJSON_GetObjectItem(sv, "angle");
            m->servo[ch].angle = cJSON_IsNumber(ang) ? ang->valuedouble : 0;

            cJSON *min_a = cJSON_GetObjectItem(sv, "min_angle");
            m->servo[ch].min_angle = cJSON_IsNumber(min_a) ? min_a->valuedouble : -90;

            cJSON *max_a = cJSON_GetObjectItem(sv, "max_angle");
            m->servo[ch].max_angle = cJSON_IsNumber(max_a) ? max_a->valuedouble : 90;

            cJSON *step = cJSON_GetObjectItem(sv, "step");
            m->servo[ch].step = cJSON_IsNumber(step) ? step->valuedouble : 1.5;

            cJSON *sdm = cJSON_GetObjectItem(sv, "step_delay_ms");
            m->servo[ch].step_delay_ms = cJSON_IsNumber(sdm) ? (uint32_t)sdm->valueint : 14;

            cJSON *ease = cJSON_GetObjectItem(sv, "ease_type");
            m->servo[ch].ease_type = parse_ease_type(
                cJSON_IsString(ease) ? ease->valuestring : NULL);
        }
    }

    cJSON_Delete(root);

    ESP_RETURN_ON_ERROR(ensure_servo(), TAG, "servo init");
    stop_motion_task();

    motion_task_args_t *args = malloc(sizeof(motion_task_args_t));
    if (!args) {
        free(table);
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }
    args->servo = &s_servo;
    args->table = table;
    args->count = count;
    args->loop = loop;

    BaseType_t created = xTaskCreate(motion_task_fn, "motion_player",
                                     4096, args, 5, &s_motion_task);
    if (created != pdPASS) {
        free(table);
        free(args);
        s_motion_task = NULL;
        snprintf(output, output_size, "Error: failed to create motion task");
        return ESP_FAIL;
    }

    snprintf(output, output_size,
             "Playing %d motion%s%s", count,
             count > 1 ? "s" : "",
             loop ? " in loop" : "");
    return ESP_OK;
}

static esp_err_t cap_servo_stop_execute(const char *input_json,
                                        const claw_cap_call_context_t *ctx,
                                        char *output, size_t output_size)
{
    (void)ctx;

    bool reset = true;
    cJSON *root = cJSON_Parse(input_json ? input_json : "{}");
    if (root) {
        cJSON *rst = cJSON_GetObjectItem(root, "reset");
        if (cJSON_IsBool(rst)) {
            reset = cJSON_IsTrue(rst);
        }
        cJSON_Delete(root);
    }

    if (!s_servo_initialized) {
        snprintf(output, output_size, "Servo not initialized");
        return ESP_OK;
    }

    stop_motion_task();

    if (reset) {
        servo_mcpwm_set_angle(&s_servo, 0, SERVO_HOME_ANGLE_0);
        servo_mcpwm_set_angle(&s_servo, 1, SERVO_HOME_ANGLE_1);
        s_last_angle[0] = SERVO_HOME_ANGLE_0;
        s_last_angle[1] = SERVO_HOME_ANGLE_1;
        snprintf(output, output_size, "Servo stopped and reset to home positions");
    } else {
        snprintf(output, output_size, "Servo stopped");
    }

    return ESP_OK;
}

/* ---------- descriptors & group ---------- */

static const claw_cap_descriptor_t s_servo_descriptors[] = {
    {
        .id = "set_servo_angle",
        .name = "set_servo_angle",
        .family = "servo",
        .description = "Set a servo channel to a specific angle with smooth "
                       "easing. Channel 0=Yaw(Pan) bottom/base servo for horizontal "
                       "left(-)/right(+) rotation, Channel 1=Pitch(Tilt) top/bracket "
                       "servo for vertical up(+)/down(-) tilt. Angle in degrees, "
                       "optional duration_ms (default 1000ms, 0=instant).",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
              "\"channel\":{\"type\":\"integer\",\"enum\":[0,1],"
                           "\"description\":\"Servo channel: 0=Yaw(Pan) bottom base (horizontal rotation), 1=Pitch(Tilt) top bracket (vertical tilt)\"},"
              "\"angle\":{\"type\":\"number\","
                        "\"description\":\"Target angle in degrees\"},"
              "\"duration_ms\":{\"type\":\"integer\","
                               "\"description\":\"Easing duration in ms, default 1000\"}"
            "},"
            "\"required\":[\"channel\",\"angle\"]}",
        .execute = cap_servo_set_angle_execute,
    },
    {
        .id = "play_motion_sequence",
        .name = "play_motion_sequence",
        .family = "servo",
        .description = "Play a sequence of servo motions on the 2-DOF Pan-Tilt mount. "
                       "servo0=Yaw(Pan) bottom/base servo for horizontal left/right rotation, "
                       "servo1=Pitch(Tilt) top/bracket servo for vertical up/down tilt. "
                       "Each action has a duration_ms and per-channel settings with mode "
                       "(fixed/sweep/ease), angle, min/max angles, step, step_delay_ms, "
                       "and ease_type.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
              "\"actions\":{\"type\":\"array\",\"items\":{\"type\":\"object\","
                "\"properties\":{"
                  "\"duration_ms\":{\"type\":\"integer\"},"
                  "\"servo0\":{\"type\":\"object\",\"description\":\"Yaw(Pan) bottom base servo — horizontal left/right rotation\",\"properties\":{"
                    "\"mode\":{\"type\":\"string\",\"enum\":[\"fixed\",\"sweep\",\"ease\"]},"
                    "\"angle\":{\"type\":\"number\"},"
                    "\"min_angle\":{\"type\":\"number\"},"
                    "\"max_angle\":{\"type\":\"number\"},"
                    "\"step\":{\"type\":\"number\"},"
                    "\"step_delay_ms\":{\"type\":\"integer\"},"
                    "\"ease_type\":{\"type\":\"string\",\"enum\":[\"linear\",\"ease_in\",\"ease_out\",\"ease_in_out\",\"ease_in_out_cubic\"]}"
                  "}},"
                  "\"servo1\":{\"type\":\"object\",\"description\":\"Pitch(Tilt) top bracket servo — vertical up/down tilt\",\"properties\":{"
                    "\"mode\":{\"type\":\"string\",\"enum\":[\"fixed\",\"sweep\",\"ease\"]},"
                    "\"angle\":{\"type\":\"number\"},"
                    "\"min_angle\":{\"type\":\"number\"},"
                    "\"max_angle\":{\"type\":\"number\"},"
                    "\"step\":{\"type\":\"number\"},"
                    "\"step_delay_ms\":{\"type\":\"integer\"},"
                    "\"ease_type\":{\"type\":\"string\",\"enum\":[\"linear\",\"ease_in\",\"ease_out\",\"ease_in_out\",\"ease_in_out_cubic\"]}"
                  "}}"
                "}"
              "}},"
              "\"loop\":{\"type\":\"boolean\",\"description\":\"Loop the sequence, default false\"}"
            "},"
            "\"required\":[\"actions\"]}",
        .execute = cap_servo_play_motion_execute,
    },
    {
        .id = "stop_servo",
        .name = "stop_servo",
        .family = "servo",
        .description = "Stop all servo motion immediately. Optionally reset "
                       "both channels to home positions: ch0(Yaw/Pan)=0° "
                       "(straight ahead), ch1(Pitch/Tilt)=60° (slight downward tilt).",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
              "\"reset\":{\"type\":\"boolean\","
                        "\"description\":\"Reset to home positions, default true\"}"
            "}}",
        .execute = cap_servo_stop_execute,
    },
};

static const claw_cap_group_t s_servo_group = {
    .group_id = "cap_servo",
    .descriptors = s_servo_descriptors,
    .descriptor_count = sizeof(s_servo_descriptors) / sizeof(s_servo_descriptors[0]),
};

esp_err_t cap_servo_register_group(void)
{
    if (claw_cap_group_exists(s_servo_group.group_id)) {
        return ESP_OK;
    }
    return claw_cap_register_group(&s_servo_group);
}
