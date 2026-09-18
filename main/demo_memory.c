// main/demo_memory.c -- Memory Passport: a three-key sequence memory game.
#include "demo.h"
#include "memory_model.h"
#include "memory_store.h"

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "ui_pixel.h"

#include "esp_log.h"
#include "esp_random.h"
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
    "上 ↑", "下 ↓", "确定 ●",
};

static const uint32_t KEY_COLOR[MEMORY_TOKEN_COUNT] = {
    UI_KEY_UP, UI_KEY_DOWN, UI_KEY_OK,
};

typedef struct {
    uint8_t start_level;
    uint32_t timeout_ms;
    uint16_t cue_ms;
} difficulty_config_t;

static const difficulty_config_t DIFFICULTIES[MEMORY_DIFFICULTY_COUNT] = {
    { MEMORY_EASY_START_LEVEL, MEMORY_EASY_INPUT_TIMEOUT_MS, 580 },
    { MEMORY_INITIAL_LEVEL, MEMORY_INPUT_TIMEOUT_MS, 460 },
    { MEMORY_CHALLENGE_START_LEVEL, MEMORY_CHALLENGE_INPUT_TIMEOUT_MS, 360 },
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
static bool s_sound_enabled = true;
static uint8_t s_difficulty = MEMORY_DIFFICULTY_NORMAL;
static uint8_t s_best_level;
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
    return esp_random() ^ (uint32_t)seed ^ (uint32_t)(seed >> 32) ^ 0x9E3779B9U;
}

static uint32_t playback_on_ms(void)
{
    return DIFFICULTIES[s_difficulty].cue_ms;
}

static uint32_t playback_gap_ms(void)
{
    return 170;
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

    static const char *const difficulty_names[] = { "轻松", "标准", "挑战" };
    lv_label_set_text_fmt(s_best, "最高 %u", (unsigned)s_model.best_level);

    if (!s_worker_ready) {
        lv_label_set_text_fmt(s_level, "%s难度", difficulty_names[s_difficulty]);
        lv_label_set_text(s_state, "正在准备");
        lv_label_set_text(s_prompt, "正在读取本地记录");
        lv_label_set_text(s_hint, "请稍候");
    } else {
        switch (s_model.state) {
        case MEMORY_STATE_READY:
            lv_label_set_text_fmt(s_level, "%s难度", difficulty_names[s_difficulty]);
            break;
        case MEMORY_STATE_RESULT:
        case MEMORY_STATE_SUCCESS:
            lv_label_set_text_fmt(s_level, "完成 %u 项", (unsigned)s_model.clear_level);
            break;
        case MEMORY_STATE_MAX_RESULT:
            lv_label_set_text(s_level, "完成 12 项");
            break;
        default:
            lv_label_set_text_fmt(s_level, "第 %u 项", (unsigned)s_model.sequence_len);
            break;
        }

        switch (s_model.state) {
        case MEMORY_STATE_READY:
            lv_label_set_text(s_state, "准备开始");
            lv_label_set_text(s_prompt, "记住顺序，再完整复现");
            lv_label_set_text(s_hint, s_save_ok ? "按确定开始"
                                                : "记录未保存  按确定开始");
            break;
        case MEMORY_STATE_PLAYBACK:
            lv_label_set_text(s_state, "请观察");
            lv_label_set_text(s_prompt, "跟随机器人记住顺序");
            lv_label_set_text_fmt(s_hint, "%s %u/%u",
                                  s_sound_ok ? "正在播放" : "静音播放",
                                  (unsigned)(s_playback_index + (s_cue_on ? 1U : 0U)),
                                  (unsigned)s_model.sequence_len);
            break;
        case MEMORY_STATE_INPUT: {
            lv_label_set_text(s_state, "轮到你了");
            lv_label_set_text(s_prompt, "按刚才顺序操作三键");
            const uint64_t now = now_ms();
            const uint64_t remaining = s_model.deadline_ms > now ? s_model.deadline_ms - now : 0;
            lv_label_set_text_fmt(s_hint, "已输入 %u/%u  剩余 %u.%u 秒",
                                  (unsigned)s_model.input_index,
                                  (unsigned)s_model.sequence_len,
                                  (unsigned)(remaining / 1000U),
                                  (unsigned)((remaining % 1000U) / 100U));
            break;
        }
        case MEMORY_STATE_SUCCESS:
            lv_label_set_text(s_state, "答对了");
            lv_label_set_text(s_prompt, "下一轮会生成全新顺序");
            lv_label_set_text(s_hint, "即将增加一项");
            break;
        case MEMORY_STATE_RESULT:
            lv_label_set_text(s_state, s_model.failure == MEMORY_FAILURE_TIMEOUT
                                           ? "时间到了"
                                           : "顺序不对");
            lv_label_set_text_fmt(s_prompt, "本轮挑战到 %u 项",
                                  (unsigned)s_model.failed_at);
            lv_label_set_text(s_hint, s_save_ok ? "按确定再来一局"
                                                : "记录未保存  按确定重试");
            break;
        case MEMORY_STATE_MAX_RESULT:
            lv_label_set_text(s_state, "全部通关");
            lv_label_set_text(s_prompt, "已完成最高 12 项");
            lv_label_set_text(s_hint, s_save_ok ? "按确定再来一局"
                                                : "记录未保存  按确定重试");
            break;
        }
    }

    const bool result = s_model.state == MEMORY_STATE_RESULT;
    const bool success = s_model.state == MEMORY_STATE_SUCCESS ||
                         s_model.state == MEMORY_STATE_MAX_RESULT;
    lv_obj_set_style_bg_color(s_panel,
                              lv_color_hex(success ? UI_SUCCESS
                                                   : result ? UI_RESULT : UI_PAPER), 0);
    lv_obj_set_style_border_color(s_panel,
                                  lv_color_hex(result ? UI_RED
                                                      : success ? UI_GRASS_DARK : UI_INK), 0);
    lv_obj_set_style_text_color(s_state,
                                lv_color_hex(result ? UI_RED
                                                    : success ? UI_GRASS_DARK
                                                              : s_model.state == MEMORY_STATE_PLAYBACK
                                                                    ? UI_SKY_DARK
                                                                    : UI_INK),
                                0);

    for (uint8_t i = 0; i < MEMORY_TOKEN_COUNT; i++) {
        uint32_t background = KEY_COLOR[i];
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
        lv_label_set_text(s_battery, "--%");
        lv_obj_set_style_text_color(s_battery, lv_color_hex(UI_TEXT_MUTED), 0);
    } else {
        lv_label_set_text_fmt(s_battery, "%d%%", s_soc);
        lv_obj_set_style_text_color(s_battery,
                                    lv_color_hex(s_soc < 20 ? UI_RED : UI_INK), 0);
    }
}

