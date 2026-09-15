// main/demo_memory.c -- Memory Passport: a three-key sequence memory game.
#include "demo.h"
#include "memory_model.h"
#include "memory_store.h"

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "ui_pixel.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <stdint.h>
#include <stdio.h>

static const char *TAG = "demo_memory";

#define MEMORY_COMMAND_QUEUE_DEPTH 16
#define MEMORY_WORKER_STACK_SIZE 4096
#define MEMORY_STOP_TIMEOUT_MS 2000
#define MEMORY_WORKER_POLL_MS 20
#define MEMORY_READY_HOLD_MS 500
#define MEMORY_PLAYBACK_ON_MS 360
#define MEMORY_PLAYBACK_GAP_MS 160
#define MEMORY_INPUT_FEEDBACK_MS 180
#define MEMORY_RESULT_FEEDBACK_MS 220
#define MEMORY_BATTERY_REFRESH_MS 5000

// Audio calibration knobs for the real speaker/codec path.
#define MEMORY_AUDIO_RATE 16000
#define MEMORY_AUDIO_VOLUME 65
#define MEMORY_AUDIO_AMPLITUDE 4500
#define MEMORY_AUDIO_CHUNK_SAMPLES 128

typedef struct {
    memory_token_t token;
    memory_input_event_t source;
    uint64_t time_ms;
} memory_command_t;

static const char *const KEY_TEXT[MEMORY_TOKEN_COUNT] = {
    "UP\n^", "DOWN\nv", "OK\no",
};

static lv_obj_t *s_scr;
static lv_obj_t *s_panel;
static lv_obj_t *s_level;
static lv_obj_t *s_best;
static lv_obj_t *s_state;
static lv_obj_t *s_prompt;
static lv_obj_t *s_hint;
static lv_obj_t *s_battery;
static lv_obj_t *s_keys[MEMORY_TOKEN_COUNT];
static lv_obj_t *s_progress[MEMORY_MAX_LEVEL];
static lv_obj_t *s_mascot;

static memory_model_t s_model;
static QueueHandle_t s_commands;
static SemaphoreHandle_t s_stopped;
static TaskHandle_t s_task;
static volatile bool s_cancel;
static bool s_worker_ready;
static bool s_sound_ok;
static bool s_save_ok;
static bool s_queue_drop_logged;
static int s_soc;

static uint8_t s_playback_index;
static bool s_cue_on;
static memory_token_t s_active_token;
static uint64_t s_playback_due_ms;
static uint64_t s_feedback_due_ms;
static uint64_t s_battery_due_ms;
static uint64_t s_input_refresh_due_ms;

static uint32_t model_seed(void)
{
    const uint64_t seed = (uint64_t)esp_timer_get_time();
    return (uint32_t)seed ^ (uint32_t)(seed >> 32) ^ 0x9E3779B9U;
}

static bool token_is_valid(memory_token_t token)
{
    return token == MEMORY_TOKEN_UP || token == MEMORY_TOKEN_DOWN ||
           token == MEMORY_TOKEN_OK;
}

static uint64_t now_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000U;
}

static void set_key_style(uint8_t index, uint32_t background, uint32_t border)
{
    lv_obj_set_style_bg_color(s_keys[index], lv_color_hex(background), 0);
    lv_obj_set_style_border_color(s_keys[index], lv_color_hex(border), 0);
}

static void render_progress(void)
{
    uint8_t filled = 0;
    int current = -1;

    switch (s_model.state) {
    case MEMORY_STATE_PLAYBACK:
        filled = s_playback_index + (s_cue_on ? 1U : 0U);
        current = s_cue_on ? s_playback_index : -1;
        break;
    case MEMORY_STATE_INPUT:
        filled = s_model.input_index;
        current = s_model.input_index < s_model.sequence_len ? s_model.input_index : -1;
        break;
    case MEMORY_STATE_SUCCESS:
    case MEMORY_STATE_MAX_RESULT:
        filled = s_model.sequence_len;
        break;
    case MEMORY_STATE_RESULT:
        filled = s_model.clear_level;
        break;
    default:
        break;
    }

    for (uint8_t i = 0; i < MEMORY_MAX_LEVEL; i++) {
        lv_obj_set_style_bg_color(s_progress[i],
                                  lv_color_hex(i < filled ? UI_GRASS : UI_MUTED), 0);
        lv_obj_set_style_border_color(s_progress[i],
                                      lv_color_hex(i == current ? UI_YELLOW : UI_INK), 0);
        lv_obj_set_style_border_width(s_progress[i], i == current ? 2 : 1, 0);
    }
}

