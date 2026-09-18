// Memory Passport product shell: boot, five-item menu, settings, and game.
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "demo.h"
#include "demo_navigation.h"
#include "ui_pixel.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <stddef.h>
#include <stdio.h>

static const char *TAG = "main";

static void difficulty_enter(void);
static void difficulty_exit(void);
static void difficulty_key(bsp_btn_t btn, bsp_btn_ev_t event);
static void more_enter(void);
static void more_exit(void);
static void more_key(bsp_btn_t btn, bsp_btn_ev_t event);
static void app_exit_enter(void);
static void app_exit_exit(void);
static void app_exit_key(bsp_btn_t btn, bsp_btn_ev_t event);

static const demo_entry_t MENU[] = {
    { .name = "开始游戏", .enter = demo_memory_enter, .exit = demo_memory_exit,
      .key = demo_memory_key, .start = demo_memory_start, .stop = demo_memory_stop },
    { .name = "选择难度", .enter = difficulty_enter, .exit = difficulty_exit,
      .key = difficulty_key },
    { .name = "基础设置", .enter = demo_settings_enter, .exit = demo_settings_exit,
      .key = demo_settings_key },
    { .name = "更多功能", .enter = more_enter, .exit = more_exit, .key = more_key },
    { .name = "退出", .enter = app_exit_enter, .exit = app_exit_exit, .key = app_exit_key },
};
#define MENU_COUNT (sizeof(MENU) / sizeof(MENU[0]))
#define INPUT_QUEUE_DEPTH 8

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static bool s_ok[MENU_COUNT];
static demo_navigation_t s_navigation;
static QueueHandle_t s_input_queue;
static TaskHandle_t s_input_task;
static volatile bool s_input_ready;
static lv_obj_t *s_status_battery;
static lv_timer_t *s_status_timer;

static lv_obj_t *s_boot_scr;
static lv_obj_t *s_home_scr;
static lv_obj_t *s_home_cards[MENU_COUNT];
static lv_obj_t *s_home_rows[MENU_COUNT];

static uint8_t s_brightness = 100;
static bool s_sound_enabled = true;
static uint8_t s_difficulty = MEMORY_DIFFICULTY_NORMAL;

static lv_obj_t *s_difficulty_scr;
static lv_obj_t *s_difficulty_cards[MEMORY_DIFFICULTY_COUNT];
static lv_obj_t *s_difficulty_names[MEMORY_DIFFICULTY_COUNT];
static uint8_t s_difficulty_selected;

static lv_obj_t *s_settings_scr;
static lv_obj_t *s_settings_cards[2];
static lv_obj_t *s_settings_values[2];
static uint8_t s_settings_selected;

static lv_obj_t *s_more_scr;
static lv_obj_t *s_more_cards[3];
static lv_obj_t *s_more_copy;
static uint8_t s_more_selected;

static lv_obj_t *s_exit_scr;
static lv_obj_t *s_exit_cards[2];
static uint8_t s_exit_selected;
static bool s_exit_shutdown;

static void set_status_battery(lv_obj_t *battery);
static void leave_active_page(void);

static lv_obj_t *page_create(lv_obj_t *screen)
{
    return ui_pixel_panel_create(screen, 8, 32, 224, 275, UI_PAPER);
}

static void set_status_battery(lv_obj_t *battery)
{
    if (!battery)
        return;
    const int soc = bsp_battery_soc();
    if (soc < 0) {
        lv_label_set_text(battery, "--%");
        lv_obj_set_style_text_color(battery, lv_color_hex(UI_TEXT_MUTED), 0);
    } else {
        lv_label_set_text_fmt(battery, "%d%%", soc);
        lv_obj_set_style_text_color(battery,
                                    lv_color_hex(soc < 20 ? UI_RED : UI_INK), 0);
    }
}

static void status_tick(lv_timer_t *timer)
{
    (void)timer;
    set_status_battery(s_status_battery);
}