static void render_ui(bool jump)
{
    if (!bsp_lvgl_lock(500))
        return;
    render_locked();
    if (jump && s_mascot) {
        static const int x[MEMORY_TOKEN_COUNT] = { 24, 99, 174 };
        if (token_is_valid(s_active_token))
            lv_obj_set_x(s_mascot, x[s_active_token]);
        ui_pixel_mascot_jump(s_mascot);
    }
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
        s_active_token = token;
        s_feedback_due_ms = event_ms + MEMORY_INPUT_FEEDBACK_MS;
        render_ui(true);
        play_success();
        break;
    case MEMORY_EVENT_WRONG:
        s_active_token = token;
        s_feedback_due_ms = event_ms + MEMORY_RESULT_FEEDBACK_MS;
        render_ui(true);
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
        if ((command->token == MEMORY_TOKEN_OK && command->source == MEMORY_INPUT_CLICK) ||
            (command->token != MEMORY_TOKEN_OK && command->source == MEMORY_INPUT_PRESS)) {
            s_active_token = command->token;
            s_feedback_due_ms = command->time_ms + MEMORY_INPUT_FEEDBACK_MS;
            render_ui(true);
        }
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
        s_playback_due_ms = now + playback_gap_ms();
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
    s_playback_due_ms = now + playback_on_ms();
    render_ui(true);
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
            schedule_playback(now, playback_gap_ms());
        } else if (event == MEMORY_EVENT_MAX_REACHED) {
            render_ui(false);
            save_best_if_needed();
            render_ui(false);
        }
    }
}

static void memory_task(void *arg)
{
    (void)arg;

    memory_model_init(&s_model, model_seed(), s_best_level);
    const difficulty_config_t *difficulty = &DIFFICULTIES[s_difficulty];
    memory_model_configure(&s_model, difficulty->start_level, difficulty->timeout_ms);

    s_sound_ok = s_sound_enabled && bsp_audio_init() == ESP_OK &&
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
    s_best_level = s_model.best_level;
    if (s_stopped)
        xSemaphoreGive(s_stopped);
    s_task = NULL;
    vTaskDelete(NULL);
}