static void render_locked(void)
{
    if (!s_scr)
        return;

    if (!s_worker_ready) {
        lv_label_set_text(s_level, "READY");
        lv_label_set_text(s_best, "BEST --");
        lv_label_set_text(s_state, "READY");
        lv_label_set_text(s_prompt, "Loading local score");
        lv_label_set_text(s_hint, "STARTING...");
    } else {
        switch (s_model.state) {
        case MEMORY_STATE_READY:
            lv_label_set_text(s_level, "READY");
            break;
        case MEMORY_STATE_RESULT:
            lv_label_set_text_fmt(s_level, "CLEAR %u", (unsigned)s_model.clear_level);
            break;
        case MEMORY_STATE_MAX_RESULT:
            lv_label_set_text(s_level, "12 / 12");
            break;
        case MEMORY_STATE_SUCCESS:
            lv_label_set_text_fmt(s_level, "CLEAR %u", (unsigned)s_model.clear_level);
            break;
        default:
            lv_label_set_text_fmt(s_level, "LEVEL %u/12", (unsigned)s_model.sequence_len);
            break;
        }
        lv_label_set_text_fmt(s_best, "BEST %u", (unsigned)s_model.best_level);

        char hint[48];
        switch (s_model.state) {
        case MEMORY_STATE_READY:
            lv_label_set_text(s_state, "READY");
            lv_label_set_text(s_prompt, "Press OK to begin");
            if (!s_sound_ok && !s_save_ok)
                lv_label_set_text(s_hint, "SOUND / SAVE OFF");
            else if (!s_sound_ok)
                lv_label_set_text(s_hint, "SOUND OFF | OK START");
            else if (!s_save_ok)
                lv_label_set_text(s_hint, "SAVE OFF | OK START");
            else
                lv_label_set_text(s_hint, "OK START | HOLD OK BACK");
            break;
        case MEMORY_STATE_PLAYBACK:
            lv_label_set_text(s_state, "WATCH");
            lv_label_set_text(s_prompt, "Remember the pattern");
            snprintf(hint, sizeof(hint), "%s%u/%u",
                     s_sound_ok ? "WATCH " : "SOUND OFF | ",
                     (unsigned)(s_playback_index + (s_cue_on ? 1U : 0U)),
                     (unsigned)s_model.sequence_len);
            lv_label_set_text(s_hint, hint);
            break;
        case MEMORY_STATE_INPUT: {
            lv_label_set_text(s_state, "YOUR TURN");
            lv_label_set_text(s_prompt, "Repeat the pattern");
            const uint64_t now = now_ms();
            const uint64_t remaining = s_model.deadline_ms > now ? s_model.deadline_ms - now : 0;
            snprintf(hint, sizeof(hint), "TIME %u.%us | %u/%u",
                     (unsigned)(remaining / 1000U),
                     (unsigned)((remaining % 1000U) / 100U),
                     (unsigned)s_model.input_index, (unsigned)s_model.sequence_len);
            lv_label_set_text(s_hint, hint);
            break;
        }
        case MEMORY_STATE_SUCCESS:
            lv_label_set_text(s_state, "GOOD");
            lv_label_set_text(s_prompt, "Round cleared");
            lv_label_set_text(s_hint, "NEXT LEVEL...");
            break;
        case MEMORY_STATE_RESULT:
            if (s_model.failure == MEMORY_FAILURE_TIMEOUT) {
                lv_label_set_text(s_state, "TIME UP");
                lv_label_set_text(s_prompt, "No key received");
            } else {
                lv_label_set_text(s_state, "WRONG");
                lv_label_set_text_fmt(s_prompt, "Failed at level %u",
                                      (unsigned)s_model.failed_at);
            }
            lv_label_set_text(s_hint,
                              (!s_save_ok && memory_model_best_needs_save(&s_model))
                                  ? "NOT SAVED | OK RETRY"
                                  : "OK RETRY | HOLD OK BACK");
            break;
        case MEMORY_STATE_MAX_RESULT:
            lv_label_set_text(s_state, "MAX CLEAR");
            lv_label_set_text(s_prompt, "12-step sequence complete");
            lv_label_set_text(s_hint,
                              (!s_save_ok && memory_model_best_needs_save(&s_model))
                                  ? "NOT SAVED | OK RETRY"
                                  : "OK RETRY | HOLD OK BACK");
            break;
        }
    }

    const bool result = s_model.state == MEMORY_STATE_RESULT;
    const bool success = s_model.state == MEMORY_STATE_SUCCESS ||
                         s_model.state == MEMORY_STATE_MAX_RESULT;
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(success ? UI_GRASS : UI_PAPER), 0);
    lv_obj_set_style_border_color(s_panel,
                                  lv_color_hex(result ? UI_RED : UI_INK), 0);
    lv_obj_set_style_text_color(s_state,
                                lv_color_hex(result ? UI_RED
                                                    : success ? UI_GRASS_DARK
                                                              : s_model.state == MEMORY_STATE_PLAYBACK
                                                                    ? UI_SKY_DARK
                                                                    : UI_INK),
                                0);

    for (uint8_t i = 0; i < MEMORY_TOKEN_COUNT; i++) {
        uint32_t background = UI_PAPER;
        uint32_t border = UI_INK;
        if (s_active_token == (memory_token_t)i) {
            background = result ? UI_RED : UI_YELLOW;
            border = 0xFFFFFF;
        } else if (s_model.state == MEMORY_STATE_PLAYBACK || success || result) {
            background = UI_MUTED;
        }
        set_key_style(i, background, border);
    }
    render_progress();

    if (s_soc < 0) {
        lv_obj_add_flag(s_battery, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_battery, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(s_battery, "%d%%", s_soc);
        lv_obj_set_style_text_color(s_battery,
                                    lv_color_hex(s_soc < 20 ? UI_RED : UI_PAPER), 0);
    }
}

