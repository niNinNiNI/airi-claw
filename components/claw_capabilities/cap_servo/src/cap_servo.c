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

/* ---------- idle breathing state ---------- */

static TaskHandle_t s_idle_mgr_task = NULL;
static TaskHandle_t s_idle_breath_task = NULL;
static bool s_idle_enabled = true;
static uint32_t s_idle_delay_ms = CONFIG_APP_CLAW_SERVO_IDLE_DELAY_MS;
static motion_t s_idle_breathing_table[4];

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

/* ---------- idle breathing ---------- */

static void init_idle_breathing_table(void)
{
    double range = (double)CONFIG_APP_CLAW_SERVO_IDLE_SWEEP_RANGE;
    double step_slow = 0.08;
    uint32_t step_delay = 25;

    /* Phase 1: Inhale — drift outward slowly */
    s_idle_breathing_table[0] = (motion_t){
        .id = 1,
        .duration_ms = 2000,
        .servo = {
            {
                .mode = MOTION_SWEEP,
                .min_angle = 0.0,
                .max_angle = range / 2.0,
                .step = step_slow,
                .step_delay_ms = step_delay,
            },
            {
                .mode = MOTION_SWEEP,
                .min_angle = SERVO_HOME_ANGLE_1,
                .max_angle = SERVO_HOME_ANGLE_1 + range / 2.0,
                .step = step_slow,
                .step_delay_ms = step_delay,
            },
        },
    };

    /* Phase 2: Hold peak — subtle wobble */
    s_idle_breathing_table[1] = (motion_t){
        .id = 2,
        .duration_ms = 1500,
        .servo = {
            {
                .mode = MOTION_SWEEP,
                .min_angle = range / 4.0,
                .max_angle = range / 2.0,
                .step = step_slow * 0.5,
                .step_delay_ms = step_delay * 2,
            },
            {
                .mode = MOTION_SWEEP,
                .min_angle = SERVO_HOME_ANGLE_1 + range / 4.0,
                .max_angle = SERVO_HOME_ANGLE_1 + range / 2.0,
                .step = step_slow * 0.5,
                .step_delay_ms = step_delay * 2,
            },
        },
    };

    /* Phase 3: Exhale — return toward home */
    s_idle_breathing_table[2] = (motion_t){
        .id = 3,
        .duration_ms = 2000,
        .servo = {
            {
                .mode = MOTION_SWEEP,
                .min_angle = -range / 4.0,
                .max_angle = range / 2.0,
                .step = step_slow,
                .step_delay_ms = step_delay,
            },
            {
                .mode = MOTION_SWEEP,
                .min_angle = SERVO_HOME_ANGLE_1 - range / 4.0,
                .max_angle = SERVO_HOME_ANGLE_1 + range / 2.0,
                .step = step_slow,
                .step_delay_ms = step_delay,
            },
        },
    };

    /* Phase 4: Rest — subtle tremor near home */
    s_idle_breathing_table[3] = (motion_t){
        .id = 4,
        .duration_ms = 1500,
        .servo = {
            {
                .mode = MOTION_SWEEP,
                .min_angle = -range / 4.0,
                .max_angle = range / 4.0,
                .step = step_slow * 0.4,
                .step_delay_ms = step_delay * 2,
            },
            {
                .mode = MOTION_SWEEP,
                .min_angle = SERVO_HOME_ANGLE_1 - range / 4.0,
                .max_angle = SERVO_HOME_ANGLE_1 + range / 4.0,
                .step = step_slow * 0.4,
                .step_delay_ms = step_delay * 2,
            },
        },
    };
}

static void idle_breathing_task_fn(void *arg)
{
    (void)arg;
    motion_player_run_loop(&s_servo, s_idle_breathing_table, 4);
    /* motion_player_run_loop only returns when stopped.
     * Do NOT reset to home — let the next explicit servo command handle it. */
    s_idle_breath_task = NULL;
    vTaskDelete(NULL);
}

static void idle_motion_stop(void)
{
    if (s_idle_breath_task) {
        motion_player_stop();
        TaskHandle_t t = s_idle_breath_task;
        TickType_t start = xTaskGetTickCount();
        while (motion_player_is_running() &&
               (xTaskGetTickCount() - start) < pdMS_TO_TICKS(500)) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (eTaskGetState(t) != eDeleted) {
            vTaskDelete(t);
        }
        s_idle_breath_task = NULL;
        motion_player_reset();
    }
}