void demo_memory_enter(void)
{
    memory_model_init(&s_model, 1, s_best_level);
    const difficulty_config_t *difficulty = &DIFFICULTIES[s_difficulty];
    memory_model_configure(&s_model, difficulty->start_level, difficulty->timeout_ms);
    s_worker_ready = false;
    s_sound_ok = false;
    s_soc = -1;
    s_active_token = MEMORY_TOKEN_INVALID;

    s_scr = ui_pixel_screen_create("记忆训练");
    s_battery = ui_pixel_label(s_scr, "--%", &ui_font_chinese_16, UI_INK);
    lv_obj_set_pos(s_battery, 195, 3);
    lv_obj_set_width(s_battery, 38);
    lv_obj_set_style_text_align(s_battery, LV_TEXT_ALIGN_RIGHT, 0);

    s_panel = ui_pixel_panel_create(s_scr, 8, 32, 224, 204, UI_PAPER);

    s_level = ui_pixel_label(s_panel, "", &ui_font_chinese_16, UI_INK);
    lv_obj_set_pos(s_level, 0, 0);
    lv_obj_set_width(s_level, 96);

    s_best = ui_pixel_label(s_panel, "", &ui_font_chinese_16, UI_INK);
    lv_obj_set_pos(s_best, 100, 0);
    lv_obj_set_width(s_best, 92);
    lv_obj_set_style_text_align(s_best, LV_TEXT_ALIGN_RIGHT, 0);

    s_state = ui_pixel_label(s_panel, "", &ui_font_chinese_16, UI_INK);
    lv_obj_set_pos(s_state, 4, 42);
    lv_obj_set_width(s_state, 194);
    lv_obj_set_style_text_align(s_state, LV_TEXT_ALIGN_CENTER, 0);

    s_prompt = ui_pixel_label(s_panel, "", &ui_font_chinese_16, UI_INK);
    lv_obj_set_pos(s_prompt, 4, 72);
    lv_obj_set_width(s_prompt, 194);
    lv_obj_set_style_text_align(s_prompt, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_prompt, lv_color_hex(UI_TEXT_MUTED), 0);

    for (uint8_t i = 0; i < MEMORY_TOKEN_COUNT; i++) {
        s_keys[i] = lv_obj_create(s_scr);
        lv_obj_remove_flag(s_keys[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(s_keys[i], 8 + i * 75, 277);
        lv_obj_set_size(s_keys[i], 70, 38);
        lv_obj_set_style_radius(s_keys[i], 0, 0);
        lv_obj_set_style_border_width(s_keys[i], 3, 0);
        lv_obj_set_style_pad_all(s_keys[i], 0, 0);
        set_key_style(i, KEY_COLOR[i], UI_INK);

        lv_obj_t *label = ui_pixel_label(s_keys[i], KEY_TEXT[i],
                                         &ui_font_chinese_16, UI_INK);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(label);
    }

    for (uint8_t i = 0; i < MEMORY_MAX_LEVEL; i++) {
        s_progress[i] = lv_obj_create(s_panel);
        lv_obj_remove_flag(s_progress[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(s_progress[i], 11 + i * 15, 137);
        lv_obj_set_size(s_progress[i], 10, 7);
        lv_obj_set_style_radius(s_progress[i], 0, 0);
        lv_obj_set_style_pad_all(s_progress[i], 0, 0);
    }

    s_hint = ui_pixel_label(s_panel, "", &ui_font_chinese_16, UI_INK);
    lv_obj_set_pos(s_hint, 4, 106);
    lv_obj_set_width(s_hint, 194);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);

    s_mascot = ui_pixel_mascot_create(s_scr, 99, 226);
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
        set_hint("启动失败");
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
        set_hint("启动失败");
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
        set_hint("请再试一次");
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

esp_err_t demo_memory_prepare(void)
{
    s_best_level = 0;
    const esp_err_t error = memory_store_load_best(&s_best_level);
    s_save_ok = error == ESP_OK;
    return error;
}

uint8_t demo_memory_best_level(void)
{
    return s_best_level;
}

bool demo_memory_score_persistent(void)
{
    return s_save_ok;
}

void demo_memory_configure(bool sound_enabled, uint8_t difficulty)
{
    s_sound_enabled = sound_enabled;
    s_difficulty = difficulty < MEMORY_DIFFICULTY_COUNT
                       ? difficulty
                       : MEMORY_DIFFICULTY_NORMAL;
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
