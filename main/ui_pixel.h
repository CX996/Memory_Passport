#pragma once

#include "lvgl.h"

LV_FONT_DECLARE(ui_font_chinese_16);

#define UI_SKY        0x1689E8
#define UI_SKY_DARK   0x0872C9
#define UI_INK        0x17202A
#define UI_PAPER      0xF8F5DF
#define UI_CARD       0xE8F2EC
#define UI_COPY       0xDFF2FF
#define UI_TEXT_MUTED 0x486273
#define UI_GRASS      0x82BE2D
#define UI_GRASS_DARK 0x55951D
#define UI_YELLOW     0xFFD928
#define UI_ORANGE     0xFFB23E
#define UI_RED        0xE43B2F
#define UI_MUTED      0xD9E7EC
#define UI_SUCCESS    0xEFFFD4
#define UI_RESULT     0xFFF0E7
#define UI_KEY_UP     0xBFE5F8
#define UI_KEY_DOWN   0xFFD18E
#define UI_KEY_OK     0xFFE879

lv_obj_t *ui_pixel_screen_create(const char *title);
lv_obj_t *ui_pixel_panel_create(lv_obj_t *parent, int x, int y, int w, int h,
                                uint32_t color);
lv_obj_t *ui_pixel_label(lv_obj_t *parent, const char *text,
                         const lv_font_t *font, uint32_t color);
lv_obj_t *ui_pixel_mascot_create(lv_obj_t *parent, int x, int y);
void ui_pixel_mascot_jump(lv_obj_t *mascot);
void ui_pixel_set_selected(lv_obj_t *panel, bool selected, bool enabled);