static lv_obj_t *shell_screen_create(const char *title)
{
    lv_obj_t *screen = ui_pixel_screen_create(title);
    s_status_battery = ui_pixel_label(screen, "--%", &ui_font_chinese_16, UI_INK);
    lv_obj_set_pos(s_status_battery, 195, 3);
    lv_obj_set_width(s_status_battery, 38);
    lv_obj_set_style_text_align(s_status_battery, LV_TEXT_ALIGN_RIGHT, 0);
    set_status_battery(s_status_battery);
    s_status_timer = lv_timer_create(status_tick, 5000, NULL);
    return screen;
}

static void shell_screen_delete(lv_obj_t **screen)
{
    if (s_status_timer) {
        lv_timer_delete(s_status_timer);
        s_status_timer = NULL;
    }
    if (screen && *screen) {
        lv_obj_delete(*screen);
        *screen = NULL;
    }
    s_status_battery = NULL;
}

static void home_refresh(void)
{
    for (size_t i = 0; i < MENU_COUNT; i++) {
        lv_label_set_text(s_home_rows[i], MENU[i].name);
        ui_pixel_set_selected(s_home_cards[i], i == s_navigation.selected, s_ok[i]);
    }
}

static void home_build(void)
{
    s_home_scr = shell_screen_create("主菜单");
    lv_obj_t *page = page_create(s_home_scr);
    lv_obj_t *heading = ui_pixel_label(page, "记忆通行证", &ui_font_chinese_16, UI_INK);
    lv_obj_set_width(heading, 188);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(heading, LV_ALIGN_TOP_MID, 0, 0);

    for (size_t i = 0; i < MENU_COUNT; i++) {
        s_home_cards[i] = ui_pixel_panel_create(page, 7, 31 + (int)i * 40,
                                                188, 34, UI_CARD);
        s_home_rows[i] = ui_pixel_label(s_home_cards[i], MENU[i].name,
                                        &ui_font_chinese_16, UI_INK);
        lv_obj_set_width(s_home_rows[i], 164);
        lv_obj_set_style_text_align(s_home_rows[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(s_home_rows[i]);
    }

    lv_obj_t *hint = ui_pixel_label(page, "上下选择  确定进入",
                                    &ui_font_chinese_16, UI_TEXT_MUTED);
    lv_obj_set_pos(hint, 7, 238);
    lv_obj_set_width(hint, 188);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    home_refresh();
    lv_screen_load(s_home_scr);
}

static void home_delete(void)
{
    shell_screen_delete(&s_home_scr);
    for (size_t i = 0; i < MENU_COUNT; i++)
        s_home_cards[i] = s_home_rows[i] = NULL;
}

static void home_load(void)
{
    home_build();
}

static void boot_build(void)
{
    s_boot_scr = shell_screen_create("开机");
    lv_obj_t *page = page_create(s_boot_scr);
    lv_obj_t *name = ui_pixel_label(page, "记忆通行证", &ui_font_chinese_16, UI_INK);
    lv_obj_set_width(name, 188);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 82);
    lv_obj_t *ready = ui_pixel_label(page, "正在启动", &ui_font_chinese_16, UI_TEXT_MUTED);
    lv_obj_set_width(ready, 188);
    lv_obj_set_style_text_align(ready, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ready, LV_ALIGN_TOP_MID, 0, 118);
    ui_pixel_mascot_create(s_boot_scr, 101, 238);
    lv_screen_load(s_boot_scr);
}

static demo_nav_input_t navigation_input(bsp_btn_t btn, bsp_btn_ev_t event)
{
    if (event == BSP_BTN_LONG && btn == BSP_BTN_OK)
        return DEMO_NAV_INPUT_OK_LONG;
    if (event != BSP_BTN_CLICK)
        return DEMO_NAV_INPUT_OTHER;
    if (btn == BSP_BTN_UP)
        return DEMO_NAV_INPUT_UP_CLICK;
    if (btn == BSP_BTN_DOWN)
        return DEMO_NAV_INPUT_DOWN_CLICK;
    if (btn == BSP_BTN_OK)
        return DEMO_NAV_INPUT_OK_CLICK;
    return DEMO_NAV_INPUT_OTHER;
}

static void leave_active_page(void)
{
    if (s_navigation.active < 0)
        return;

    const demo_entry_t *active_demo = &MENU[s_navigation.active];
    const esp_err_t error = active_demo->stop ? active_demo->stop() : ESP_OK;
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "%s stop failed: %s", active_demo->name, esp_err_to_name(error));
        return;
    }
    if (!bsp_lvgl_lock(500))
        return;
    active_demo->exit();
    demo_navigation_complete_exit(&s_navigation);
    home_load();
    bsp_lvgl_unlock();
}

