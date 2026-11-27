// main/app_main.c —— 应用入口：自己的专注计时器（Focus Timer）。
//
// 这个文件取代基线的硬件自测菜单，是“二次开发应用”的起点：UI 全部重新设计，
// 只复用 components/bsp 的板级 API。状态机在 focus_logic.c，可脱离硬件测试。
//
// 按键语义（自定义，不再沿用基线菜单）：
//   UP  / DOWN 短按   待机或完成状态下切换时长档位（5/10/15/25/45/60 分钟）
//   OK 短按           开始 / 暂停 / 继续 / 再来一轮
//   OK 长按           重置本轮
//
// 硬件约束提醒（详见 docs/hardware-design）：
//   * LVGL 非线程安全，本任务里操作 lv_* 必须先 bsp_lvgl_lock()
//   * 按键回调跑在共享 esp_timer 任务上，只允许入队，不能阻塞
//   * 音频 write/read 是阻塞调用，放专门的 worker 任务
//   * C3 无 PSRAM，UI 只用纯控件，不加载大图/大字体缓存
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"

#include "focus_logic.h"
#include "lvgl.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "focus";

// ---------------------------------------------------------------- 配色
#define COLOR_BG       0x0B1016
#define COLOR_CARD     0x151F2A
#define COLOR_ACCENT   0x35D6A4
#define COLOR_WARN     0xFFB23E
#define COLOR_TEXT     0xE7EDF3
#define COLOR_MUTED    0x6C7C8C

// ---------------------------------------------------------------- NVS
#define NVS_NAMESPACE  "focus"
#define NVS_KEY_INDEX  "dur_idx"
#define NVS_KEY_DONE   "done"

// ---------------------------------------------------------------- 全局
typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t ev;
} input_event_t;

static QueueHandle_t s_input_queue;
static TaskHandle_t s_beep_task;
static focus_t s_focus;
static bool s_battery_ok;

static lv_obj_t *s_time_label;
static lv_obj_t *s_state_pill;
static lv_obj_t *s_state_label;
static lv_obj_t *s_bar;
static lv_obj_t *s_rounds_label;
static lv_obj_t *s_batt_label;

// ---------------------------------------------------------------- NVS 读写
static void persist_load(uint8_t *index, uint32_t *done) {
    *index = 3;   // 25 分钟
    *done = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t idx = 0;
    if (nvs_get_u8(h, NVS_KEY_INDEX, &idx) == ESP_OK && idx < FOCUS_PRESET_COUNT) *index = idx;
    uint32_t d = 0;
    if (nvs_get_u32(h, NVS_KEY_DONE, &d) == ESP_OK) *done = d;
    nvs_close(h);
}

static void persist_save(const focus_t *f) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    (void)nvs_set_u8(h, NVS_KEY_INDEX, f->duration_index);
    (void)nvs_set_u32(h, NVS_KEY_DONE, f->done_count);
    (void)nvs_commit(h);
    nvs_close(h);
}

// ---------------------------------------------------------------- 提示音
#define CHIME_RATE 16000
#define CHIME_HZ   880
#define CHIME_MS   110
#define CHIME_GAP  70
#define CHIME_TOTAL 3
#define CHIME_CHUNK 256

// 阻塞式写 PCM：只在 beep 任务里跑。
static void write_tone(int freq, int ms) {
    int16_t buf[CHIME_CHUNK];
    const int period = CHIME_RATE / freq;
    const int total = CHIME_RATE * ms / 1000;
    int phase = 0;
    for (int done = 0; done < total;) {
        int n = (total - done) < CHIME_CHUNK ? (total - done) : CHIME_CHUNK;
        for (int i = 0; i < n; i++) {
            buf[i] = (phase < period / 2) ? 5000 : -5000;
            if (++phase >= period) phase = 0;
        }
        if (bsp_audio_write(buf, (size_t)n * sizeof(int16_t)) != ESP_OK) return;
        done += n;
    }
}

static void write_silence(int ms) {
    int16_t buf[CHIME_CHUNK] = {0};
    const int total = CHIME_RATE * ms / 1000;
    for (int done = 0; done < total;) {
        int n = (total - done) < CHIME_CHUNK ? (total - done) : CHIME_CHUNK;
        if (bsp_audio_write(buf, (size_t)n * sizeof(int16_t)) != ESP_OK) return;
        done += n;
    }
}

static void play_chime(void) {
    if (bsp_audio_set_format(CHIME_RATE, 16, 1) != ESP_OK) {
        ESP_LOGW(TAG, "音频格式设置失败，跳过提示音");
        return;
    }
    bsp_audio_set_volume(70);
    for (int i = 0; i < CHIME_TOTAL; i++) {
        write_tone(CHIME_HZ, CHIME_MS);
        if (i + 1 < CHIME_TOTAL) write_silence(CHIME_GAP);
    }
}

static void beep_task(void *arg) {
    (void)arg;
    for (;;) {
        if (ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == 0) continue;
        play_chime();
    }
}

