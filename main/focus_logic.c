// main/focus_logic.c —— 纯逻辑实现，无 LVGL / 无 ESP-IDF 依赖。
#include "focus_logic.h"

#include <stdio.h>
#include <string.h>

const uint16_t FOCUS_PRESETS_MIN[FOCUS_PRESET_COUNT] = {5, 10, 15, 25, 45, 60};

static uint32_t preset_ms(uint8_t index) {
    if (index >= FOCUS_PRESET_COUNT) index = 0;
    return (uint32_t)FOCUS_PRESETS_MIN[index] * 60u * 1000u;
}

static void load_preset(focus_t *f) {
    f->total_ms = preset_ms(f->duration_index);
    f->remaining_ms = f->total_ms;
}

void focus_init(focus_t *f, uint8_t duration_index, uint32_t done_count) {
    if (!f) return;
    memset(f, 0, sizeof(*f));
    f->state = FOCUS_IDLE;
    f->duration_index = (duration_index < FOCUS_PRESET_COUNT) ? duration_index : 3;  // 默认 25 分钟
    f->done_count = done_count;
    load_preset(f);
}

uint16_t focus_duration_min(const focus_t *f) {
    if (!f) return 0;
    return FOCUS_PRESETS_MIN[f->duration_index < FOCUS_PRESET_COUNT ? f->duration_index : 0];
}

// 档位切换只在“没有正在跑的计时”时允许：RUNNING/PAUSED 期间改时长会让
// 已经投入的时间失去意义，所以直接忽略。
static bool cycle_preset(focus_t *f, int step) {
    if (f->state == FOCUS_RUNNING || f->state == FOCUS_PAUSED) return false;
    int next = (int)f->duration_index + step;
    if (next < 0) next = FOCUS_PRESET_COUNT - 1;
    if (next >= FOCUS_PRESET_COUNT) next = 0;
    f->duration_index = (uint8_t)next;
    load_preset(f);
    return true;
}

bool focus_apply(focus_t *f, focus_action_t action) {
    if (!f) return false;

    switch (action) {
    case FOCUS_KEY_UP:
        return cycle_preset(f, +1);
    case FOCUS_KEY_DOWN:
        return cycle_preset(f, -1);

    case FOCUS_KEY_OK:
        switch (f->state) {
        case FOCUS_RUNNING:
            f->state = FOCUS_PAUSED;
            return true;
        case FOCUS_PAUSED:
            f->state = FOCUS_RUNNING;
            return true;
        case FOCUS_IDLE:
        case FOCUS_DONE:
            // 开始新的一轮：用当前档位重新装填
            load_preset(f);
            f->state = FOCUS_RUNNING;
            f->just_finished = false;
            return true;
        }
        return false;

    case FOCUS_KEY_OK_LONG:
        if (f->state == FOCUS_IDLE) return false;   // 已经是待机，无需重置
        load_preset(f);
        f->state = FOCUS_IDLE;
        f->just_finished = false;
        return true;
    }
    return false;
}

bool focus_advance(focus_t *f, uint32_t dt_ms) {
    if (!f || f->state != FOCUS_RUNNING || dt_ms == 0) return false;
    if (dt_ms >= f->remaining_ms) {
        f->remaining_ms = 0;
        f->state = FOCUS_DONE;
        f->done_count++;
        f->just_finished = true;
        return true;
    }
    f->remaining_ms -= dt_ms;
    return false;
}

bool focus_consume_finished(focus_t *f) {
    if (!f || !f->just_finished) return false;
    f->just_finished = false;
    return true;
}

uint16_t focus_progress_permille(const focus_t *f) {
    if (!f || f->total_ms == 0) return 0;
    if (f->remaining_ms >= f->total_ms) return 0;
    const uint32_t elapsed = f->total_ms - f->remaining_ms;
    // 先缩放再除法，避免 elapsed*1000 在 60 分钟档位上溢出 uint32
    const uint32_t permille = (uint32_t)(((uint64_t)elapsed * 1000u) / f->total_ms);
    return (uint16_t)(permille > 1000u ? 1000u : permille);
}

void focus_format_remaining(const focus_t *f, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    if (!f) { out[0] = '\0'; return; }
    // 向上取整：剩 24:59.4 时显示 25:00，而不是一开始就跳到 24:59
    uint32_t seconds = (f->remaining_ms + 999u) / 1000u;
    snprintf(out, out_len, "%02u:%02u", (unsigned)(seconds / 60u), (unsigned)(seconds % 60u));
}

const char *focus_state_label(focus_state_t state) {
    switch (state) {
    case FOCUS_RUNNING: return "FOCUS";
    case FOCUS_PAUSED:  return "PAUSED";
    case FOCUS_DONE:    return "DONE";
    case FOCUS_IDLE:
    default:            return "READY";
    }
}