static void render_ui(bool jump)
{
    if (!bsp_lvgl_lock(500))
        return;
    render_locked();
    if (jump && s_mascot)
        ui_pixel_mascot_jump(s_mascot);
    bsp_lvgl_unlock();
}

static void set_hint(const char *text)
{
    if (!bsp_lvgl_lock(500))
        return;
    if (s_hint)
        lv_label_set_text(s_hint, text);
    bsp_lvgl_unlock();
}

static void play_frequency(uint16_t frequency_hz, uint16_t duration_ms)
{
    if (!s_sound_ok || frequency_hz == 0)
        return;

    int16_t pcm[MEMORY_AUDIO_CHUNK_SAMPLES];
    const uint32_t period = MEMORY_AUDIO_RATE / frequency_hz;
    uint32_t phase = 0;
    uint32_t remaining = MEMORY_AUDIO_RATE * duration_ms / 1000U;

    while (remaining && !s_cancel) {
        const uint32_t count = remaining < MEMORY_AUDIO_CHUNK_SAMPLES
                                   ? remaining
                                   : MEMORY_AUDIO_CHUNK_SAMPLES;
        for (uint32_t i = 0; i < count; i++) {
            pcm[i] = phase < period / 2U ? MEMORY_AUDIO_AMPLITUDE
                                         : -MEMORY_AUDIO_AMPLITUDE;
            if (++phase >= period)
                phase = 0;
        }
        if (bsp_audio_write(pcm, count * sizeof(pcm[0])) != ESP_OK) {
            ESP_LOGW(TAG, "音调播放失败，后续仅保留视觉提示");
            s_sound_ok = false;
            return;
        }
        remaining -= count;
    }
}

static void play_token(memory_token_t token, uint16_t duration_ms)
{
    static const uint16_t frequencies[MEMORY_TOKEN_COUNT] = { 523, 659, 784 };
    if (token_is_valid(token))
        play_frequency(frequencies[token], duration_ms);
}

static void play_success(void)
{
    play_frequency(523, 100);
    play_frequency(659, 100);
    play_frequency(784, 120);
}

static void play_timeout(void)
{
    play_frequency(330, 150);
    play_frequency(196, 180);
}

static void save_best_if_needed(void)
{
    if (!memory_model_best_needs_save(&s_model))
        return;

    const uint8_t saved_level = s_model.best_level;
    const esp_err_t error = memory_store_save_best(saved_level);
    s_save_ok = error == ESP_OK;
    if (error == ESP_OK)
        memory_model_mark_best_saved(&s_model, saved_level);
}

static void schedule_playback(uint64_t now, uint32_t delay_ms)
{
    s_playback_index = 0;
    s_cue_on = false;
    s_active_token = MEMORY_TOKEN_INVALID;
    s_playback_due_ms = now + delay_ms;
    s_feedback_due_ms = 0;
    render_ui(false);
}