static void process_input(const input_event_t *input)
{
    const demo_nav_input_t nav_input = navigation_input(input->btn, input->event);
    if (s_navigation.active >= 0) {
        const demo_entry_t *active_demo = &MENU[s_navigation.active];
        const demo_nav_result_t result = demo_navigation_handle(&s_navigation, nav_input, true);
        if (result.action == DEMO_NAV_ACTION_EXIT)
            leave_active_page();
        else if (result.action == DEMO_NAV_ACTION_FORWARD)
            active_demo->key(input->btn, input->event);
        return;
    }

    if (nav_input == DEMO_NAV_INPUT_OTHER || nav_input == DEMO_NAV_INPUT_OK_LONG)
        return;
    if (!bsp_lvgl_lock(500))
        return;
    const demo_nav_result_t result = demo_navigation_handle(
        &s_navigation, nav_input, s_ok[s_navigation.selected]);
    if (result.action == DEMO_NAV_ACTION_REFRESH) {
        home_refresh();
    } else if (result.action == DEMO_NAV_ACTION_ENTER) {
        const demo_entry_t *demo = &MENU[result.index];
        home_delete();
        demo->enter();
        bsp_lvgl_unlock();
        if (demo->start) {
            const esp_err_t error = demo->start();
            if (error != ESP_OK)
                ESP_LOGE(TAG, "%s start failed: %s", demo->name, esp_err_to_name(error));
        }
        return;
    }
    bsp_lvgl_unlock();
}

static void input_task(void *arg)
{
    (void)arg;
    input_event_t input;
    for (;;) {
        if (xQueueReceive(s_input_queue, &input, portMAX_DELAY) == pdTRUE)
            process_input(&input);
    }
}