static void idle_motion_start(void)
{
    if (s_idle_breath_task) {
        return;
    }

    /* Ensure servo hardware is initialized (lazy init on first use) */
    if (ensure_servo() != ESP_OK) {
        ESP_LOGE(TAG, "Idle breathing skipped: servo init failed");
        return;
    }

    /* Stop any regular motion before starting idle breathing */
    if (s_motion_task) {
        stop_motion_task();
    }

    BaseType_t created = xTaskCreate(idle_breathing_task_fn, "idle_breath",
                                     4096, NULL, 4, &s_idle_breath_task);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create idle breathing task");
        s_idle_breath_task = NULL;
    } else {
        ESP_LOGI(TAG, "Idle breathing started");
    }
}

/* ---------- idle manager task ---------- */

static void idle_manager_notify(void)
{
    if (s_idle_mgr_task) {
        xTaskNotifyGive(s_idle_mgr_task);
    }
}

static void idle_manager_task_fn(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Idle manager started, delay=%lums", (unsigned long)s_idle_delay_ms);

    while (true) {
        /* Wait for activity notification or idle timeout */
        uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(s_idle_delay_ms));

        if (notified == 0) {
            /* Timeout: no activity for s_idle_delay_ms, start breathing */
            if (s_idle_enabled) {
                idle_motion_start();
            }

            /* Block until activity wakes us */
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

            /* Activity: stop breathing, loop back to countdown */
            idle_motion_stop();
        }
        /* else: notified during countdown, just loop back with fresh timeout */
    }
}

static void idle_manager_start(void)
{
    if (s_idle_mgr_task) {
        /* Already running — notify to reset the countdown */
        idle_manager_notify();
        return;
    }

    BaseType_t created = xTaskCreate(idle_manager_task_fn, "idle_mgr",
                                     4096, NULL, 3, &s_idle_mgr_task);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create idle manager task");
        s_idle_mgr_task = NULL;
    }
}

/* ---------- public idle API ---------- */

void cap_servo_on_activity(void)
{
    idle_manager_notify();
}

void cap_servo_on_response_complete(void)
{
    if (!s_idle_enabled) {
        return;
    }
    idle_manager_notify();
}

void cap_servo_start_idle(void)
{
    if (!s_idle_enabled) {
        return;
    }

    /* Pre-init servo hardware on the caller's stack (main task, plenty of
     * headroom) so that idle_mgr never has to call the heavy MCPWM driver
     * init path.  Safe to call repeatedly — ensure_servo is idempotent. */
    if (ensure_servo() != ESP_OK) {
        ESP_LOGE(TAG, "Idle not started: servo init failed");
        return;
    }

    idle_manager_start();
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

static esp_err_t cap_servo_set_idle_motion_execute(const char *input_json,
                                                    const claw_cap_call_context_t *ctx,
                                                    char *output, size_t output_size)
{
    (void)ctx;

    cJSON *root = cJSON_Parse(input_json ? input_json : "{}");
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *enabled = cJSON_GetObjectItem(root, "enabled");
    if (!cJSON_IsBool(enabled)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: 'enabled' (bool) is required");
        return ESP_ERR_INVALID_ARG;
    }

    s_idle_enabled = cJSON_IsTrue(enabled);

    cJSON *delay_item = cJSON_GetObjectItem(root, "delay_ms");
    if (cJSON_IsNumber(delay_item)) {
        int delay = delay_item->valueint;
        if (delay >= 1000 && delay <= 60000) {
            s_idle_delay_ms = (uint32_t)delay;
        }
    }

    cJSON_Delete(root);

    if (!s_idle_enabled) {
        cap_servo_on_activity();
        snprintf(output, output_size, "Idle motion disabled");
    } else {
        /* Wake the manager so the new delay takes effect immediately */
        idle_manager_notify();
        snprintf(output, output_size, "Idle motion enabled, delay=%lums",
                 (unsigned long)s_idle_delay_ms);
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
    {
        .id = "set_idle_motion",
        .name = "set_idle_motion",
        .family = "servo",
        .description = "Enable or disable automatic idle breathing motion. "
                       "When enabled, the servo performs slow, subtle breathing-like "
                       "sweeps after a period of inactivity following each LLM response. "
                       "A new user request immediately stops idle motion.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
              "\"enabled\":{\"type\":\"boolean\","
                           "\"description\":\"true to enable idle breathing, false to disable\"},"
              "\"delay_ms\":{\"type\":\"integer\","
                            "\"minimum\":1000,\"maximum\":60000,"
                            "\"description\":\"Idle delay in ms before breathing starts (1000-60000), default 5000\"}"
            "},"
            "\"required\":[\"enabled\"]}",
        .execute = cap_servo_set_idle_motion_execute,
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
    init_idle_breathing_table();
    return claw_cap_register_group(&s_servo_group);
}
