// main/focus_logic.h —— 专注计时器的纯逻辑层。
//
// 刻意不 include LVGL / ESP-IDF：状态机、时间换算、进度计算全部可以脱离硬件
// 编译和测试（见 tests/test_focus_logic.c，用主机 cc 直接跑）。
// UI 层只负责把 focus_t 渲染出来，事件只往这里派发。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    FOCUS_IDLE = 0,   // 待机：可以调时长
    FOCUS_RUNNING,    // 计时中
    FOCUS_PAUSED,     // 已暂停：可以继续或重置
    FOCUS_DONE,       // 本轮完成：可以再来一轮
} focus_state_t;

// 应用层按键语义（和 BSP 的按键事件解耦，方便改成别的输入源）
typedef enum {
    FOCUS_KEY_UP = 0,   // UP 短按：切换时长档位
    FOCUS_KEY_DOWN,     // DOWN 短按：切换时长档位
    FOCUS_KEY_OK,       // OK 短按：开始 / 暂停 / 继续 / 再来一轮
    FOCUS_KEY_OK_LONG,  // OK 长按：重置本轮
} focus_action_t;

#define FOCUS_PRESET_COUNT 6
extern const uint16_t FOCUS_PRESETS_MIN[FOCUS_PRESET_COUNT];

typedef struct {
    focus_state_t state;
    uint8_t duration_index;   // 指向 FOCUS_PRESETS_MIN
    uint32_t total_ms;        // 本轮总时长
    uint32_t remaining_ms;    // 本轮剩余
    uint32_t done_count;      // 累计完成轮数（掉电由应用层存 NVS）
    bool just_finished;       // 本轮刚结束，待应用层取走后清零
} focus_t;

// 用持久化读回来的档位/计数初始化。
void focus_init(focus_t *f, uint8_t duration_index, uint32_t done_count);

// 派发一次按键动作。返回 true 表示状态或时长发生了变化（UI 需要刷新）。
bool focus_apply(focus_t *f, focus_action_t action);

// 推进时间。仅在 RUNNING 有效；dt_ms 用单调时钟算出来的差值，别用计数器自增，
// 这样即使 tick 抖动或任务被抢占，剩余时间也不会累积误差。
// 返回 true 表示本轮在这一次推进中结束（UI 该响铃/刷新）。
bool focus_advance(focus_t *f, uint32_t dt_ms);

// 取走“刚结束”标志（一次性）。
bool focus_consume_finished(focus_t *f);

// 0..1000 的进度（千分比），UI 直接喂给 lv_bar 的 0..1000 range。
uint16_t focus_progress_permille(const focus_t *f);

// "MM:SS"。out 至少 6 字节。
void focus_format_remaining(const focus_t *f, char *out, size_t out_len);

// 状态短标签，UI 直接显示（ASCII，避免默认字体缺中文字形）。
const char *focus_state_label(focus_state_t state);

// 当前时长档位的分钟数。
uint16_t focus_duration_min(const focus_t *f);