static esp_err_t input_dispatch_init(void)
{
    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (!s_input_queue)
        return ESP_ERR_NO_MEM;
    if (xTaskCreate(input_task, "app_input", 4096, NULL, 5, &s_input_task) != pdPASS) {
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void on_key(bsp_btn_t btn, bsp_btn_ev_t event, void *user)
{
    (void)user;
    if (!s_input_ready || !s_input_queue)
        return;
    const input_event_t input = { .btn = btn, .event = event };
    (void)xQueueSend(s_input_queue, &input, 0);
}

static void difficulty_refresh_locked(void)
{
    static const char *const names[] = { "轻松", "标准", "挑战" };
    for (uint8_t i = 0; i < MEMORY_DIFFICULTY_COUNT; i++) {
        char text[20];
        snprintf(text, sizeof(text), "%s%s", names[i], i == s_difficulty ? " 当前" : "");
        lv_label_set_text(s_difficulty_names[i], text);
        ui_pixel_set_selected(s_difficulty_cards[i], i == s_difficulty_selected, true);
    }
}

static void difficulty_refresh(void)
{
    if (!bsp_lvgl_lock(250))
        return;
    difficulty_refresh_locked();
    bsp_lvgl_unlock();
}

static void difficulty_enter(void)
{
    static const char *const notes[] = { "2项 7秒", "3项 5秒", "4项 4秒" };
    s_difficulty_selected = s_difficulty;
    s_difficulty_scr = shell_screen_create("选择难度");
    lv_obj_t *page = page_create(s_difficulty_scr);
    lv_obj_t *heading = ui_pixel_label(page, "选择难度", &ui_font_chinese_16, UI_INK);
    lv_obj_set_width(heading, 188);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(heading, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_t *subtitle = ui_pixel_label(page, "确定后返回主菜单",
                                        &ui_font_chinese_16, UI_TEXT_MUTED);
    lv_obj_set_pos(subtitle, 7, 24);
    lv_obj_set_width(subtitle, 188);
    lv_obj_set_style_text_align(subtitle, LV_TEXT_ALIGN_CENTER, 0);

    for (uint8_t i = 0; i < MEMORY_DIFFICULTY_COUNT; i++) {
        s_difficulty_cards[i] = ui_pixel_panel_create(page, 5, 54 + i * 54,
                                                      192, 42, UI_CARD);
        s_difficulty_names[i] = ui_pixel_label(s_difficulty_cards[i], "",
                                               &ui_font_chinese_16, UI_INK);
        lv_obj_set_width(s_difficulty_names[i], 88);
        lv_obj_align(s_difficulty_names[i], LV_ALIGN_LEFT_MID, 4, 0);
        lv_obj_t *note = ui_pixel_label(s_difficulty_cards[i], notes[i],
                                        &ui_font_chinese_16, UI_TEXT_MUTED);
        lv_obj_set_width(note, 70);
        lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(note, LV_ALIGN_RIGHT_MID, -4, 0);
    }
    lv_obj_t *hint = ui_pixel_label(page, "上下选择  确定保存",
                                    &ui_font_chinese_16, UI_TEXT_MUTED);
    lv_obj_set_pos(hint, 5, 236);
    lv_obj_set_width(hint, 192);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    difficulty_refresh_locked();
    lv_screen_load(s_difficulty_scr);
}

static void difficulty_exit(void)
{
    shell_screen_delete(&s_difficulty_scr);
    for (uint8_t i = 0; i < MEMORY_DIFFICULTY_COUNT; i++)
        s_difficulty_cards[i] = s_difficulty_names[i] = NULL;
}

static void difficulty_key(bsp_btn_t btn, bsp_btn_ev_t event)
{
    if (event != BSP_BTN_CLICK || !s_difficulty_scr)
        return;
    if (btn == BSP_BTN_UP) {
        s_difficulty_selected =
            (s_difficulty_selected + MEMORY_DIFFICULTY_COUNT - 1) % MEMORY_DIFFICULTY_COUNT;
    } else if (btn == BSP_BTN_DOWN) {
        s_difficulty_selected = (s_difficulty_selected + 1) % MEMORY_DIFFICULTY_COUNT;
    } else if (btn == BSP_BTN_OK) {
        s_difficulty = s_difficulty_selected;
        demo_memory_configure(s_sound_enabled, s_difficulty);
        leave_active_page();
        return;
    } else {
        return;
    }
    difficulty_refresh();
}

static void settings_refresh_locked(void)
{
    lv_label_set_text_fmt(s_settings_values[0], "%u%%", (unsigned)s_brightness);
    lv_label_set_text(s_settings_values[1], s_sound_enabled ? "开启" : "关闭");
    for (uint8_t i = 0; i < 2; i++)
        ui_pixel_set_selected(s_settings_cards[i], i == s_settings_selected, true);
}

static void settings_refresh(void)
{
    if (!bsp_lvgl_lock(250))
        return;
    settings_refresh_locked();
    bsp_lvgl_unlock();
}

void demo_settings_enter(void)
{
    static const char *const names[] = { "屏幕亮度", "游戏音效" };
    s_settings_selected = 0;
    s_settings_scr = shell_screen_create("基础设置");
    lv_obj_t *page = page_create(s_settings_scr);
    lv_obj_t *heading = ui_pixel_label(page, "基础设置", &ui_font_chinese_16, UI_INK);
    lv_obj_set_width(heading, 188);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(heading, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_t *subtitle = ui_pixel_label(page, "设置保留至本次关机",
                                        &ui_font_chinese_16, UI_TEXT_MUTED);
    lv_obj_set_pos(subtitle, 5, 28);
    lv_obj_set_width(subtitle, 192);
    lv_obj_set_style_text_align(subtitle, LV_TEXT_ALIGN_CENTER, 0);

    for (uint8_t i = 0; i < 2; i++) {
        s_settings_cards[i] = ui_pixel_panel_create(page, 5, 68 + i * 65,
                                                    192, 54, UI_CARD);
        lv_obj_t *label = ui_pixel_label(s_settings_cards[i], names[i],
                                         &ui_font_chinese_16, UI_INK);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 4, 0);
        s_settings_values[i] = ui_pixel_label(s_settings_cards[i], "",
                                              &ui_font_chinese_16, UI_SKY_DARK);
        lv_obj_set_width(s_settings_values[i], 64);
        lv_obj_set_style_text_align(s_settings_values[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(s_settings_values[i], LV_ALIGN_RIGHT_MID, -4, 0);
    }
    lv_obj_t *hint = ui_pixel_label(page, "上下选择  确定调整",
                                    &ui_font_chinese_16, UI_TEXT_MUTED);
    lv_obj_set_pos(hint, 5, 236);
    lv_obj_set_width(hint, 192);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    settings_refresh_locked();
    lv_screen_load(s_settings_scr);
}

void demo_settings_exit(void)
{
    shell_screen_delete(&s_settings_scr);
    for (uint8_t i = 0; i < 2; i++)
        s_settings_cards[i] = s_settings_values[i] = NULL;
}

void demo_settings_key(bsp_btn_t btn, bsp_btn_ev_t event)
{
    if (event != BSP_BTN_CLICK || !s_settings_scr)
        return;
    if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
        s_settings_selected = (s_settings_selected + 1) % 2;
    } else if (btn == BSP_BTN_OK) {
        static const uint8_t levels[] = { 50, 75, 100 };
        if (s_settings_selected == 0) {
            size_t current = 0;
            for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
                if (levels[i] == s_brightness)
                    current = i;
            }
            s_brightness = levels[(current + 1) % (sizeof(levels) / sizeof(levels[0]))];
            bsp_display_backlight(s_brightness);
        } else {
            s_sound_enabled = !s_sound_enabled;
        }
        demo_memory_configure(s_sound_enabled, s_difficulty);
    } else {
        return;
    }
    settings_refresh();
}

static void more_refresh_locked(void)
{
    for (uint8_t i = 0; i < 3; i++)
        ui_pixel_set_selected(s_more_cards[i], i == s_more_selected, true);

    if (s_more_selected == 0) {
        lv_label_set_text(s_more_copy,
                          "观察顺序，用三键复现\n"
                          "答对后，下一轮生成\n"
                          "全新的随机顺序");
    } else if (s_more_selected == 1) {
        if (demo_memory_score_persistent()) {
            lv_label_set_text_fmt(s_more_copy,
                                  "记录按通关长度计算\n"
                                  "当前最高 %u 项\n不作能力评价",
                                  (unsigned)demo_memory_best_level());
        } else {
            lv_label_set_text_fmt(s_more_copy,
                                  "记录未保存\n当前最高 %u 项\n重新开机后不保留",
                                  (unsigned)demo_memory_best_level());
        }
    } else {
        lv_label_set_text(s_more_copy, "记忆通行证\n版本 1.0");
    }
}

static void more_refresh(void)
{
    if (!bsp_lvgl_lock(250))
        return;
    more_refresh_locked();
    bsp_lvgl_unlock();
}

static void more_enter(void)
{
    static const char *const names[] = { "玩法说明", "最高记录", "版本信息" };
    s_more_selected = 0;
    s_more_scr = shell_screen_create("更多功能");
    lv_obj_t *page = page_create(s_more_scr);
    lv_obj_t *heading = ui_pixel_label(page, "更多功能", &ui_font_chinese_16, UI_INK);
    lv_obj_set_width(heading, 188);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(heading, LV_ALIGN_TOP_MID, 0, 0);

    for (uint8_t i = 0; i < 3; i++) {
        s_more_cards[i] = ui_pixel_panel_create(page, 15, 29 + i * 41,
                                                172, 35, UI_CARD);
        lv_obj_t *label = ui_pixel_label(s_more_cards[i], names[i],
                                         &ui_font_chinese_16, UI_INK);
        lv_obj_set_width(label, 148);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(label);
    }

    lv_obj_t *copy_panel = ui_pixel_panel_create(page, 5, 155, 192, 78, UI_COPY);
    s_more_copy = ui_pixel_label(copy_panel, "", &ui_font_chinese_16, UI_TEXT_MUTED);
    lv_obj_set_size(s_more_copy, 170, 54);
    lv_label_set_long_mode(s_more_copy, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_more_copy, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_line_space(s_more_copy, 0, 0);
    lv_obj_align(s_more_copy, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *hint = ui_pixel_label(page, "上下查看  长按确定返回",
                                    &ui_font_chinese_16, UI_TEXT_MUTED);
    lv_obj_set_pos(hint, 0, 239);
    lv_obj_set_width(hint, 202);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    more_refresh_locked();
    lv_screen_load(s_more_scr);
}

static void more_exit(void)
{
    shell_screen_delete(&s_more_scr);
    for (uint8_t i = 0; i < 3; i++)
        s_more_cards[i] = NULL;
    s_more_copy = NULL;
}

static void more_key(bsp_btn_t btn, bsp_btn_ev_t event)
{
    if (event != BSP_BTN_CLICK || !s_more_scr)
        return;
    if (btn == BSP_BTN_UP)
        s_more_selected = (s_more_selected + 2) % 3;
    else if (btn == BSP_BTN_DOWN)
        s_more_selected = (s_more_selected + 1) % 3;
    else
        return;
    more_refresh();
}

static void exit_refresh_locked(void)
{
    for (uint8_t i = 0; i < 2; i++)
        ui_pixel_set_selected(s_exit_cards[i], i == s_exit_selected, true);
}

static void app_exit_build(bool shutdown)
{
    s_exit_scr = shell_screen_create(shutdown ? "已退出" : "退出");
    lv_obj_t *page = page_create(s_exit_scr);
    if (shutdown) {
        lv_obj_t *heading = ui_pixel_label(page, "已退出游戏",
                                           &ui_font_chinese_16, UI_INK);
        lv_obj_set_pos(heading, 7, 88);
        lv_obj_set_width(heading, 188);
        lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_t *subtitle = ui_pixel_label(page, "按确定重新开始",
                                            &ui_font_chinese_16, UI_TEXT_MUTED);
        lv_obj_set_pos(subtitle, 7, 128);
        lv_obj_set_width(subtitle, 188);
        lv_obj_set_style_text_align(subtitle, LV_TEXT_ALIGN_CENTER, 0);
        ui_pixel_mascot_create(s_exit_scr, 101, 238);
        lv_screen_load(s_exit_scr);
        return;
    }

    lv_obj_t *heading = ui_pixel_label(page, "确定退出？",
                                        &ui_font_chinese_16, UI_INK);
    lv_obj_set_pos(heading, 7, 10);
    lv_obj_set_width(heading, 188);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *subtitle = ui_pixel_label(page,
                                        demo_memory_score_persistent()
                                            ? "当前最高记录会保留"
                                            : "当前记录未保存",
                                        &ui_font_chinese_16, UI_TEXT_MUTED);
    lv_obj_set_pos(subtitle, 7, 38);
    lv_obj_set_width(subtitle, 188);
    lv_obj_set_style_text_align(subtitle, LV_TEXT_ALIGN_CENTER, 0);

    static const char *const choices[] = { "返回主菜单", "退出游戏" };
    for (uint8_t i = 0; i < 2; i++) {
        s_exit_cards[i] = ui_pixel_panel_create(page, 13, 86 + i * 59,
                                                176, 48, UI_CARD);
        lv_obj_t *label = ui_pixel_label(s_exit_cards[i], choices[i],
                                         &ui_font_chinese_16, UI_INK);
        lv_obj_set_width(label, 152);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(label);
    }
    lv_obj_t *hint = ui_pixel_label(page, "上下选择  确定执行",
                                    &ui_font_chinese_16, UI_TEXT_MUTED);
    lv_obj_set_pos(hint, 5, 236);
    lv_obj_set_width(hint, 192);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    exit_refresh_locked();
    lv_screen_load(s_exit_scr);
}

static void app_exit_enter(void)
{
    s_exit_selected = 0;
    s_exit_shutdown = false;
    app_exit_build(false);
}

static void app_exit_exit(void)
{
    shell_screen_delete(&s_exit_scr);
    for (uint8_t i = 0; i < 2; i++)
        s_exit_cards[i] = NULL;
}

static void app_exit_key(bsp_btn_t btn, bsp_btn_ev_t event)
{
    if (event != BSP_BTN_CLICK || !s_exit_scr)
        return;
    if (s_exit_shutdown) {
        if (btn == BSP_BTN_OK)
            leave_active_page();
        return;
    }
    if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
        s_exit_selected = (s_exit_selected + 1) % 2;
        if (bsp_lvgl_lock(250)) {
            exit_refresh_locked();
            bsp_lvgl_unlock();
        }
        return;
    }
    if (btn != BSP_BTN_OK)
        return;
    if (s_exit_selected == 0) {
        leave_active_page();
        return;
    }
    if (!bsp_lvgl_lock(500))
        return;
    app_exit_exit();
    s_exit_shutdown = true;
    app_exit_build(true);
    bsp_lvgl_unlock();
}

void app_main(void)
{
    ESP_LOGI(TAG, "Memory Passport starting");

    bsp_i2c_init();
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "Display/LVGL initialization failed (MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(s_brightness);

    const int64_t boot_screen_started_us = esp_timer_get_time();
    if (bsp_lvgl_lock(1000)) {
        boot_build();
        bsp_lvgl_unlock();
    }

    const esp_err_t input_error = input_dispatch_init();
    const esp_err_t button_error = input_error == ESP_OK
                                       ? bsp_button_init(on_key, NULL)
                                       : ESP_ERR_INVALID_STATE;
    const bool input_ok = input_error == ESP_OK && button_error == ESP_OK;
    const esp_err_t battery_error = bsp_battery_init();
    const bool battery_ok = battery_error == ESP_OK;
    const esp_err_t store_error = demo_memory_prepare();
    if (store_error != ESP_OK)
        ESP_LOGW(TAG, "memory score unavailable: %s", esp_err_to_name(store_error));
    demo_memory_configure(s_sound_enabled, s_difficulty);
    demo_navigation_init(&s_navigation, MENU_COUNT);
    for (size_t i = 0; i < MENU_COUNT - 1; i++)
        s_ok[i] = input_ok;
    s_ok[MENU_COUNT - 1] = true;

    const int64_t boot_elapsed_ms = (esp_timer_get_time() - boot_screen_started_us) / 1000;
    if (boot_elapsed_ms < 900)
        vTaskDelay(pdMS_TO_TICKS((uint32_t)(900 - boot_elapsed_ms)));

    if (bsp_lvgl_lock(1000)) {
        shell_screen_delete(&s_boot_scr);
        home_load();
        bsp_lvgl_unlock();
        s_input_ready = true;
    }

    ESP_LOGI(TAG,
             "ready: game=%d difficulty=%d settings=%d more=%d exit=%d battery=%d save=%d",
             s_ok[0], s_ok[1], s_ok[2], s_ok[3], s_ok[4], battery_ok,
             store_error == ESP_OK);
}
