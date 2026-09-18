#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * 记忆训练模型的固定产品边界。
 * 序列只使用三个 token，容量固定为 12，避免在 ESP32-C3 上引入动态内存。
 */
#define MEMORY_EASY_START_LEVEL 2
#define MEMORY_INITIAL_LEVEL 3
#define MEMORY_CHALLENGE_START_LEVEL 4
#define MEMORY_MAX_LEVEL 12
#define MEMORY_TOKEN_COUNT 3
#define MEMORY_EASY_INPUT_TIMEOUT_MS 7000
#define MEMORY_INPUT_TIMEOUT_MS 5000
#define MEMORY_CHALLENGE_INPUT_TIMEOUT_MS 4000
#define MEMORY_PRESS_CLICK_DEDUPE_MS 1500
#define MEMORY_SUCCESS_HOLD_MS 900

typedef enum {
    MEMORY_TOKEN_UP = 0,
    MEMORY_TOKEN_DOWN,
    MEMORY_TOKEN_OK,
    MEMORY_TOKEN_INVALID = 0xFF,
} memory_token_t;

typedef enum {
    MEMORY_STATE_READY = 0,
    MEMORY_STATE_PLAYBACK,
    MEMORY_STATE_INPUT,
    MEMORY_STATE_SUCCESS,
    MEMORY_STATE_RESULT,
    MEMORY_STATE_MAX_RESULT,
} memory_state_t;

typedef enum {
    MEMORY_FAILURE_NONE = 0,
    MEMORY_FAILURE_WRONG,
    MEMORY_FAILURE_TIMEOUT,
} memory_failure_t;

/* 输入来源用于在模型边界完成 PRESS + CLICK 去重。 */
typedef enum {
    MEMORY_INPUT_PRESS = 0,
    MEMORY_INPUT_CLICK,
    MEMORY_INPUT_DOUBLE,
} memory_input_event_t;

typedef enum {
    MEMORY_EVENT_NONE = 0,
    MEMORY_EVENT_STARTED,
    MEMORY_EVENT_INPUT_READY,
    MEMORY_EVENT_TOKEN_ACCEPTED,
    MEMORY_EVENT_ROUND_SUCCESS,
    MEMORY_EVENT_NEXT_ROUND,
    MEMORY_EVENT_WRONG,
    MEMORY_EVENT_TIMEOUT,
    MEMORY_EVENT_MAX_REACHED,
    MEMORY_EVENT_ABORTED,
} memory_event_t;

/*
 * 公开结构体便于 demo 直接读取少量状态；数组容量固定，不拥有外部内存。
 * deadline_ms 使用单调毫秒时间；READY/PLAYBACK/RESULT 等无计时状态为 0。
 */
typedef struct {
    uint8_t sequence[MEMORY_MAX_LEVEL];
    uint8_t sequence_len;
    uint8_t input_index;
    uint8_t clear_level;
    uint8_t best_level;
    uint8_t failed_at;
    uint8_t start_level;
    memory_state_t state;
    memory_failure_t failure;
    uint64_t deadline_ms;
    uint32_t rng;
    uint32_t input_timeout_ms;
    bool best_dirty;

    /* 仅用于把一次 PRESS 产生的配对 CLICK 丢弃，不跨会话持久化。 */
    bool press_pending;
    memory_token_t press_token;
    uint64_t press_time_ms;
} memory_model_t;

/* 初始化模型；persisted_best 超出 0..12 时按安全默认值 0 处理。 */
void memory_model_init(memory_model_t *model, uint32_t seed, uint8_t persisted_best);

/* 配置三档难度；非 2/7000、3/5000、4/4000 的组合回退到标准档。 */
void memory_model_configure(memory_model_t *model, uint8_t start_level,
                            uint32_t input_timeout_ms);

/* 从 READY/RESULT/MAX_RESULT 开始新会话，按配置长度生成序列并进入 PLAYBACK。 */
memory_event_t memory_model_start(memory_model_t *model);

/* 播放器完成当前序列后调用，进入 INPUT 并开启首个 token 的超时窗口。 */
memory_event_t memory_model_playback_done(memory_model_t *model, uint64_t now_ms);

/*
 * 提交一个原始按键事件。UP/DOWN 优先 PRESS 并去掉配对 CLICK；OK 只在 CLICK
 * 时确认，使可能升级为全局 LONG 的 OK PRESS 不会提前留下答案；DOUBLE 忽略。
 */
memory_event_t memory_model_submit(memory_model_t *model, memory_token_t token,
                                   memory_input_event_t source, uint64_t now_ms);

/* 推进输入超时或成功反馈计时。重复调用是幂等的。 */
memory_event_t memory_model_tick(memory_model_t *model, uint64_t now_ms);

/* 放弃当前会话并回到 READY；历史最高成绩保留。 */
memory_event_t memory_model_back(memory_model_t *model);

/* 序列查询；越界或非有效 token 返回 MEMORY_TOKEN_INVALID。 */
memory_token_t memory_model_sequence_at(const memory_model_t *model, uint8_t index);
memory_token_t memory_model_expected_token(const memory_model_t *model);

/* 成绩写入协作：模型只标记 dirty，存储层成功写入后由调用方清除。 */
bool memory_model_best_needs_save(const memory_model_t *model);

/* 仅当 saved_level 仍是当前最高分时清除 dirty，避免旧保存回执覆盖新纪录。 */
void memory_model_mark_best_saved(memory_model_t *model, uint8_t saved_level);
