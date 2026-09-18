#include "memory_model.h"

#include <assert.h>
#include <stdio.h>

static void assert_sequence_valid(const memory_model_t *model) {
    assert(model->sequence_len >= MEMORY_EASY_START_LEVEL);
    assert(model->sequence_len <= MEMORY_MAX_LEVEL);
    for (uint8_t i = 0; i < model->sequence_len; i++) {
        assert(memory_model_sequence_at(model, i) != MEMORY_TOKEN_INVALID);
    }
}

static void complete_click_round(memory_model_t *model, uint64_t start_ms) {
    assert(model->state == MEMORY_STATE_PLAYBACK);
    assert(memory_model_playback_done(model, start_ms) == MEMORY_EVENT_INPUT_READY);
    for (uint8_t i = 0; i < model->sequence_len; i++) {
        const memory_token_t expected = memory_model_sequence_at(model, i);
        const memory_event_t event = memory_model_submit(model, expected, MEMORY_INPUT_CLICK,
                                                         start_ms + (uint64_t)i * 10U + 1U);
        if (i + 1U < model->sequence_len) {
            assert(event == MEMORY_EVENT_TOKEN_ACCEPTED);
        } else {
            assert(event == MEMORY_EVENT_ROUND_SUCCESS);
        }
    }
}

static void test_initial_state_and_generation(void) {
    memory_model_t model;
    memory_model_init(&model, 0, 99);
    assert(model.state == MEMORY_STATE_READY);
    assert(model.best_level == 0);
    assert(!memory_model_best_needs_save(&model));
    assert(memory_model_start(&model) == MEMORY_EVENT_STARTED);
    assert(model.state == MEMORY_STATE_PLAYBACK);
    assert(model.sequence_len == MEMORY_INITIAL_LEVEL);
    assert_sequence_valid(&model);
    assert(memory_model_expected_token(&model) == MEMORY_TOKEN_INVALID);
}

static void test_difficulty_configuration(void) {
    static const struct {
        uint8_t start_level;
        uint32_t timeout_ms;
    } cases[] = {
        {MEMORY_EASY_START_LEVEL, MEMORY_EASY_INPUT_TIMEOUT_MS},
        {MEMORY_INITIAL_LEVEL, MEMORY_INPUT_TIMEOUT_MS},
        {MEMORY_CHALLENGE_START_LEVEL, MEMORY_CHALLENGE_INPUT_TIMEOUT_MS},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        memory_model_t model;
        memory_model_init(&model, 100U + (uint32_t)i, 0);
        memory_model_configure(&model, cases[i].start_level, cases[i].timeout_ms);
        assert(model.start_level == cases[i].start_level);
        assert(model.input_timeout_ms == cases[i].timeout_ms);
        assert(memory_model_start(&model) == MEMORY_EVENT_STARTED);
        assert(model.sequence_len == cases[i].start_level);
        assert_sequence_valid(&model);
        assert(memory_model_playback_done(&model, 100) == MEMORY_EVENT_INPUT_READY);
        assert(model.deadline_ms == 100U + cases[i].timeout_ms);
        assert(memory_model_submit(&model, memory_model_expected_token(&model),
                                   MEMORY_INPUT_CLICK, 200) == MEMORY_EVENT_TOKEN_ACCEPTED);
        assert(model.deadline_ms == 200U + cases[i].timeout_ms);
    }

    memory_model_t fallback;
    memory_model_init(&fallback, 200, 0);
    memory_model_configure(&fallback, MEMORY_EASY_START_LEVEL, MEMORY_INPUT_TIMEOUT_MS);
    assert(fallback.start_level == MEMORY_INITIAL_LEVEL);
    assert(fallback.input_timeout_ms == MEMORY_INPUT_TIMEOUT_MS);
    assert(memory_model_start(&fallback) == MEMORY_EVENT_STARTED);
    assert(fallback.sequence_len == MEMORY_INITIAL_LEVEL);
}