static void start_session(void)
{
    save_best_if_needed();
    if (memory_model_start(&s_model) != MEMORY_EVENT_STARTED)
        return;
    schedule_playback(now_ms(), MEMORY_READY_HOLD_MS);
}

static void handle_model_event(memory_event_t event, memory_token_t token, uint64_t event_ms)
{
    switch (event) {
    case MEMORY_EVENT_TOKEN_ACCEPTED:
        s_active_token = token;
        s_feedback_due_ms = event_ms + MEMORY_INPUT_FEEDBACK_MS;
        render_ui(true);
        play_token(token, 120);
        break;
    case MEMORY_EVENT_ROUND_SUCCESS:
        s_active_token = MEMORY_TOKEN_INVALID;
        s_feedback_due_ms = 0;
        render_ui(true);
        play_success();
        break;
    case MEMORY_EVENT_WRONG:
        s_active_token = token;
        s_feedback_due_ms = event_ms + MEMORY_RESULT_FEEDBACK_MS;
        render_ui(false);
        play_frequency(220, 250);
        save_best_if_needed();
        render_ui(false);
        break;
    case MEMORY_EVENT_TIMEOUT:
        s_active_token = MEMORY_TOKEN_INVALID;
        s_feedback_due_ms = 0;
        render_ui(false);
        play_timeout();
        save_best_if_needed();
        render_ui(false);
        break;
    default:
        break;
    }
}

static void handle_command(const memory_command_t *command)
{
    if (command->source == MEMORY_INPUT_DOUBLE)
        return;

    if (s_model.state == MEMORY_STATE_READY || s_model.state == MEMORY_STATE_RESULT ||
        s_model.state == MEMORY_STATE_MAX_RESULT) {
        if (command->token == MEMORY_TOKEN_OK && command->source == MEMORY_INPUT_CLICK)
            start_session();
        return;
    }

    if (s_model.state != MEMORY_STATE_INPUT)
        return;

    const memory_event_t event = memory_model_submit(&s_model, command->token,
                                                      command->source, command->time_ms);
    handle_model_event(event, command->token, command->time_ms);
}

static void advance_playback(uint64_t now)
{
    if (s_model.state != MEMORY_STATE_PLAYBACK || now < s_playback_due_ms)
        return;

    if (s_cue_on) {
        s_cue_on = false;
        s_active_token = MEMORY_TOKEN_INVALID;
        s_playback_index++;
        s_playback_due_ms = now + MEMORY_PLAYBACK_GAP_MS;
        render_ui(false);
        return;
    }

    if (s_playback_index >= s_model.sequence_len) {
        s_playback_due_ms = 0;
        s_input_refresh_due_ms = now;
        (void)memory_model_playback_done(&s_model, now);
        render_ui(false);
        return;
    }

    s_active_token = memory_model_sequence_at(&s_model, s_playback_index);
    s_cue_on = true;
    s_playback_due_ms = now + MEMORY_PLAYBACK_ON_MS;
    render_ui(false);
    play_token(s_active_token, 200);
}

static void advance_timers(void)
{
    const uint64_t now = now_ms();

    if (s_feedback_due_ms && now >= s_feedback_due_ms) {
        s_feedback_due_ms = 0;
        s_active_token = MEMORY_TOKEN_INVALID;
        render_ui(false);
    }

    if (s_battery_due_ms == 0 || now >= s_battery_due_ms) {
        const int soc = bsp_battery_soc();
        s_battery_due_ms = now + MEMORY_BATTERY_REFRESH_MS;
        if (soc != s_soc) {
            s_soc = soc;
            render_ui(false);
        }
    }

    if (s_model.state == MEMORY_STATE_PLAYBACK) {
        advance_playback(now);
        return;
    }

    if (s_model.state == MEMORY_STATE_INPUT) {
        const memory_event_t event = memory_model_tick(&s_model, now);
        if (event == MEMORY_EVENT_TIMEOUT) {
            handle_model_event(event, MEMORY_TOKEN_INVALID, now);
        } else if (now >= s_input_refresh_due_ms) {
            s_input_refresh_due_ms = now + 100;
            render_ui(false);
        }
        return;
    }

    if (s_model.state == MEMORY_STATE_SUCCESS) {
        const memory_event_t event = memory_model_tick(&s_model, now);
        if (event == MEMORY_EVENT_NEXT_ROUND) {
            schedule_playback(now, MEMORY_PLAYBACK_GAP_MS);
        } else if (event == MEMORY_EVENT_MAX_REACHED) {
            render_ui(true);
            save_best_if_needed();
            render_ui(false);
        }
    }
}

