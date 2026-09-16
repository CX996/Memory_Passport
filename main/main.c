// Memory Passport application shell: boot splash, home, settings, and memory game.
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_battery.h"
#include "bsp_pins.h"
#include "demo.h"
#include "demo_navigation.h"
#include "ui_pixel.h"

#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stddef.h>
#include <time.h>

static const char *TAG = "main";

static void app_exit_enter(void);
static void app_exit_exit(void);
static void app_exit_key(bsp_btn_t btn, bsp_btn_ev_t event);

static const demo_entry_t MENU[] = {
    { .name = "Memory", .enter = demo_memory_enter, .exit = demo_memory_exit,
      .key = demo_memory_key, .start = demo_memory_start, .stop = demo_memory_stop },
    { .name = "Settings", .enter = demo_settings_enter, .exit = demo_settings_exit,
      .key = demo_settings_key },
    { .name = "Exit", .enter = app_exit_enter, .exit = app_exit_exit,
      .key = app_exit_key },
};
#define MENU_COUNT (sizeof(MENU) / sizeof(MENU[0]))
#define INPUT_QUEUE_DEPTH 8

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static bool s_ok[MENU_COUNT];
static lv_obj_t *s_home_scr;
static lv_obj_t *s_home_cards[MENU_COUNT];
static lv_obj_t *s_home_rows[MENU_COUNT];
static lv_obj_t *s_home_clock;
static lv_obj_t *s_home_battery;
static lv_obj_t *s_boot_scr;
static lv_obj_t *s_exit_scr;
static lv_timer_t *s_home_timer;
static demo_navigation_t s_navigation;
static QueueHandle_t s_input_queue;
static TaskHandle_t s_input_task;
static volatile bool s_input_ready;
static int64_t s_clock_boot_us;

static uint8_t s_brightness = 100;
static bool s_sound_enabled = true;
static uint8_t s_pace = MEMORY_PACE_NORMAL;

static lv_obj_t *s_settings_scr;
static lv_obj_t *s_settings_cards[3];
static lv_obj_t *s_settings_values[3];
static lv_obj_t *s_settings_clock;
static lv_obj_t *s_settings_battery;
static lv_timer_t *s_settings_timer;
static uint8_t s_settings_selected;

static void set_status_labels(lv_obj_t *clock, lv_obj_t *battery);
static void settings_refresh_locked(void);
static void settings_refresh(void)
{
    if (!bsp_lvgl_lock(250)) return;
    settings_refresh_locked();
    bsp_lvgl_unlock();
}
static void settings_tick(lv_timer_t *timer)
{
    (void)timer;
    set_status_labels(s_settings_clock, s_settings_battery);
}

void app_clock_text(char text[6])
{
    if (!text) return;

    time_t wall_clock = time(NULL);
    struct tm local = { 0 };
    if (wall_clock >= 1700000000 && localtime_r(&wall_clock, &local)) {
        snprintf(text, 6, "%02d:%02d", local.tm_hour, local.tm_min);
        return;
    }

    /* No RTC/NTP service is configured yet: show elapsed device time, never a fake date. */
    uint64_t elapsed = s_clock_boot_us > 0
                     ? ((uint64_t)esp_timer_get_time() - (uint64_t)s_clock_boot_us) / 1000000U
                     : 0;
    const int minutes = (int)(elapsed / 60U) % (24 * 60);
    snprintf(text, 6, "%02d:%02d", minutes / 60, minutes % 60);
}

static void set_status_labels(lv_obj_t *clock, lv_obj_t *battery)
{
    char clock_text[6];
    app_clock_text(clock_text);
    if (clock) lv_label_set_text(clock, clock_text);

    if (!battery) return;
    const int soc = bsp_battery_soc();
    if (soc < 0) {
        lv_label_set_text(battery, "--%");
        lv_obj_set_style_text_color(battery, lv_color_hex(UI_MUTED), 0);
    } else {
        lv_label_set_text_fmt(battery, "%d%%", soc);
        lv_obj_set_style_text_color(battery,
                                    lv_color_hex(soc < 20 ? UI_RED : UI_PAPER), 0);
    }
}

