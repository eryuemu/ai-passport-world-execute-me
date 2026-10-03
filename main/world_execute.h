// main/world_execute.h —— 《world.execute(me);》赛博 2-bit 调色板播放器接口
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEXE_MAGIC   0x57455845 // 'WEXE'
#define WEXE_VERSION 1

// 二进制文件头部结构 (48 字节定长)
typedef struct {
    uint32_t magic;               // 'WEXE' = 0x57455845
    uint16_t version;             // 1
    uint16_t width;               // 320
    uint16_t height;              // 180
    uint16_t fps;                 // 15
    uint16_t color_bits;          // 2 (4 色调色板)
    uint16_t palette[4];          // RGB565 x 4
    uint32_t total_frames;        // 3178
    uint32_t audio_sample_rate;   // 16000
    uint32_t audio_data_offset;   // 音频起始偏移
    uint32_t audio_data_size;     // 音频总字节数
    uint32_t video_index_offset;  // 视频索引表偏移
    uint32_t video_data_offset;   // 视频帧数据起始偏移
    uint16_t reserved;            // 对齐填充
} __attribute__((packed)) world_execute_header_t;

// 帧索引表项 (6 字节定长)
typedef struct {
    uint32_t offset;              // 相对 video_data_offset 的偏移
    uint16_t comp_size;           // Deflate 压缩字节数
} __attribute__((packed)) world_execute_index_entry_t;

/**
 * @brief 播放自然结束时的回调(在播放器任务上下文调用，只应投递事件、立即返回)
 */
typedef void (*world_execute_done_cb_t)(void);

/**
 * @brief 启动 《world.execute(me);》 播放器
 * @param on_done 自然播完时回调，可为 NULL
 * @return esp_err_t ESP_OK 成功启动；ESP_ERR_INVALID_STATE 正在运行中
 */
esp_err_t world_execute_player_start(world_execute_done_cb_t on_done);

/**
 * @brief 停止播放器。会阻塞直到音频、视频任务都已真正退出、
 *        不再访问屏幕/I2S，之后调用方可以安全地接管屏幕。
 */
esp_err_t world_execute_player_stop(void);

/**
 * @brief 查询播放器是否正在运行
 */
bool world_execute_player_is_running(void);

/**
 * @brief 切换播放/暂停状态
 */
void world_execute_player_toggle_pause(void);

/**
 * @brief 设置播放音量 (0~100)
 */
void world_execute_player_set_volume(uint8_t volume);

/**
 * @brief 获取当前音量
 */
uint8_t world_execute_player_get_volume(void);

#ifdef __cplusplus
}
#endif