static void memory_task(void *arg)
{
    (void)arg;

    uint8_t best_level = 0;
    s_save_ok = memory_store_load_best(&best_level) == ESP_OK;
    memory_model_init(&s_model, model_seed(), best_level);

    s_sound_ok = bsp_audio_init() == ESP_OK &&
                 bsp_audio_set_format(MEMORY_AUDIO_RATE, 16, 1) == ESP_OK;
    if (s_sound_ok)
        bsp_audio_set_volume(MEMORY_AUDIO_VOLUME);

    s_soc = -1;
    s_battery_due_ms = 0;
    s_worker_ready = true;
    render_ui(false);

    while (!s_cancel) {
        memory_command_t command;
        if (xQueueReceive(s_commands, &command,
                          pdMS_TO_TICKS(MEMORY_WORKER_POLL_MS)) == pdTRUE) {
            handle_command(&command);
        }
        if (!s_cancel)
            advance_timers();
    }

    save_best_if_needed();
    if (s_stopped)
        xSemaphoreGive(s_stopped);
    s_task = NULL;
    vTaskDelete(NULL);
}

void demo_memory_enter(void)
{
    memory_model_init(&s_model, 1, 0);
    s_worker_ready = false;
    s_sound_ok = false;
    s_save_ok = false;
    s_soc = -1;
    s_active_token = MEMORY_TOKEN_INVALID;

    s_scr = ui_pixel_screen_create("MEMORY");
    s_battery = ui_pixel_label(s_scr, "", &lv_font_montserrat_14, UI_PAPER);
    lv_obj_set_pos(s_battery, 174, 28);
    lv_obj_set_width(s_battery, 54);
    lv_obj_set_style_text_align(s_battery, LV_TEXT_ALIGN_RIGHT, 0);

    s_panel = ui_pixel_panel_create(s_scr, 12, 54, 216, 178, UI_PAPER);

    s_level = ui_pixel_label(s_panel, "READY", &lv_font_montserrat_14, UI_INK);
    lv_obj_set_pos(s_level, 0, 0);

    s_best = ui_pixel_label(s_panel, "BEST --", &lv_font_montserrat_14, UI_INK);
    lv_obj_set_pos(s_best, 112, 0);
    lv_obj_set_width(s_best, 76);
    lv_obj_set_style_text_align(s_best, LV_TEXT_ALIGN_RIGHT, 0);

    s_state = ui_pixel_label(s_panel, "READY", &lv_font_montserrat_20, UI_INK);
    lv_obj_set_pos(s_state, 0, 22);
    lv_obj_set_width(s_state, 188);
    lv_obj_set_style_text_align(s_state, LV_TEXT_ALIGN_CENTER, 0);

    s_prompt = ui_pixel_label(s_panel, "Loading local score", &lv_font_montserrat_14, UI_INK);
    lv_obj_set_pos(s_prompt, 0, 47);
    lv_obj_set_width(s_prompt, 188);
    lv_obj_set_style_text_align(s_prompt, LV_TEXT_ALIGN_CENTER, 0);

    for (uint8_t i = 0; i < MEMORY_TOKEN_COUNT; i++) {
        s_keys[i] = lv_obj_create(s_panel);
        lv_obj_remove_flag(s_keys[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(s_keys[i], i * 64, 66);
        lv_obj_set_size(s_keys[i], 56, 48);
        lv_obj_set_style_radius(s_keys[i], 0, 0);
        lv_obj_set_style_border_width(s_keys[i], 3, 0);
        lv_obj_set_style_pad_all(s_keys[i], 0, 0);
        set_key_style(i, UI_PAPER, UI_INK);

        lv_obj_t *label = ui_pixel_label(s_keys[i], KEY_TEXT[i],
                                         &lv_font_montserrat_14, UI_INK);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(label);
    }

    for (uint8_t i = 0; i < MEMORY_MAX_LEVEL; i++) {
        s_progress[i] = lv_obj_create(s_panel);
        lv_obj_remove_flag(s_progress[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(s_progress[i], 6 + i * 15, 120);
        lv_obj_set_size(s_progress[i], 10, 7);
        lv_obj_set_style_radius(s_progress[i], 0, 0);
        lv_obj_set_style_pad_all(s_progress[i], 0, 0);
    }

    s_hint = ui_pixel_label(s_panel, "STARTING...", &lv_font_montserrat_14, UI_INK);
    lv_obj_set_pos(s_hint, 0, 134);
    lv_obj_set_width(s_hint, 188);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);

    s_mascot = ui_pixel_mascot_create(s_scr, 101, 238);
    render_locked();
    lv_screen_load(s_scr);
}

esp_err_t demo_memory_start(void)
{
    if (s_task)
        return ESP_OK;

    if (s_commands) {
        vQueueDelete(s_commands);
        s_commands = NULL;
    }
    if (s_stopped) {
        vSemaphoreDelete(s_stopped);
        s_stopped = NULL;
    }

    s_commands = xQueueCreate(MEMORY_COMMAND_QUEUE_DEPTH, sizeof(memory_command_t));
    s_stopped = xSemaphoreCreateBinary();
    if (!s_commands || !s_stopped) {
        if (s_commands) {
            vQueueDelete(s_commands);
            s_commands = NULL;
        }
        if (s_stopped) {
            vSemaphoreDelete(s_stopped);
            s_stopped = NULL;
        }
        set_hint("START FAILED");
        return ESP_ERR_NO_MEM;
    }

    s_cancel = false;
    s_queue_drop_logged = false;
    if (xTaskCreate(memory_task, "demo_memory", MEMORY_WORKER_STACK_SIZE, NULL, 4,
                    &s_task) != pdPASS) {
        vQueueDelete(s_commands);
        vSemaphoreDelete(s_stopped);
        s_commands = NULL;
        s_stopped = NULL;
        set_hint("START FAILED");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t demo_memory_stop(void)
{
    TaskHandle_t task = s_task;
    if (!task) {
        if (s_commands) {
            vQueueDelete(s_commands);
            s_commands = NULL;
        }
        if (s_stopped) {
            vSemaphoreDelete(s_stopped);
            s_stopped = NULL;
        }
        return ESP_OK;
    }

    s_cancel = true;
    if (!s_stopped ||
        xSemaphoreTake(s_stopped, pdMS_TO_TICKS(MEMORY_STOP_TIMEOUT_MS)) != pdTRUE) {
        set_hint("STOP RETRY");
        return ESP_ERR_TIMEOUT;
    }

    s_task = NULL;
    vQueueDelete(s_commands);
    vSemaphoreDelete(s_stopped);
    s_commands = NULL;
    s_stopped = NULL;
    return ESP_OK;
}

void demo_memory_exit(void)
{
    if (s_scr)
        lv_obj_delete(s_scr);
    s_scr = s_panel = s_level = s_best = s_state = s_prompt = s_hint = NULL;
    s_battery = s_mascot = NULL;
    for (uint8_t i = 0; i < MEMORY_TOKEN_COUNT; i++)
        s_keys[i] = NULL;
    for (uint8_t i = 0; i < MEMORY_MAX_LEVEL; i++)
        s_progress[i] = NULL;
}

void demo_memory_key(bsp_btn_t btn, bsp_btn_ev_t event)
{
    if (!s_task || !s_commands || s_cancel || event == BSP_BTN_LONG)
        return;

    memory_input_event_t source;
    if (event == BSP_BTN_PRESS)
        source = MEMORY_INPUT_PRESS;
    else if (event == BSP_BTN_CLICK)
        source = MEMORY_INPUT_CLICK;
    else if (event == BSP_BTN_DOUBLE)
        source = MEMORY_INPUT_DOUBLE;
    else
        return;

    memory_token_t token = MEMORY_TOKEN_INVALID;
    if (btn == BSP_BTN_UP)
        token = MEMORY_TOKEN_UP;
    else if (btn == BSP_BTN_DOWN)
        token = MEMORY_TOKEN_DOWN;
    else if (btn == BSP_BTN_OK)
        token = MEMORY_TOKEN_OK;
    if (token == MEMORY_TOKEN_INVALID)
        return;

    const memory_command_t command = {
        .token = token,
        .source = source,
        .time_ms = now_ms(),
    };
    if (xQueueSend(s_commands, &command, 0) != pdTRUE && !s_queue_drop_logged) {
        s_queue_drop_logged = true;
        ESP_LOGW(TAG, "输入队列已满，丢弃后续过快按键");
    }
}
