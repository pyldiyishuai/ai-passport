// tests/test_focus_logic.c —— 专注计时器状态机的主机测试（无需硬件/无需 ESP-IDF）。
//
// 跑法（需要主机 cc，Linux/macOS/Git-Bash+MinGW 均可）：
//   cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_focus_logic.c \
//      main/focus_logic.c -o /tmp/test_focus_logic && /tmp/test_focus_logic
#include "focus_logic.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(cond) do {                                                   \
    if (!(cond)) {                                                         \
        printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);             \
        g_failures++;                                                      \
    }                                                                      \
} while (0)

static void test_defaults(void) {
    focus_t f;
    focus_init(&f, 3, 0);
    CHECK(f.state == FOCUS_IDLE);
    CHECK(focus_duration_min(&f) == 25);
    CHECK(f.total_ms == 25u * 60u * 1000u);
    CHECK(f.remaining_ms == f.total_ms);
    CHECK(focus_progress_permille(&f) == 0);
}

static void test_start_pause_resume(void) {
    focus_t f;
    focus_init(&f, 0, 0);
    CHECK(focus_apply(&f, FOCUS_KEY_OK));
    CHECK(f.state == FOCUS_RUNNING);
    CHECK(!focus_advance(&f, 60u * 1000u));       // 5 分钟档，跑 1 分钟还没结束
    CHECK(f.remaining_ms == 4u * 60u * 1000u);
    CHECK(focus_apply(&f, FOCUS_KEY_OK));         // 暂停
    CHECK(f.state == FOCUS_PAUSED);
    CHECK(!focus_advance(&f, 10u * 1000u));       // 暂停期间不推进
    CHECK(f.remaining_ms == 4u * 60u * 1000u);
    CHECK(focus_apply(&f, FOCUS_KEY_OK));         // 继续
    CHECK(f.state == FOCUS_RUNNING);
}

static void test_preset_locked_while_running(void) {
    focus_t f;
    focus_init(&f, 1, 0);                         // 10 分钟
    CHECK(focus_apply(&f, FOCUS_KEY_OK));
    const uint8_t before = f.duration_index;
    CHECK(!focus_apply(&f, FOCUS_KEY_UP));        // 计时中改档位应被忽略
    CHECK(f.duration_index == before);
    CHECK(focus_apply(&f, FOCUS_KEY_OK_LONG));    // 长按重置
    CHECK(f.state == FOCUS_IDLE);
    CHECK(f.remaining_ms == f.total_ms);
    CHECK(focus_apply(&f, FOCUS_KEY_UP));         // 重置后可以改
    CHECK(f.duration_index == 2);
    CHECK(focus_duration_min(&f) == 15);
}

static void test_preset_wraps(void) {
    focus_t f;
    focus_init(&f, 0, 0);
    CHECK(focus_apply(&f, FOCUS_KEY_DOWN));       // 0 -> 末尾
    CHECK(f.duration_index == FOCUS_PRESET_COUNT - 1);
    CHECK(focus_apply(&f, FOCUS_KEY_UP));         // 末尾 -> 0
    CHECK(f.duration_index == 0);
}

static void test_finish_and_count(void) {
    focus_t f;
    focus_init(&f, 0, 7);
    CHECK(focus_apply(&f, FOCUS_KEY_OK));
    const uint32_t total = f.total_ms;
    CHECK(focus_advance(&f, total - 1));          // 差 1ms 没结束
    CHECK(f.state == FOCUS_RUNNING);
    CHECK(focus_advance(&f, 1));                  // 正好结束
    CHECK(f.state == FOCUS_DONE);
    CHECK(f.remaining_ms == 0);
    CHECK(f.done_count == 8);
    CHECK(focus_consume_finished(&f));            // 一次性
    CHECK(!focus_consume_finished(&f));
    CHECK(focus_progress_permille(&f) == 1000);
}

static void test_overtime_does_not_underflow(void) {
    focus_t f;
    focus_init(&f, 0, 0);
    CHECK(focus_apply(&f, FOCUS_KEY_OK));
    CHECK(focus_advance(&f, f.total_ms + 5u * 60u * 1000u));   // 一口气跑过头
    CHECK(f.remaining_ms == 0);
    CHECK(focus_progress_permille(&f) == 1000);
    CHECK(f.done_count == 1);
}

static void test_format_rounds_up(void) {
    focus_t f;
    focus_init(&f, 3, 0);                         // 25:00
    char buf[8];
    focus_format_remaining(&f, buf, sizeof(buf));
    CHECK(strcmp(buf, "25:00") == 0);
    CHECK(focus_apply(&f, FOCUS_KEY_OK));
    focus_advance(&f, 600);                       // 剩 24:59.4 → 仍显示 25:00
    focus_format_remaining(&f, buf, sizeof(buf));
    CHECK(strcmp(buf, "25:00") == 0);
    focus_advance(&f, 1000);
    focus_format_remaining(&f, buf, sizeof(buf));
    CHECK(strcmp(buf, "24:59") == 0);   // 剩 1498.4s → 向上取整 1499s
}

static void test_new_round_after_done(void) {
    focus_t f;
    focus_init(&f, 0, 0);
    focus_apply(&f, FOCUS_KEY_OK);
    focus_advance(&f, f.total_ms);
    CHECK(f.state == FOCUS_DONE);
    CHECK(focus_apply(&f, FOCUS_KEY_OK));         // 再来一轮
    CHECK(f.state == FOCUS_RUNNING);
    CHECK(f.remaining_ms == f.total_ms);
    CHECK(!f.just_finished);
}

static void test_state_labels(void) {
    CHECK(strcmp(focus_state_label(FOCUS_IDLE), "READY") == 0);
    CHECK(strcmp(focus_state_label(FOCUS_RUNNING), "FOCUS") == 0);
    CHECK(strcmp(focus_state_label(FOCUS_PAUSED), "PAUSED") == 0);
    CHECK(strcmp(focus_state_label(FOCUS_DONE), "DONE") == 0);
}

int main(void) {
    test_defaults();
    test_start_pause_resume();
    test_preset_locked_while_running();
    test_preset_wraps();
    test_finish_and_count();
    test_overtime_does_not_underflow();
    test_format_rounds_up();
    test_new_round_after_done();
    test_state_labels();

    if (g_failures == 0) {
        printf("focus_logic: ALL PASS\n");
        return 0;
    }
    printf("focus_logic: %d FAILURE(S)\n", g_failures);
    return 1;
}
