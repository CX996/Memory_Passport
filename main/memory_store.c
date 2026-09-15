#include "memory_store.h"

#include "esp_log.h"
#include "memory_model.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stdbool.h>

static const char *TAG = "memory_store";

#define MEMORY_NVS_NAMESPACE "memory"
#define MEMORY_NVS_KEY_BEST "best_level"

static bool s_nvs_ready;

/*
 * 初始化默认 NVS 分区。函数可能阻塞，且不擦除任何数据；成功后可重复调用。
 * 调用方需串行化首次初始化，不能从按键/esp_timer 回调调用。
 */
esp_err_t memory_store_init(void) {
    if (s_nvs_ready)
        return ESP_OK;

    esp_err_t error = nvs_flash_init();
    if (error != ESP_OK) {
        /* 不调用 nvs_flash_erase：该分区可能包含其他功能的用户数据。 */
        ESP_LOGE(TAG, "NVS 初始化失败: %s; 不自动擦除分区", esp_err_to_name(error));
        return error;
    }
    s_nvs_ready = true;
    return ESP_OK;
}

/*
 * 读取一个 uint8 成绩；key 不存在时返回 ESP_OK 且输出 0。
 * 值损坏或越界时输出仍为 0，并返回错误供 UI 显示 SAVE OFF。
 */
esp_err_t memory_store_load_best(uint8_t *best_level) {
    if (!best_level)
        return ESP_ERR_INVALID_ARG;
    *best_level = 0;

    esp_err_t error = memory_store_init();
    if (error != ESP_OK)
        return error;

    nvs_handle_t handle;
    error = nvs_open(MEMORY_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND)
        return ESP_OK;
    if (error != ESP_OK)
        return error;

    uint8_t value = 0;
    error = nvs_get_u8(handle, MEMORY_NVS_KEY_BEST, &value);
    nvs_close(handle);
    if (error == ESP_ERR_NVS_NOT_FOUND)
        return ESP_OK;
    if (error != ESP_OK)
        return error;
    if (value > MEMORY_MAX_LEVEL) {
        ESP_LOGW(TAG, "best_level 越界(%u), 按 0 处理", (unsigned)value);
        return ESP_ERR_INVALID_STATE;
    }

    *best_level = value;
    return ESP_OK;
}

/*
 * 写入并立即 commit 一个 uint8 成绩。函数同步阻塞，不拥有调用者内存；
 * 只应由 worker task 在模型 best_dirty 时调用。
 */
esp_err_t memory_store_save_best(uint8_t best_level) {
    if (best_level > MEMORY_MAX_LEVEL)
        return ESP_ERR_INVALID_ARG;

    esp_err_t error = memory_store_init();
    if (error != ESP_OK)
        return error;

    nvs_handle_t handle;
    error = nvs_open(MEMORY_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK)
        return error;

    error = nvs_set_u8(handle, MEMORY_NVS_KEY_BEST, best_level);
    if (error == ESP_OK)
        error = nvs_commit(handle);
    nvs_close(handle);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "保存 best_level=%u 失败: %s", (unsigned)best_level, esp_err_to_name(error));
    }
    return error;
}