static void home_tick(lv_timer_t *timer)
{
    (void)timer;
    set_status_labels(s_home_clock, s_home_battery);
}

static void home_refresh(void)
{
    for (size_t i = 0; i < MENU_COUNT; i++) {
        lv_label_set_text_fmt(s_home_rows[i], "%s", MENU[i].name);
        ui_pixel_set_selected(s_home_cards[i], i == s_navigation.selected, s_ok[i]);
    }
}

static void home_build(void)
{
    s_home_scr = ui_pixel_screen_create("MEMORY");
    s_home_clock = ui_pixel_label(s_home_scr, "--:--", &lv_font_montserrat_14, UI_PAPER);
    lv_obj_set_pos(s_home_clock, 158, 28);
    lv_obj_set_width(s_home_clock, 44);
    s_home_battery = ui_pixel_label(s_home_scr, "--%", &lv_font_montserrat_14, UI_PAPER);
    lv_obj_set_pos(s_home_battery, 202, 28);
    lv_obj_set_width(s_home_battery, 36);
    lv_obj_set_style_text_align(s_home_battery, LV_TEXT_ALIGN_RIGHT, 0);

    static const char *const names[MENU_COUNT] = { "Memory", "Settings", "Exit" };
    static const char *const hints[MENU_COUNT] = { "START", "OPEN", "POWER" };
    for (size_t i = 0; i < MENU_COUNT; i++) {
        const bool primary = i == 0;
        const int x = primary ? 12 : 156;
        const int y = primary ? 78 : 78 + ((int)i - 1) * 62;
        const int width = primary ? 136 : 72;
        const int height = primary ? 106 : 48;
        s_home_cards[i] = ui_pixel_panel_create(s_home_scr, x, y, width, height, UI_PAPER);
        s_home_rows[i] = ui_pixel_label(s_home_cards[i], names[i],
                                        primary ? &lv_font_montserrat_20 : &lv_font_montserrat_14,
                                        UI_INK);
        lv_obj_set_width(s_home_rows[i], width - 16);
        lv_obj_set_style_text_align(s_home_rows[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(s_home_rows[i], LV_ALIGN_TOP_MID, 0, primary ? 20 : 5);
        lv_obj_t *hint = ui_pixel_label(s_home_cards[i], hints[i],
                                        &lv_font_montserrat_14, UI_SKY_DARK);
        lv_obj_set_width(hint, width - 16);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, primary ? -18 : -5);
    }

    ui_pixel_mascot_create(s_home_scr, 101, 238);
    set_status_labels(s_home_clock, s_home_battery);
    s_home_timer = lv_timer_create(home_tick, 1000, NULL);
    home_refresh();
    lv_screen_load(s_home_scr);
}

static void home_delete(void)
{
    if (s_home_timer) {
        lv_timer_delete(s_home_timer);
        s_home_timer = NULL;
    }
    if (s_home_scr) lv_obj_delete(s_home_scr);
    s_home_scr = s_home_clock = s_home_battery = NULL;
    for (size_t i = 0; i < MENU_COUNT; i++)
        s_home_cards[i] = s_home_rows[i] = NULL;
}

static void home_load(void)
{
    home_build();
}

static void boot_build(void)
{
    s_boot_scr = ui_pixel_screen_create("MEMORY");
    lv_obj_t *panel = ui_pixel_panel_create(s_boot_scr, 18, 76, 204, 126, UI_PAPER);
    lv_obj_t *name = ui_pixel_label(panel, "MEMORY\nPASSPORT",
                                    &lv_font_montserrat_20, UI_INK);
    lv_obj_set_width(name, 180);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_t *ready = ui_pixel_label(panel, "READY", &lv_font_montserrat_14, UI_SKY_DARK);
    lv_obj_align(ready, LV_ALIGN_BOTTOM_MID, 0, -16);
    ui_pixel_mascot_create(s_boot_scr, 101, 238);
    lv_screen_load(s_boot_scr);
}

static demo_nav_input_t navigation_input(bsp_btn_t btn, bsp_btn_ev_t event)
{
    if (event == BSP_BTN_LONG && btn == BSP_BTN_OK) return DEMO_NAV_INPUT_OK_LONG;
    if (event != BSP_BTN_CLICK) return DEMO_NAV_INPUT_OTHER;
    if (btn == BSP_BTN_UP) return DEMO_NAV_INPUT_UP_CLICK;
    if (btn == BSP_BTN_DOWN) return DEMO_NAV_INPUT_DOWN_CLICK;
    if (btn == BSP_BTN_OK) return DEMO_NAV_INPUT_OK_CLICK;
    return DEMO_NAV_INPUT_OTHER;
}

static void process_input(const input_event_t *input)
{
    const demo_nav_input_t nav_input = navigation_input(input->btn, input->event);
    if (s_navigation.active >= 0) {
        const demo_entry_t *active_demo = &MENU[s_navigation.active];
        const demo_nav_result_t result = demo_navigation_handle(&s_navigation, nav_input, true);
        if (result.action == DEMO_NAV_ACTION_EXIT) {
            const esp_err_t error = active_demo->stop ? active_demo->stop() : ESP_OK;
            if (error != ESP_OK) {
                ESP_LOGE(TAG, "%s stop failed: %s", active_demo->name, esp_err_to_name(error));
                return;
            }
            if (!bsp_lvgl_lock(500)) return;
            active_demo->exit();
            demo_navigation_complete_exit(&s_navigation);
            home_load();
            bsp_lvgl_unlock();
        } else if (result.action == DEMO_NAV_ACTION_FORWARD) {
            active_demo->key(input->btn, input->event);
        }
        return;
    }

    if (nav_input == DEMO_NAV_INPUT_OTHER || nav_input == DEMO_NAV_INPUT_OK_LONG) return;
    if (!bsp_lvgl_lock(500)) return;
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
    if (!s_input_queue) return ESP_ERR_NO_MEM;
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
    if (!s_input_ready || !s_input_queue) return;
    const input_event_t input = { .btn = btn, .event = event };
    (void)xQueueSend(s_input_queue, &input, 0);
}

void demo_settings_enter(void)
{
    s_settings_selected = 0;
    s_settings_scr = ui_pixel_screen_create("SETTINGS");
    s_settings_clock = ui_pixel_label(s_settings_scr, "--:--", &lv_font_montserrat_14, UI_PAPER);
    lv_obj_set_pos(s_settings_clock, 158, 28);
    s_settings_battery = ui_pixel_label(s_settings_scr, "--%", &lv_font_montserrat_14, UI_PAPER);
    lv_obj_set_pos(s_settings_battery, 202, 28);
    lv_obj_set_width(s_settings_battery, 36);
    lv_obj_set_style_text_align(s_settings_battery, LV_TEXT_ALIGN_RIGHT, 0);

    static const char *const names[] = { "BRIGHTNESS", "SOUND", "PACE" };
    for (int i = 0; i < 3; i++) {
        s_settings_cards[i] = ui_pixel_panel_create(s_settings_scr, 20, 62 + i * 58,
                                                     200, 44, UI_PAPER);
        lv_obj_t *label = ui_pixel_label(s_settings_cards[i], names[i],
                                         &lv_font_montserrat_14, UI_INK);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 0);
        s_settings_values[i] = ui_pixel_label(s_settings_cards[i], "",
                                              &lv_font_montserrat_14, UI_SKY_DARK);
        lv_obj_set_width(s_settings_values[i], 82);
        lv_obj_set_style_text_align(s_settings_values[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(s_settings_values[i], LV_ALIGN_RIGHT_MID, -10, 0);
    }
    lv_obj_t *hint = ui_pixel_label(s_settings_scr, "UP/DOWN SELECT  OK CHANGE",
                                    &lv_font_montserrat_14, UI_PAPER);
    lv_obj_set_pos(hint, 18, 254);
    s_settings_timer = lv_timer_create(settings_tick, 1000, NULL);
    settings_refresh_locked();
    lv_screen_load(s_settings_scr);
}

void demo_settings_exit(void)
{
    if (s_settings_timer) {
        lv_timer_delete(s_settings_timer);
        s_settings_timer = NULL;
    }
    if (s_settings_scr) lv_obj_delete(s_settings_scr);
    s_settings_scr = s_settings_clock = s_settings_battery = NULL;
    for (int i = 0; i < 3; i++) s_settings_cards[i] = s_settings_values[i] = NULL;
}

static void settings_refresh_locked(void)
{
    static const char *const pace_names[] = { "SLOW", "NORMAL", "FAST" };
    lv_label_set_text_fmt(s_settings_values[0], "%u%%", (unsigned)s_brightness);
    lv_label_set_text(s_settings_values[1], s_sound_enabled ? "ON" : "OFF");
    lv_label_set_text(s_settings_values[2], pace_names[s_pace]);
    for (int i = 0; i < 3; i++)
        ui_pixel_set_selected(s_settings_cards[i], i == s_settings_selected, true);
}

void demo_settings_key(bsp_btn_t btn, bsp_btn_ev_t event)
{
    if (event != BSP_BTN_CLICK || !s_settings_scr) return;
    if (btn == BSP_BTN_UP) {
        s_settings_selected = (s_settings_selected + 2) % 3;
    } else if (btn == BSP_BTN_DOWN) {
        s_settings_selected = (s_settings_selected + 1) % 3;
    } else if (btn == BSP_BTN_OK) {
        static const uint8_t levels[] = { 25, 50, 75, 100 };
        if (s_settings_selected == 0) {
            size_t current = 0;
            for (size_t i = 0; i < sizeof(levels); i++)
                if (levels[i] == s_brightness) current = i;
            s_brightness = levels[(current + 1) % (sizeof(levels) / sizeof(levels[0]))];
            bsp_display_backlight(s_brightness);
        } else if (s_settings_selected == 1) {
            s_sound_enabled = !s_sound_enabled;
        } else {
            s_pace = (s_pace + 1) % MEMORY_PACE_COUNT;
        }
        demo_memory_configure(s_sound_enabled, s_pace);
    } else {
        return;
    }
    settings_refresh();
}

static void app_exit_enter(void)
{
    s_exit_scr = ui_pixel_screen_create("EXIT");
    lv_obj_t *panel = ui_pixel_panel_create(s_exit_scr, 18, 76, 204, 126, UI_PAPER);
    lv_obj_t *text = ui_pixel_label(panel, "SAFE TO POWER OFF\n\nUSE POWER KEY",
                                    &lv_font_montserrat_14, UI_INK);
    lv_obj_set_width(text, 180);
    lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(text, LV_ALIGN_TOP_MID, 0, 20);
    ui_pixel_mascot_create(s_exit_scr, 101, 238);
    lv_screen_load(s_exit_scr);
}

static void app_exit_exit(void)
{
    if (s_exit_scr) lv_obj_delete(s_exit_scr);
    s_exit_scr = NULL;
}

static void app_exit_key(bsp_btn_t btn, bsp_btn_ev_t event)
{
    (void)btn;
    (void)event;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Memory Passport starting");
    s_clock_boot_us = esp_timer_get_time();

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
                                 ? bsp_button_init(on_key, NULL) : ESP_ERR_INVALID_STATE;
    const bool input_ok = input_error == ESP_OK && button_error == ESP_OK;
    const esp_err_t battery_error = bsp_battery_init();
    const bool battery_ok = battery_error == ESP_OK;
    demo_memory_configure(s_sound_enabled, s_pace);
    demo_navigation_init(&s_navigation, MENU_COUNT);
    s_ok[0] = input_ok;
    s_ok[1] = input_ok;
    s_ok[2] = true;

    const int64_t boot_elapsed_ms =
        (esp_timer_get_time() - boot_screen_started_us) / 1000;
    if (boot_elapsed_ms < 900)
        vTaskDelay(pdMS_TO_TICKS((uint32_t)(900 - boot_elapsed_ms)));

    if (bsp_lvgl_lock(1000)) {
        if (s_boot_scr) lv_obj_delete(s_boot_scr);
        s_boot_scr = NULL;
        home_load();
        bsp_lvgl_unlock();
        s_input_ready = true;
    }

    ESP_LOGI(TAG, "ready: memory=%d settings=%d exit=%d battery=%d",
             s_ok[0], s_ok[1], s_ok[2], battery_ok);
}
