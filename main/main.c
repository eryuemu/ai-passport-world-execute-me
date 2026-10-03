// main/main.c —— FoloToy AI Passport: 《world.execute(me);》 专用播放器固件
//
// 纯净直驱架构 (无 LVGL 调度开销与看门狗隐患):
// 1. 开机直接全屏推流展示 蓝色大肥鱼 赛博朋克二创立绘封面 (240x320)
// 2. 封面界面按 OK 键即刻开启横屏《world.execute(me);》PV 影音同步播放
// 3. 播放过程中短按 OK 暂停/继续，UP/DOWN 调节音量，长按或双击 OK 随时退出回封面
// 4. PV 自然播放结束后，自动平滑切回蓝色大肥鱼封面
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "cover.h"
#include "world_execute.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "main";

typedef enum {
    APP_STATE_COVER = 0,
    APP_STATE_PLAYING,
} app_state_t;

typedef enum {
    EV_TYPE_BUTTON = 0,
    EV_TYPE_PLAYER_DONE,
} event_type_t;

typedef struct {
    event_type_t type;
    bsp_btn_t    btn;
    bsp_btn_ev_t btn_ev;
} app_event_t;

static QueueHandle_t s_evt_queue = NULL;
static app_state_t   s_state = APP_STATE_COVER;

// 按键事件回调 (在 esp_timer 上下文运行，仅投递队列)
static void on_button(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!s_evt_queue) return;
    app_event_t event = {
        .type   = EV_TYPE_BUTTON,
        .btn    = btn,
        .btn_ev = ev,
    };
    (void)xQueueSend(s_evt_queue, &event, 0);
}

// 播放自然完成回调 (在 audio_task 上下文运行，仅投递队列)
static void on_player_done(void) {
    if (!s_evt_queue) return;
    app_event_t event = {
        .type = EV_TYPE_PLAYER_DONE,
    };
    (void)xQueueSend(s_evt_queue, &event, 0);
}

static void app_enter_cover(void) {
    ESP_LOGI(TAG, "切换到封面状态");
    world_execute_player_stop();
    cover_draw();
    s_state = APP_STATE_COVER;
}

static void app_enter_playing(void) {
    ESP_LOGI(TAG, "切换到播放状态");
    s_state = APP_STATE_PLAYING;
    esp_err_t err = world_execute_player_start(on_player_done);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "启动播放器失败: %s，恢复封面", esp_err_to_name(err));
        app_enter_cover();
    }
}

static void event_loop_task(void *arg) {
    (void)arg;
    app_event_t evt;
    for (;;) {
        if (xQueueReceive(s_evt_queue, &evt, portMAX_DELAY) == pdTRUE) {
            if (evt.type == EV_TYPE_PLAYER_DONE) {
                if (s_state == APP_STATE_PLAYING) {
                    ESP_LOGI(TAG, "PV 播放自然结束，返回封面");
                    app_enter_cover();
                }
                continue;
            }

            if (evt.type == EV_TYPE_BUTTON) {
                bsp_btn_t btn = evt.btn;
                bsp_btn_ev_t ev = evt.btn_ev;

                if (s_state == APP_STATE_COVER) {
                    // 封面下点击或按下 OK 键进入播放
                    if (btn == BSP_BTN_OK && (ev == BSP_BTN_CLICK || ev == BSP_BTN_PRESS)) {
                        app_enter_playing();
                    }
                } else if (s_state == APP_STATE_PLAYING) {
                    // 播放中按键逻辑
                    if (btn == BSP_BTN_OK && (ev == BSP_BTN_LONG || ev == BSP_BTN_DOUBLE)) {
                        ESP_LOGI(TAG, "用户长按/双击 OK 键退出播放");
                        app_enter_cover();
                    } else if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
                        world_execute_player_toggle_pause();
                    } else if (btn == BSP_BTN_UP && (ev == BSP_BTN_CLICK || ev == BSP_BTN_PRESS)) {
                        uint8_t vol = world_execute_player_get_volume();
                        if (vol <= 90) vol += 10; else vol = 100;
                        world_execute_player_set_volume(vol);
                    } else if (btn == BSP_BTN_DOWN && (ev == BSP_BTN_CLICK || ev == BSP_BTN_PRESS)) {
                        uint8_t vol = world_execute_player_get_volume();
                        if (vol >= 10) vol -= 10; else vol = 0;
                        world_execute_player_set_volume(vol);
                    }
                }
            }
        }
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "  FoloToy AI Passport - world.execute(me);");
    ESP_LOGI(TAG, "  DeepSeek 蓝色大肥鱼 赛博专属固件");
    ESP_LOGI(TAG, "================================================");

    bsp_i2c_init();
    bsp_i2c_scan();

    if (bsp_display_init() != ESP_OK) {
        ESP_LOGE(TAG, "显示屏初始化失败");
        return;
    }
    bsp_display_backlight(100);

    bsp_audio_init();
    bsp_battery_init();

    s_evt_queue = xQueueCreate(16, sizeof(app_event_t));
    if (!s_evt_queue) {
        ESP_LOGE(TAG, "创建事件队列失败");
        return;
    }

    esp_err_t btn_err = bsp_button_init(on_button, NULL);
    if (btn_err != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败: %s", esp_err_to_name(btn_err));
    }

    // 开机直接绘制蓝色大肥鱼赛博全屏封面
    cover_draw();
    s_state = APP_STATE_COVER;

    // 启动主交互事件循环任务
    xTaskCreate(event_loop_task, "main_evt", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "主系统已就绪，当前处于大肥鱼封面界面");
}
