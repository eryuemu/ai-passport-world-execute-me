// main/cover.c —— 开机封面:蓝色大肥鱼二创立绘
//
// deepseek_cover.bin = 240x320 RGB565，已按 ST7789 SPI 要求存成高字节在前，
// 圆角与提示文字都已烘焙进图里，这里只负责分片搬运到屏幕。
#include "cover.h"
#include "bsp_display.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "cover";

extern const uint8_t deepseek_cover_bin_start[] asm("_binary_deepseek_cover_bin_start");
extern const uint8_t deepseek_cover_bin_end[]   asm("_binary_deepseek_cover_bin_end");

#define COVER_W     240
#define COVER_H     320
#define SLICE_LINES 16

// Flash 里的数据不能直接给 DMA，先拷到内部 RAM。
// SPI 颜色传输是异步的，所以用两块缓冲交替:填 A 时 DMA 在发 B。
static uint16_t s_buf[2][COVER_W * SLICE_LINES] __attribute__((aligned(4)));

void cover_draw(void) {
    esp_lcd_panel_handle_t panel = bsp_display_panel();
    if (!panel) return;
    if ((size_t)(deepseek_cover_bin_end - deepseek_cover_bin_start) < COVER_W * COVER_H * 2) {
        ESP_LOGE(TAG, "封面数据尺寸不对");
        return;
    }

    // 竖屏 (与 bsp_display_init 的默认方向一致)
    esp_lcd_panel_swap_xy(panel, false);
    esp_lcd_panel_mirror(panel, false, false);

    const uint8_t *src = deepseek_cover_bin_start;
    const size_t slice_bytes = COVER_W * SLICE_LINES * sizeof(uint16_t);
    for (int y = 0, i = 0; y < COVER_H; y += SLICE_LINES, i ^= 1) {
        memcpy(s_buf[i], src + (size_t)y * COVER_W * 2, slice_bytes);
        esp_lcd_panel_draw_bitmap(panel, 0, y, COVER_W, y + SLICE_LINES, s_buf[i]);
    }
    ESP_LOGI(TAG, "封面已绘制");
}
