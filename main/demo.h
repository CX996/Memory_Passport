// main/demo.h —— 每个产品页面实现的统一接口。
// 新增页面 = 实现 enter/exit/key，慢服务按需实现 start/stop，再注册到 MENU[]。
#pragma once

#include "bsp_button.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *name;
    void (*enter)(void);                          // 持 LVGL 锁创建并载入页面
    void (*exit)(void);                           // lifecycle stop 成功后,持 LVGL 锁删除页面
    void (*key)(bsp_btn_t btn, bsp_btn_ev_t ev);  // lifecycle task 调用;函数自行缩短 LVGL 锁范围
    esp_err_t (*start)(void);                     // 可选:页面创建后,不持 LVGL 锁启动慢服务
    esp_err_t (*stop)(void);                      // 可选:删页面前,不持 LVGL 锁停止 producer
} demo_entry_t;

// 各演示页(定义在各自的 .c 里)
void demo_memory_enter(void);  void demo_memory_exit(void);
void demo_memory_key(bsp_btn_t btn, bsp_btn_ev_t ev);
esp_err_t demo_memory_start(void); esp_err_t demo_memory_stop(void);
esp_err_t demo_memory_prepare(void);
uint8_t demo_memory_best_level(void);
bool demo_memory_score_persistent(void);
void demo_memory_configure(bool sound_enabled, uint8_t difficulty);

enum {
    MEMORY_DIFFICULTY_EASY = 0,
    MEMORY_DIFFICULTY_NORMAL,
    MEMORY_DIFFICULTY_CHALLENGE,
    MEMORY_DIFFICULTY_COUNT,
};

void demo_settings_enter(void); void demo_settings_exit(void);
void demo_settings_key(bsp_btn_t btn, bsp_btn_ev_t ev);
