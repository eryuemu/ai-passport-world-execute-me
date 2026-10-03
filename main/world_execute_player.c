// main/world_execute_player.c —— 《world.execute(me);》赛博 2-bit 调色板播放器实现
//
// 两条硬约束(之前的花屏/卡死都出在这里):
// 1. esp_lcd 的 SPI 颜色传输是【异步】的:draw_bitmap() 只把 DMA 排进队列就返回，
//    DMA 还在读缓冲区。因此切片缓冲必须 ping-pong 双缓冲，绝不能填一块正在发送的缓冲。
//    (下一次 draw_bitmap 发 CASET 命令前，驱动会等待上一笔颜色传输完成。)
// 2. ST7789 走 SPI 要求 RGB565 高字节先发，ESP32-C3 是小端，所以内存里的像素要先字节交换。
#include "world_execute.h"
#include "bsp_display.h"
#include "bsp_audio.h"
#include "tinf.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "world_execute";

// 引用链接入 Flash 只读区的 WEXE 媒体数据
extern const uint8_t world_execute_data_bin_start[] asm("_binary_world_execute_data_bin_start");
extern const uint8_t world_execute_data_bin_end[]   asm("_binary_world_execute_data_bin_end");

// 播放状态管理
static volatile bool s_running = false;   // 任务循环条件
static volatile bool s_paused = false;
static bool s_active = false;             // start 成功后到 stop 完成前为 true
static uint8_t s_volume = 80;
static volatile uint32_t s_samples_played = 0;
static world_execute_done_cb_t s_done_cb = NULL;

// 任务退出握手:stop() 等这两个信号量，保证返回后没有任何任务再碰屏幕/I2S
static SemaphoreHandle_t s_audio_done = NULL;
static SemaphoreHandle_t s_video_done = NULL;

// 调色板缓存 (已字节交换，可直接送 SPI)
static uint16_t s_palette[4];

