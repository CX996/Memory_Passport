#include "memory_model.h"

#include <string.h>

#define MEMORY_DEFAULT_SEED 0xA341316CU

/* xorshift32 只负责提供可重复的本地序列，不承诺密码学随机性。 */
static uint32_t next_random(memory_model_t *model) {
    uint32_t value = model->rng;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    model->rng = value ? value : MEMORY_DEFAULT_SEED;
    return model->rng;
}

static memory_token_t next_token(memory_model_t *model, memory_token_t previous,
                                 bool has_previous) {
    memory_token_t token;
    do {
        token = (memory_token_t)(next_random(model) % MEMORY_TOKEN_COUNT);
    } while (has_previous && token == previous);
    return token;
}

/* 清除本轮时钟与 PRESS 去重临时状态；不修改序列或历史最佳。 */
static void clear_runtime(memory_model_t *model) {
    model->input_index = 0;
    model->deadline_ms = 0;
    model->press_pending = false;
    model->press_token = MEMORY_TOKEN_INVALID;
    model->press_time_ms = 0;
}

/* 将输入失败归约为稳定的 RESULT 状态；后续 tick 不会重复发失败事件。 */
static void enter_failure(memory_model_t *model, memory_failure_t failure) {
    model->state = MEMORY_STATE_RESULT;
    model->failure = failure;
    model->failed_at = model->sequence_len;
    clear_runtime(model);
}

static bool token_valid(memory_token_t token) {
    return token >= MEMORY_TOKEN_UP && token <= MEMORY_TOKEN_OK;
}

static bool click_is_pair(const memory_model_t *model, memory_token_t token, uint64_t now_ms) {
    if (!model->press_pending || model->press_token != token || now_ms < model->press_time_ms) {
        return false;
    }
    return now_ms - model->press_time_ms <= MEMORY_PRESS_CLICK_DEDUPE_MS;
}

/* 生成首轮固定长度序列；相邻 token 不重复。 */
static void generate_initial_sequence(memory_model_t *model) {
    model->sequence_len = MEMORY_INITIAL_LEVEL;
    for (uint8_t i = 0; i < model->sequence_len; i++) {
        const bool has_previous = i != 0;
        const memory_token_t previous =
            has_previous ? (memory_token_t)model->sequence[i - 1] : MEMORY_TOKEN_INVALID;
        model->sequence[i] = (uint8_t)next_token(model, previous, has_previous);
    }
}

/* 开启新局并保留 best_level；新局从长度 3 的 PLAYBACK 开始。 */
static void reset_session(memory_model_t *model) {
    generate_initial_sequence(model);
    model->clear_level = 0;
    model->failed_at = 0;
    model->failure = MEMORY_FAILURE_NONE;
    clear_runtime(model);
    model->state = MEMORY_STATE_PLAYBACK;
}

/* 结算完整复现，更新 RAM 成绩并进入短暂 SUCCESS 反馈。 */
static memory_event_t finish_success(memory_model_t *model, uint64_t now_ms) {
    model->clear_level = model->sequence_len;
    if (model->clear_level > model->best_level) {
        model->best_level = model->clear_level;
        model->best_dirty = true;
    }
    model->state = MEMORY_STATE_SUCCESS;
    model->failure = MEMORY_FAILURE_NONE;
    model->deadline_ms = now_ms + MEMORY_SUCCESS_HOLD_MS;
    model->press_pending = false;
    return MEMORY_EVENT_ROUND_SUCCESS;
}

/*
 * 初始化纯内存模型。可从任意任务调用，但调用期间 model 不得被其他任务并发修改。
 * seed=0 使用固定非零种子；persisted_best 越界时降为 0。
 */
void memory_model_init(memory_model_t *model, uint32_t seed, uint8_t persisted_best) {
    if (!model)
        return;

    memset(model, 0, sizeof(*model));
    model->rng = seed ? seed : MEMORY_DEFAULT_SEED;
    model->best_level = persisted_best <= MEMORY_MAX_LEVEL ? persisted_best : 0;
    model->state = MEMORY_STATE_READY;
    model->press_token = MEMORY_TOKEN_INVALID;
}

/* 非阻塞地从可重试状态开始一局；其他状态调用时返回 NONE。 */
memory_event_t memory_model_start(memory_model_t *model) {
    if (!model || (model->state != MEMORY_STATE_READY && model->state != MEMORY_STATE_RESULT &&
                   model->state != MEMORY_STATE_MAX_RESULT)) {
        return MEMORY_EVENT_NONE;
    }

    reset_session(model);
    return MEMORY_EVENT_STARTED;
}

/* 非阻塞地结束设备播放并开启输入超时窗口。 */
memory_event_t memory_model_playback_done(memory_model_t *model, uint64_t now_ms) {
    if (!model || model->state != MEMORY_STATE_PLAYBACK || model->sequence_len == 0) {
        return MEMORY_EVENT_NONE;
    }

    model->state = MEMORY_STATE_INPUT;
    model->failure = MEMORY_FAILURE_NONE;
    model->deadline_ms = now_ms + MEMORY_INPUT_TIMEOUT_MS;
    model->press_pending = false;
    model->press_token = MEMORY_TOKEN_INVALID;
    return MEMORY_EVENT_INPUT_READY;
}