static void test_press_click_dedupe_and_click_fallback(void) {
    memory_model_t model;
    memory_model_init(&model, 1234, 0);
    assert(memory_model_start(&model) == MEMORY_EVENT_STARTED);
    assert(memory_model_playback_done(&model, 100) == MEMORY_EVENT_INPUT_READY);

    for (uint8_t i = 0; i < MEMORY_INITIAL_LEVEL; i++) {
        const memory_token_t expected = memory_model_sequence_at(&model, i);
        const uint64_t now = 200U + (uint64_t)i * 200U;
        const memory_event_t press = memory_model_submit(&model, expected, MEMORY_INPUT_PRESS, now);
        const memory_event_t wanted = i + 1U < MEMORY_INITIAL_LEVEL ? MEMORY_EVENT_TOKEN_ACCEPTED
                                                                    : MEMORY_EVENT_ROUND_SUCCESS;
        if (expected == MEMORY_TOKEN_OK) {
            assert(press == MEMORY_EVENT_NONE);
            assert(memory_model_submit(&model, expected, MEMORY_INPUT_CLICK, now + 100U) == wanted);
        } else {
            assert(press == wanted);
            assert(memory_model_submit(&model, expected, MEMORY_INPUT_CLICK, now + 100U) ==
                   MEMORY_EVENT_NONE);
        }
    }
    assert(model.clear_level == 3);
    assert(model.best_level == 3);
    assert(memory_model_best_needs_save(&model));

    assert(memory_model_tick(&model, model.deadline_ms - 1U) == MEMORY_EVENT_NONE);
    /* 下一关必须覆盖整段序列，而不是沿用旧序列前缀。 */
    for (uint8_t i = 0; i < model.sequence_len; i++)
        model.sequence[i] = MEMORY_TOKEN_INVALID;
    assert(memory_model_tick(&model, model.deadline_ms) == MEMORY_EVENT_NEXT_ROUND);
    assert(model.sequence_len == 4);
    assert_sequence_valid(&model);

    /* 没有 PRESS 时，连续 CLICK 都是合法输入，不能被错误吞掉。 */
    complete_click_round(&model, 2000);
    assert(model.clear_level == 4);
    assert(model.best_level == 4);

    memory_model_mark_best_saved(&model, 3);
    assert(memory_model_best_needs_save(&model));
    memory_model_mark_best_saved(&model, model.best_level);
    assert(!memory_model_best_needs_save(&model));
    assert(memory_model_submit(&model, MEMORY_TOKEN_UP, MEMORY_INPUT_DOUBLE, 3000) ==
           MEMORY_EVENT_NONE);
}

static void test_each_round_is_fresh_random_sequence(void) {
    memory_model_t model;
    memory_model_init(&model, 12345, 0);
    assert(memory_model_start(&model) == MEMORY_EVENT_STARTED);
    assert(memory_model_playback_done(&model, 100) == MEMORY_EVENT_INPUT_READY);

    uint8_t first[MEMORY_INITIAL_LEVEL];
    for (uint8_t i = 0; i < MEMORY_INITIAL_LEVEL; i++)
        first[i] = model.sequence[i];
    for (uint8_t i = 0; i < MEMORY_INITIAL_LEVEL; i++)
        assert(memory_model_submit(&model, (memory_token_t)first[i], MEMORY_INPUT_CLICK,
                                   200U + i) == (i + 1U == MEMORY_INITIAL_LEVEL
                                                     ? MEMORY_EVENT_ROUND_SUCCESS
                                                     : MEMORY_EVENT_TOKEN_ACCEPTED));

    /* 污染旧序列，确保下一关会完整重生成，而不是只补写新增尾项。 */
    for (uint8_t i = 0; i < MEMORY_INITIAL_LEVEL; i++)
        model.sequence[i] = MEMORY_TOKEN_INVALID;
    assert(memory_model_tick(&model, model.deadline_ms) == MEMORY_EVENT_NEXT_ROUND);
    assert(model.sequence_len == MEMORY_INITIAL_LEVEL + 1U);
    for (uint8_t i = 0; i < MEMORY_INITIAL_LEVEL; i++)
        assert(model.sequence[i] != MEMORY_TOKEN_INVALID);
}

static void test_ok_press_waits_for_click(void) {
    memory_model_t model;
    memory_model_init(&model, 42, 0);
    assert(memory_model_start(&model) == MEMORY_EVENT_STARTED);
    assert(memory_model_playback_done(&model, 100) == MEMORY_EVENT_INPUT_READY);
    model.sequence[0] = MEMORY_TOKEN_OK;

    assert(memory_model_submit(&model, MEMORY_TOKEN_OK, MEMORY_INPUT_PRESS, 200) ==
           MEMORY_EVENT_NONE);
    assert(model.input_index == 0);
    assert(memory_model_back(&model) == MEMORY_EVENT_ABORTED);
    assert(model.clear_level == 0);
    assert(model.best_level == 0);
}