// IMA-ADPCM 步长表与索引表
static const int16_t STEP_TABLE[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static const int8_t INDEX_TABLE[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

static inline int16_t adpcm_decode_nibble(uint8_t nibble, int16_t *predicted, int *step_idx) {
    int step = STEP_TABLE[*step_idx];
    *step_idx += INDEX_TABLE[nibble & 0x0F];
    if (*step_idx < 0) *step_idx = 0;
    else if (*step_idx > 88) *step_idx = 88;

    int diff = step >> 3;
    if (nibble & 4) diff += step;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 1) diff += step >> 2;

    int pred = *predicted;
    if (nibble & 8) pred -= diff;
    else pred += diff;

    if (pred > 32767) pred = 32767;
    else if (pred < -32768) pred = -32768;

    *predicted = (int16_t)pred;
    return (int16_t)pred;
}

static inline uint16_t swap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

// 视频渲染配置：320x180 居中显示在 320x240 横屏上 (y 偏移 30..210)
#define VIDEO_W        320
#define VIDEO_H        180
#define VIDEO_Y_OFFSET 30
#define PACKED_FRAME_BYTES ((VIDEO_W * VIDEO_H) / 4) // 14,400 字节
#define SLICE_LINES    15
#define SLICE_COUNT    (VIDEO_H / SLICE_LINES)       // 12 批次

// 音频缓冲区配置 (256 字节 ADPCM = 512 采样点 = 1024 字节 PCM)
#define AUDIO_CHUNK_ADPCM_BYTES 256
#define AUDIO_CHUNK_SAMPLES     (AUDIO_CHUNK_ADPCM_BYTES * 2)

#define CLEAR_LINES 8

// 静态缓冲区 (零动态堆分配；.bss 在内部 SRAM，DMA 可直接访问)
static uint8_t  s_decomp_buf[PACKED_FRAME_BYTES];
static uint16_t s_slice_buf[2][VIDEO_W * SLICE_LINES] __attribute__((aligned(4)));
static uint16_t s_black_buf[VIDEO_W * CLEAR_LINES] __attribute__((aligned(4))); // 永远全 0，只读
static uint16_t s_hud_buf[VIDEO_W * 4] __attribute__((aligned(4)));
static int16_t  s_pcm_buf[AUDIO_CHUNK_SAMPLES];

static void clear_screen_black_landscape(esp_lcd_panel_handle_t panel) {
    for (int y = 0; y < 240; y += CLEAR_LINES) {
        esp_lcd_panel_draw_bitmap(panel, 0, y, 320, y + CLEAR_LINES, s_black_buf);
    }
}

// 绘制底部赛博进度条 HUD (y 222~225)。每秒一次，上次传输早已完成，可安全复用缓冲。
static void draw_hud_landscape(esp_lcd_panel_handle_t panel, uint32_t current_frame, uint32_t total_frames) {
    if (total_frames == 0) return;
    memset(s_hud_buf, 0, sizeof(s_hud_buf));

    int bar_width = (int)((uint64_t)current_frame * 300 / total_frames);
    if (bar_width > 300) bar_width = 300;

    for (int y = 0; y < 4; y++) {
        for (int x = 10; x < 10 + 300; x++) {
            s_hud_buf[y * 320 + x] = (x < 10 + bar_width) ? s_palette[2] : s_palette[1];
        }
    }
    esp_lcd_panel_draw_bitmap(panel, 0, 222, 320, 226, s_hud_buf);
}

// 音频播放任务 (主时钟基准)
static void audio_task(void *arg) {
    (void)arg;
    const world_execute_header_t *hdr = (const world_execute_header_t *)world_execute_data_bin_start;
    const uint8_t *adpcm_stream = world_execute_data_bin_start + hdr->audio_data_offset;
    const uint32_t total_adpcm_bytes = hdr->audio_data_size;

    ESP_LOGI(TAG, "音频任务启动: %lu Hz, %lu 字节",
             (unsigned long)hdr->audio_sample_rate, (unsigned long)total_adpcm_bytes);

    int16_t predicted = 0;
    int step_idx = 0;
    uint32_t adpcm_offset = 0;

    while (s_running && adpcm_offset < total_adpcm_bytes) {
        if (s_paused) {
            // I2S 配置了 auto_clear，停写后输出静音
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }

        uint32_t chunk_bytes = AUDIO_CHUNK_ADPCM_BYTES;
        if (adpcm_offset + chunk_bytes > total_adpcm_bytes) {
            chunk_bytes = total_adpcm_bytes - adpcm_offset;
        }

        const uint8_t *src = adpcm_stream + adpcm_offset;
        int sample_count = 0;
        for (uint32_t i = 0; i < chunk_bytes; i++) {
            uint8_t byte_val = src[i];
            s_pcm_buf[sample_count++] = adpcm_decode_nibble((byte_val >> 4) & 0x0F, &predicted, &step_idx);
            s_pcm_buf[sample_count++] = adpcm_decode_nibble(byte_val & 0x0F, &predicted, &step_idx);
        }

        if (bsp_audio_write(s_pcm_buf, (size_t)sample_count * sizeof(int16_t)) == ESP_OK) {
            s_samples_played += sample_count;
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        adpcm_offset += chunk_bytes;
    }

    const bool natural_end = s_running;   // 循环因播完而退出(而非被 stop)
    s_running = false;                    // 让视频任务也收尾
    ESP_LOGI(TAG, "音频任务退出%s", natural_end ? " (全曲播完)" : "");

    world_execute_done_cb_t cb = s_done_cb;
    xSemaphoreGive(s_audio_done);
    if (natural_end && cb) cb();
    vTaskDelete(NULL);
}

// 视频渲染任务 (从属时钟刷新)
static void video_task(void *arg) {
    (void)arg;
    esp_lcd_panel_handle_t panel = bsp_display_panel();
    const world_execute_header_t *hdr = (const world_execute_header_t *)world_execute_data_bin_start;

    // 横屏 (Landscape: 320 x 240)
    esp_lcd_panel_swap_xy(panel, true);
    esp_lcd_panel_mirror(panel, false, true);
    clear_screen_black_landscape(panel);

    const world_execute_index_entry_t *index_table =
        (const world_execute_index_entry_t *)(world_execute_data_bin_start + hdr->video_index_offset);
    const uint8_t *video_stream = world_execute_data_bin_start + hdr->video_data_offset;

    const uint32_t total_frames = hdr->total_frames;
    const uint32_t fps = hdr->fps;
    const uint32_t sample_rate = hdr->audio_sample_rate;
    uint32_t current_frame = 0;
    uint32_t last_offset = 0xFFFFFFFF;
    int buf_idx = 0;

    tinf_init();
    ESP_LOGI(TAG, "视频任务启动: %ux%u @ %u FPS, 共 %lu 帧",
             hdr->width, hdr->height, fps, (unsigned long)total_frames);

    while (s_running && current_frame < total_frames) {
        if (s_paused) {
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }

        // 以音频采样数作为绝对时间源
        uint32_t audio_frame = (uint32_t)(((uint64_t)s_samples_played * fps) / sample_rate);
        if (current_frame > audio_frame) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        const world_execute_index_entry_t *entry = &index_table[current_frame];
        if (entry->offset != last_offset && entry->comp_size > 0) {
            unsigned int dest_len = sizeof(s_decomp_buf);
            int ret = tinf_zlib_uncompress(s_decomp_buf, &dest_len,
                                           video_stream + entry->offset, entry->comp_size);
            if (ret != TINF_OK) {
                ESP_LOGW(TAG, "帧 %lu 解压失败 (%d)", (unsigned long)current_frame, ret);
            } else {
                for (int s = 0; s < SLICE_COUNT && s_running; s++) {
                    uint16_t *dst = s_slice_buf[buf_idx];
                    const uint8_t *src = &s_decomp_buf[s * SLICE_LINES * (VIDEO_W / 4)];
                    // 2-bit 调色板查表解压: 4 像素/字节
                    for (int i = 0; i < SLICE_LINES * (VIDEO_W / 4); i++) {
                        uint8_t b = src[i];
                        dst[0] = s_palette[(b >> 6) & 3];
                        dst[1] = s_palette[(b >> 4) & 3];
                        dst[2] = s_palette[(b >> 2) & 3];
                        dst[3] = s_palette[b & 3];
                        dst += 4;
                    }
                    int y1 = VIDEO_Y_OFFSET + s * SLICE_LINES;
                    esp_lcd_panel_draw_bitmap(panel, 0, y1, VIDEO_W, y1 + SLICE_LINES, s_slice_buf[buf_idx]);
                    buf_idx ^= 1;   // 下一片写另一块缓冲，这块留给 DMA
                }
            }
            last_offset = entry->offset;
        }

        if (current_frame % 15 == 0) {
            draw_hud_landscape(panel, current_frame, total_frames);
        }

        current_frame++;
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    ESP_LOGI(TAG, "视频任务退出 (帧 %lu/%lu)", (unsigned long)current_frame, (unsigned long)total_frames);
    xSemaphoreGive(s_video_done);
    vTaskDelete(NULL);
}

esp_err_t world_execute_player_start(world_execute_done_cb_t on_done) {
    if (s_active) return ESP_ERR_INVALID_STATE;

    const world_execute_header_t *hdr = (const world_execute_header_t *)world_execute_data_bin_start;
    if (hdr->magic != WEXE_MAGIC) {
        ESP_LOGE(TAG, "WEXE 魔数不匹配: 0x%08lX", (unsigned long)hdr->magic);
        return ESP_ERR_INVALID_ARG;
    }
    if (!bsp_display_panel()) return ESP_ERR_INVALID_STATE;

    if (!s_audio_done) s_audio_done = xSemaphoreCreateBinary();
    if (!s_video_done) s_video_done = xSemaphoreCreateBinary();
    if (!s_audio_done || !s_video_done) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_audio_done, 0);
    xSemaphoreTake(s_video_done, 0);

    esp_err_t err = bsp_audio_set_format(hdr->audio_sample_rate, 16, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_set_format 失败: %s", esp_err_to_name(err));
        return err;
    }
    bsp_audio_set_volume(s_volume);

    for (int i = 0; i < 4; i++) s_palette[i] = swap16(hdr->palette[i]);

    s_done_cb = on_done;
    s_paused = false;
    s_samples_played = 0;
    s_running = true;

    // 音频优先级高于视频，保证音频时钟不卡顿
    if (xTaskCreate(audio_task, "wexe_audio", 4096, NULL, 6, NULL) != pdPASS) {
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(video_task, "wexe_video", 6144, NULL, 4, NULL) != pdPASS) {
        s_running = false;
        xSemaphoreTake(s_audio_done, portMAX_DELAY);
        return ESP_ERR_NO_MEM;
    }
    s_active = true;
    return ESP_OK;
}

esp_err_t world_execute_player_stop(void) {
    if (!s_active) return ESP_OK;
    s_running = false;
    s_paused = false;

    // 最坏情况:音频卡在一次 I2S 写(~32ms)，视频卡在一帧解压+刷屏(~60ms)
    bool ok = xSemaphoreTake(s_audio_done, pdMS_TO_TICKS(2000)) == pdTRUE;
    ok &= xSemaphoreTake(s_video_done, pdMS_TO_TICKS(2000)) == pdTRUE;
    // 恢复竖屏配置
    esp_lcd_panel_handle_t panel = bsp_display_panel();
    if (panel) {
        esp_lcd_panel_swap_xy(panel, false);
        esp_lcd_panel_mirror(panel, false, false);
    }

    s_active = false;
    s_done_cb = NULL;
    ESP_LOGI(TAG, "播放器已停止");
    return ok ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool world_execute_player_is_running(void) {
    return s_active;
}

void world_execute_player_toggle_pause(void) {
    s_paused = !s_paused;
    ESP_LOGI(TAG, "%s", s_paused ? "暂停" : "继续");
}

void world_execute_player_set_volume(uint8_t volume) {
    if (volume > 100) volume = 100;
    s_volume = volume;
    bsp_audio_set_volume(s_volume);
    ESP_LOGI(TAG, "音量: %u%%", s_volume);
}

uint8_t world_execute_player_get_volume(void) {
    return s_volume;
}