/* 非阻塞地归约一个规范化按键事件；不访问硬件、UI 或存储。 */
memory_event_t memory_model_submit(memory_model_t *model, memory_token_t token,
                                   memory_input_event_t source, uint64_t now_ms) {
    if (!model || model->state != MEMORY_STATE_INPUT || !token_valid(token)) {
        return MEMORY_EVENT_NONE;
    }

    /* DOUBLE 永远是无效输入；即使调用者在超时边界送入它，也不结算一轮。 */
    if (source == MEMORY_INPUT_DOUBLE)
        return MEMORY_EVENT_NONE;
    if (source != MEMORY_INPUT_PRESS && source != MEMORY_INPUT_CLICK) {
        return MEMORY_EVENT_NONE;
    }

    /* OK PRESS 尚可能升级为全局 LONG；等 CLICK 才能确认它是一次短按。 */
    if (source == MEMORY_INPUT_PRESS && token == MEMORY_TOKEN_OK) {
        return MEMORY_EVENT_NONE;
    }

    if (model->deadline_ms != 0 && now_ms >= model->deadline_ms) {
        enter_failure(model, MEMORY_FAILURE_TIMEOUT);
        return MEMORY_EVENT_TIMEOUT;
    }

    if (source == MEMORY_INPUT_CLICK) {
        const bool duplicate = click_is_pair(model, token, now_ms);
        /* 任意 CLICK 都消费当前 PRESS 候选；不同 token 是新的独立输入。 */
        model->press_pending = false;
        if (duplicate)
            return MEMORY_EVENT_NONE;
    }

    if (token != memory_model_expected_token(model)) {
        enter_failure(model, MEMORY_FAILURE_WRONG);
        return MEMORY_EVENT_WRONG;
    }

    model->input_index++;
    if (source == MEMORY_INPUT_PRESS) {
        model->press_pending = true;
        model->press_token = token;
        model->press_time_ms = now_ms;
    }

    if (model->input_index >= model->sequence_len) {
        return finish_success(model, now_ms);
    }

    model->deadline_ms = now_ms + MEMORY_INPUT_TIMEOUT_MS;
    return MEMORY_EVENT_TOKEN_ACCEPTED;
}

/* 非阻塞时间推进；只在输入超时或成功反馈结束时产生一次事件。 */
memory_event_t memory_model_tick(memory_model_t *model, uint64_t now_ms) {
    if (!model)
        return MEMORY_EVENT_NONE;

    if (model->state == MEMORY_STATE_INPUT) {
        if (model->deadline_ms != 0 && now_ms >= model->deadline_ms) {
            enter_failure(model, MEMORY_FAILURE_TIMEOUT);
            return MEMORY_EVENT_TIMEOUT;
        }
        if (model->press_pending && now_ms >= model->press_time_ms &&
            now_ms - model->press_time_ms > MEMORY_PRESS_CLICK_DEDUPE_MS) {
            model->press_pending = false;
        }
        return MEMORY_EVENT_NONE;
    }

    if (model->state != MEMORY_STATE_SUCCESS || model->deadline_ms == 0 ||
        now_ms < model->deadline_ms) {
        return MEMORY_EVENT_NONE;
    }

    model->deadline_ms = 0;
    model->press_pending = false;
    if (model->sequence_len >= MEMORY_MAX_LEVEL) {
        model->state = MEMORY_STATE_MAX_RESULT;
        return MEMORY_EVENT_MAX_REACHED;
    }

    const memory_token_t previous = (memory_token_t)model->sequence[model->sequence_len - 1];
    model->sequence[model->sequence_len] = (uint8_t)next_token(model, previous, true);
    model->sequence_len++;
    model->input_index = 0;
    model->state = MEMORY_STATE_PLAYBACK;
    return MEMORY_EVENT_NEXT_ROUND;
}

/* 非阻塞地中止当前会话；不会清除或降低历史最佳成绩。 */
memory_event_t memory_model_back(memory_model_t *model) {
    if (!model || model->state == MEMORY_STATE_READY)
        return MEMORY_EVENT_NONE;

    model->state = MEMORY_STATE_READY;
    model->sequence_len = 0;
    model->clear_level = 0;
    model->failed_at = 0;
    model->failure = MEMORY_FAILURE_NONE;
    clear_runtime(model);
    return MEMORY_EVENT_ABORTED;
}

/* 只读查询序列项；不修改模型。 */
memory_token_t memory_model_sequence_at(const memory_model_t *model, uint8_t index) {
    if (!model || index >= model->sequence_len ||
        !token_valid((memory_token_t)model->sequence[index])) {
        return MEMORY_TOKEN_INVALID;
    }
    return (memory_token_t)model->sequence[index];
}

/* 只读查询当前输入阶段期望的 token。 */
memory_token_t memory_model_expected_token(const memory_model_t *model) {
    if (!model || model->state != MEMORY_STATE_INPUT || model->input_index >= model->sequence_len) {
        return MEMORY_TOKEN_INVALID;
    }
    return memory_model_sequence_at(model, model->input_index);
}

bool memory_model_best_needs_save(const memory_model_t *model) {
    return model && model->best_dirty;
}

void memory_model_mark_best_saved(memory_model_t *model, uint8_t saved_level) {
    if (model && model->best_dirty && model->best_level == saved_level) {
        model->best_dirty = false;
    }
}