static void request_chime(void) {
    if (s_beep_task) xTaskNotifyGive(s_beep_task);
}

// ---------------------------------------------------------------- UI
// 全部在持有 LVGL 锁的情况下调用。
static void ui_build(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    // 顶部：应用名 + 电量（右上角，默认位置，读不到时降级为 "--"）
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "FOCUS TIMER");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_MUTED), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 14);

    s_batt_label = lv_label_create(scr);
    lv_label_set_text(s_batt_label, "--");
    lv_obj_set_style_text_font(s_batt_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_batt_label, lv_color_hex(COLOR_MUTED), 0);
    lv_obj_align(s_batt_label, LV_ALIGN_TOP_RIGHT, -16, 14);

    // 中间：大号剩余时间
    s_time_label = lv_label_create(scr);
    lv_label_set_text(s_time_label, "25:00");
    lv_obj_set_style_text_font(s_time_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_time_label, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_align(s_time_label, LV_ALIGN_TOP_MID, 0, 74);

    // 状态胶囊
    s_state_pill = lv_obj_create(scr);
    lv_obj_remove_flag(s_state_pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_state_pill, 104, 28);
    lv_obj_set_style_radius(s_state_pill, 14, 0);
    lv_obj_set_style_border_width(s_state_pill, 1, 0);
    lv_obj_set_style_border_color(s_state_pill, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_bg_color(s_state_pill, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_pad_all(s_state_pill, 0, 0);
    lv_obj_align(s_state_pill, LV_ALIGN_TOP_MID, 0, 152);

    s_state_label = lv_label_create(s_state_pill);
    lv_label_set_text(s_state_label, "READY");
    lv_obj_set_style_text_font(s_state_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_state_label, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_center(s_state_label);

    // 进度条：0..1000 与 focus_progress_permille() 对齐
    s_bar = lv_bar_create(scr);
    lv_obj_set_size(s_bar, 200, 8);
    lv_obj_set_style_radius(s_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(COLOR_CARD), LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, 4, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
    lv_bar_set_range(s_bar, 0, 1000);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_obj_align(s_bar, LV_ALIGN_TOP_MID, 0, 202);

    // 累计完成轮数
    s_rounds_label = lv_label_create(scr);
    lv_label_set_text(s_rounds_label, "ROUNDS 0");
    lv_obj_set_style_text_font(s_rounds_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_rounds_label, lv_color_hex(COLOR_MUTED), 0);
    lv_obj_align(s_rounds_label, LV_ALIGN_TOP_MID, 0, 222);

    // 底部按键提示（两行，避免 240px 宽度挤在一起）
    lv_obj_t *hint1 = lv_label_create(scr);
    lv_label_set_text(hint1, "OK  START / PAUSE");
    lv_obj_set_style_text_font(hint1, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint1, lv_color_hex(COLOR_MUTED), 0);
    lv_obj_align(hint1, LV_ALIGN_TOP_MID, 0, 258);

    lv_obj_t *hint2 = lv_label_create(scr);
    lv_label_set_text(hint2, "UP/DN MIN   HOLD RESET");
    lv_obj_set_style_text_font(hint2, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint2, lv_color_hex(COLOR_MUTED), 0);
    lv_obj_align(hint2, LV_ALIGN_TOP_MID, 0, 280);

    lv_screen_load(scr);
}

static void ui_refresh(void) {
    if (!s_time_label) return;

    char buf[8];
    focus_format_remaining(&s_focus, buf, sizeof(buf));
    lv_label_set_text(s_time_label, buf);

    lv_label_set_text(s_state_label, focus_state_label(s_focus.state));
    const uint32_t pill_color = (s_focus.state == FOCUS_DONE) ? COLOR_WARN
                              : (s_focus.state == FOCUS_PAUSED) ? COLOR_MUTED
                              : COLOR_ACCENT;
    lv_obj_set_style_border_color(s_state_pill, lv_color_hex(pill_color), 0);
    lv_obj_set_style_text_color(s_state_label, lv_color_hex(pill_color), 0);
    lv_obj_set_style_text_color(s_time_label,
        lv_color_hex(s_focus.state == FOCUS_PAUSED ? COLOR_MUTED : COLOR_TEXT), 0);

    lv_bar_set_value(s_bar, focus_progress_permille(&s_focus), LV_ANIM_OFF);
    lv_label_set_text_fmt(s_rounds_label, "ROUNDS %u", (unsigned)s_focus.done_count);
}

static void ui_refresh_battery(void) {
    if (!s_batt_label) return;
    if (!s_battery_ok) {
        lv_label_set_text(s_batt_label, "--");
        return;
    }
    const int soc = bsp_battery_soc();
    if (soc < 0) {
        lv_label_set_text(s_batt_label, "--");
    } else {
        lv_label_set_text_fmt(s_batt_label, "%d%%", soc);
    }
}

// ---------------------------------------------------------------- 按键
// 运行在共享 esp_timer 任务上：只入队，绝不阻塞、不碰 LVGL。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!s_input_queue) return;
    input_event_t e = { .btn = btn, .ev = ev };
    (void)xQueueSend(s_input_queue, &e, 0);
}

static bool map_key(const input_event_t *e, focus_action_t *out) {
    if (e->ev == BSP_BTN_LONG && e->btn == BSP_BTN_OK) { *out = FOCUS_KEY_OK_LONG; return true; }
    if (e->ev != BSP_BTN_CLICK) return false;
    switch (e->btn) {
    case BSP_BTN_UP:   *out = FOCUS_KEY_UP;   return true;
    case BSP_BTN_DOWN: *out = FOCUS_KEY_DOWN; return true;
    case BSP_BTN_OK:   *out = FOCUS_KEY_OK;   return true;
    default: return false;
    }
}

// ---------------------------------------------------------------- 主任务
static void app_task(void *arg) {
    (void)arg;
    int64_t last_us = esp_timer_get_time();
    uint64_t carry_us = 0;
    uint32_t since_batt_ms = 0;

    for (;;) {
        input_event_t e;
        // 100ms 超时既做“等按键”，也做 UI 刷新节拍
        if (xQueueReceive(s_input_queue, &e, pdMS_TO_TICKS(100)) == pdTRUE) {
            focus_action_t action;
            if (map_key(&e, &action)) {
                const focus_t prev = s_focus;
                if (focus_apply(&s_focus, action)) {
                    if (bsp_lvgl_lock(200)) {
                        ui_refresh();
                        bsp_lvgl_unlock();
                    }
                    // 档位或轮数变化才写 NVS，别每次按键都擦写 Flash
                    if (prev.duration_index != s_focus.duration_index ||
                        prev.done_count != s_focus.done_count) {
                        persist_save(&s_focus);
                    }
                }
            }
        }

        const int64_t now_us = esp_timer_get_time();
        uint64_t delta_us = (uint64_t)(now_us - last_us);
        last_us = now_us;
        carry_us += delta_us;
        const uint32_t dt_ms = (uint32_t)(carry_us / 1000u);
        carry_us -= (uint64_t)dt_ms * 1000u;

        if (dt_ms > 0) {
            const bool ended = focus_advance(&s_focus, dt_ms);
            if (ended) {
                focus_consume_finished(&s_focus);
                persist_save(&s_focus);
                request_chime();
            }
            if (bsp_lvgl_lock(50)) {
                ui_refresh();
                bsp_lvgl_unlock();
            }
        }

        since_batt_ms += dt_ms;
        if (since_batt_ms >= 15000) {
            since_batt_ms = 0;
            if (bsp_lvgl_lock(50)) {
                ui_refresh_battery();
                bsp_lvgl_unlock();
            }
        }
    }
}

// ---------------------------------------------------------------- 启动
void app_main(void) {
    ESP_LOGI(TAG, "Focus Timer 启动");

    // NVS：首次烧录或分区被改写时可能需要擦一次
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) ESP_LOGW(TAG, "NVS 初始化失败(%s)，本次不持久化", esp_err_to_name(err));

    bsp_i2c_init();
    bsp_i2c_scan();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败，应用无法启动");
        return;
    }
    bsp_display_backlight(100);

    uint8_t dur_index = 3;
    uint32_t done_count = 0;
    persist_load(&dur_index, &done_count);
    focus_init(&s_focus, dur_index, done_count);

    if (bsp_lvgl_lock(1000)) {
        ui_build();
        ui_refresh();
        bsp_lvgl_unlock();
    }

    s_battery_ok = (bsp_battery_init() == ESP_OK);
    if (!s_battery_ok) ESP_LOGW(TAG, "电量计不可用，UI 降级显示 --");
    if (bsp_lvgl_lock(200)) {
        ui_refresh_battery();
        bsp_lvgl_unlock();
    }

    // 音频失败不影响计时，只是没有完成提示音
    if (bsp_audio_init() != ESP_OK) ESP_LOGW(TAG, "音频初始化失败，提示音不可用");
    if (xTaskCreate(beep_task, "chime", 4096, NULL, 4, &s_beep_task) != pdPASS) {
        s_beep_task = NULL;
        ESP_LOGW(TAG, "提示音任务创建失败");
    }

    s_input_queue = xQueueCreate(8, sizeof(input_event_t));
    if (!s_input_queue) {
        ESP_LOGE(TAG, "按键队列创建失败");
        return;
    }
    if (xTaskCreate(app_task, "focus_app", 6144, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "应用任务创建失败");
        return;
    }
    // 队列先建好再登记回调，避免回调落到 NULL 队列
    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败，应用只能看不能按");
    }

    ESP_LOGI(TAG, "就绪：档位 %u 分钟，累计 %u 轮",
             (unsigned)focus_duration_min(&s_focus), (unsigned)s_focus.done_count);
}