static void test_wrong_timeout_and_restart(void) {
    memory_model_t model;
    memory_model_init(&model, 77, 2);
    assert(memory_model_start(&model) == MEMORY_EVENT_STARTED);
    assert(memory_model_playback_done(&model, 1000) == MEMORY_EVENT_INPUT_READY);

    assert(memory_model_submit(&model, (memory_token_t)-1, MEMORY_INPUT_CLICK, 1050) ==
           MEMORY_EVENT_NONE);
    assert(memory_model_submit(&model, (memory_token_t)MEMORY_TOKEN_COUNT, MEMORY_INPUT_CLICK,
                               1060) == MEMORY_EVENT_NONE);
    assert(memory_model_submit(&model, memory_model_expected_token(&model),
                               (memory_input_event_t)99, 1070) == MEMORY_EVENT_NONE);
    assert(model.input_index == 0);
    assert(model.state == MEMORY_STATE_INPUT);

    const memory_token_t expected = memory_model_expected_token(&model);
    const memory_token_t wrong = expected == MEMORY_TOKEN_UP ? MEMORY_TOKEN_DOWN : MEMORY_TOKEN_UP;
    assert(memory_model_submit(&model, wrong, MEMORY_INPUT_CLICK, 1100) == MEMORY_EVENT_WRONG);
    assert(model.state == MEMORY_STATE_RESULT);
    assert(model.failure == MEMORY_FAILURE_WRONG);
    assert(model.failed_at == MEMORY_INITIAL_LEVEL);
    assert(model.clear_level == 0);
    assert(memory_model_tick(&model, 999999) == MEMORY_EVENT_NONE);

    assert(memory_model_start(&model) == MEMORY_EVENT_STARTED);
    assert(memory_model_playback_done(&model, 2000) == MEMORY_EVENT_INPUT_READY);
    assert(memory_model_tick(&model, 6999) == MEMORY_EVENT_NONE);
    assert(memory_model_submit(&model, MEMORY_TOKEN_UP, MEMORY_INPUT_DOUBLE, 7000) ==
           MEMORY_EVENT_NONE);
    assert(model.state == MEMORY_STATE_INPUT);
    assert(memory_model_tick(&model, 7000) == MEMORY_EVENT_TIMEOUT);
    assert(model.state == MEMORY_STATE_RESULT);
    assert(model.failure == MEMORY_FAILURE_TIMEOUT);
    assert(memory_model_start(&model) == MEMORY_EVENT_STARTED);
    assert(memory_model_back(&model) == MEMORY_EVENT_ABORTED);
    assert(model.state == MEMORY_STATE_READY);
    assert(model.best_level == 2);
}

static void test_maximum_is_12(void) {
    memory_model_t model;
    memory_model_init(&model, 999, 0);
    assert(memory_model_start(&model) == MEMORY_EVENT_STARTED);

    for (uint8_t level = MEMORY_INITIAL_LEVEL; level <= MEMORY_MAX_LEVEL; level++) {
        complete_click_round(&model, 10000U + (uint64_t)level * 1000U);
        assert(model.clear_level == level);
        assert(model.sequence_len == level);
        assert(model.best_level == level);

        if (level < MEMORY_MAX_LEVEL) {
            assert(memory_model_tick(&model, model.deadline_ms) == MEMORY_EVENT_NEXT_ROUND);
            assert(model.state == MEMORY_STATE_PLAYBACK);
        } else {
            assert(memory_model_tick(&model, model.deadline_ms) == MEMORY_EVENT_MAX_REACHED);
            assert(model.state == MEMORY_STATE_MAX_RESULT);
            assert(model.sequence_len == MEMORY_MAX_LEVEL);
            assert(memory_model_sequence_at(&model, MEMORY_MAX_LEVEL) == MEMORY_TOKEN_INVALID);
        }
    }
}

int main(void) {
    test_initial_state_and_generation();
    test_difficulty_configuration();
    test_press_click_dedupe_and_click_fallback();
    test_each_round_is_fresh_random_sequence();
    test_ok_press_waits_for_click();
    test_wrong_timeout_and_restart();
    test_maximum_is_12();
    puts("memory_model: all tests passed");
    return 0;
}
