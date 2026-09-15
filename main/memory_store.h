#pragma once

#include "esp_err.h"

#include <stdint.h>

/*
 * 只保存历史最高完整复现长度。所有函数可能阻塞于 NVS，不能从按键回调调用。
 * NVS 初始化失败时绝不自动擦除分区，调用方可让训练继续使用 RAM 成绩。
 */
esp_err_t memory_store_init(void);
esp_err_t memory_store_load_best(uint8_t *best_level);
esp_err_t memory_store_save_best(uint8_t best_level);
