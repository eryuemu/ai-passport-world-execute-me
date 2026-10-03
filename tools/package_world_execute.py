#!/usr/bin/env python3
# tools/package_world_execute.py —— 《world.execute(me);》赛博 2-bit 调色板音视频打包器
"""
功能：
1. 提取/下载《如果聊天窗口就是她的整个世界 —— world.execute(me);》音视频。
2. 音频转换为 16kHz, Mono, 16-bit PCM，并压缩为 4-bit IMA-ADPCM。
3. 视频按 15 FPS 缩放至 320x180（或 240x135），映射至 4 级冷光赛博调色板（无抖动锐利像素）。
4. 4 像素/字节打包 + Deflate (zlib level 9) 高压缩。
5. 封装为专有的 WEXE 二进制格式 (main/world_execute_data.bin)，可在 ESP32-C3 上零堆内存直读。
"""

import os
import sys
import zlib
import struct
import argparse
import subprocess
from PIL import Image

# 魔数 'WEXE' = 0x57455845
MAGIC_WEXE = 0x57455845
VERSION = 1

# 4 色赛博朋克调色板
# 0: 极夜黑 (0, 0, 0)       -> RGB565: 0x0000
# 1: 幽深蓝 (20, 40, 68)    -> RGB565: 0x1148
# 2: 电光荧青 (56, 189, 248) -> RGB565: 0x3DFE
# 3: 极光白 (255, 255, 255) -> RGB565: 0xFFFF
CYBER_PALETTE_RGB = [
    (0, 0, 0),
    (20, 40, 68),
    (56, 189, 248),
    (255, 255, 255)
]

def rgb_to_rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)

CYBER_PALETTE_RGB565 = [rgb_to_rgb565(r, g, b) for r, g, b in CYBER_PALETTE_RGB]

# IMA-ADPCM 步长表与索引表
STEP_TABLE = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
]

INDEX_TABLE = [
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
]

def encode_ima_adpcm(pcm_data: bytes) -> bytes:
    """将 16-bit 单声道 PCM 压缩为 4-bit IMA-ADPCM。两个 nibble 打包为一个字节。"""
    num_samples = len(pcm_data) // 2
    samples = struct.unpack(f"<{num_samples}h", pcm_data)
    
    encoded = bytearray()
    predicted = 0
    step_idx = 0
    nibble_buffer = None
    
    for sample in samples:
        step = STEP_TABLE[step_idx]
        diff = sample - predicted
        
        sign = 0
        if diff < 0:
            sign = 8
            diff = -diff
            
        nibble = 0
        if diff >= step:
            nibble |= 4
            diff -= step
        if diff >= (step >> 1):
            nibble |= 2
            diff -= (step >> 1)
        if diff >= (step >> 2):
            nibble |= 1
            
        nibble |= sign
        
        # 局部解码更新状态
        diff_pred = step >> 3
        if nibble & 4:
            diff_pred += step
        if nibble & 2:
            diff_pred += (step >> 1)
        if nibble & 1:
            diff_pred += (step >> 2)
            
        if sign:
            predicted -= diff_pred
        else:
            predicted += diff_pred
            
        if predicted > 32767:
            predicted = 32767
        elif predicted < -32768:
            predicted = -32768
            
        step_idx += INDEX_TABLE[nibble]
        if step_idx < 0:
            step_idx = 0
        elif step_idx > 88:
            step_idx = 88
            
        if nibble_buffer is None:
            nibble_buffer = nibble & 0x0F
        else:
            byte_val = (nibble_buffer << 4) | (nibble & 0x0F)
            encoded.append(byte_val)
            nibble_buffer = None
            
    if nibble_buffer is not None:
        encoded.append(nibble_buffer << 4)
        
    return bytes(encoded)

def transcode_audio(video_path: str, sample_rate: int = 16000) -> bytes:
    """提取音频并转为 16kHz 单声道 IMA-ADPCM"""
    print(f"[*] 提取并重采样音频至 {sample_rate}Hz 单声道...")
    cmd = [
        "ffmpeg", "-y", "-i", video_path,
        "-ac", "1", "-ar", str(sample_rate), "-f", "s16le", "-"
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    pcm_data, _ = proc.communicate()
    if proc.returncode != 0 or not pcm_data:
        raise RuntimeError("ffmpeg 提取音频失败")
        
    print(f"[*] 原始 PCM 大小: {len(pcm_data)} 字节 ({len(pcm_data)/2/sample_rate:.1f} 秒)")
    adpcm_data = encode_ima_adpcm(pcm_data)
    print(f"[*] IMA-ADPCM 压缩后大小: {len(adpcm_data)} 字节 ({len(adpcm_data)/1024:.1f} KB)")
    return adpcm_data

def transcode_video(video_path: str, width: int = 320, height: int = 180, fps: int = 15):
    """转码视频帧并打包压缩"""
    print(f"[*] 开始视频转码: 尺寸={width}x{height}, 帧率={fps} FPS...")
    
    flat_palette = []
    for r, g, b in CYBER_PALETTE_RGB:
        flat_palette.extend([r, g, b])
    flat_palette += [0, 0, 0] * (256 - len(CYBER_PALETTE_RGB))
    
    pal_img = Image.new("P", (1, 1))
    pal_img.putpalette(flat_palette)
    
    cmd = [
        "ffmpeg", "-i", video_path,
        "-vf", f"scale={width}:{height}:flags=lanczos,fps={fps}",
        "-f", "rawvideo", "-pix_fmt", "rgb24", "-"
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    
    frame_raw_bytes = width * height * 3
    packed_len = (width * height) // 4
    
    frames_data = []
    index_entries = []
    current_offset = 0
    frame_count = 0
    duplicate_count = 0
    last_packed = None
    
    while True:
        raw_rgb = proc.stdout.read(frame_raw_bytes)
        if not raw_rgb or len(raw_rgb) < frame_raw_bytes:
            break
            
        im = Image.frombytes("RGB", (width, height), raw_rgb)
        q = im.quantize(palette=pal_img, dither=Image.Dither.NONE)
        pixels = q.tobytes()
        
        # 4 像素装 1 字节: (p0 << 6) | (p1 << 4) | (p2 << 2) | p3
        packed = bytearray(packed_len)
        for i in range(0, len(pixels), 4):
            packed[i // 4] = ((pixels[i] & 3) << 6) | ((pixels[i+1] & 3) << 4) | ((pixels[i+2] & 3) << 2) | (pixels[i+3] & 3)
            
        if last_packed is not None and packed == last_packed:
            duplicate_count += 1
            prev_offset, prev_size = index_entries[-1]
            index_entries.append((prev_offset, prev_size))
        else:
            comp = zlib.compress(packed, level=9)
            index_entries.append((current_offset, len(comp)))
            frames_data.append(comp)
            current_offset += len(comp)
            last_packed = packed
            
        frame_count += 1
        if frame_count % 300 == 0:
            print(f"    已处理 {frame_count} 帧 ({frame_count/fps:.1f} 秒)...")
            
    proc.wait()
    all_video_bytes = b"".join(frames_data)
    print(f"[*] 视频处理完毕: 共 {frame_count} 帧, 重复复用帧: {duplicate_count} ({duplicate_count/frame_count*100:.1f}%)")
    print(f"[*] 压缩后视频总大小: {len(all_video_bytes)} 字节 ({len(all_video_bytes)/1024/1024:.2f} MB)")
    return frame_count, index_entries, all_video_bytes

def package_wexe(output_bin: str, video_path: str, width: int = 320, height: int = 180, fps: int = 15):
    """生成最终 WEXE 二进制文件"""
    adpcm_data = transcode_audio(video_path, sample_rate=16000)
    total_frames, index_entries, video_data = transcode_video(video_path, width, height, fps)
    
    # 头部结构：48 字节定长
    HEADER_SIZE = 48
    
    audio_offset = HEADER_SIZE
    audio_size = len(adpcm_data)
    
    index_offset = audio_offset + audio_size
    index_size = len(index_entries) * 6
    
    video_offset = index_offset + index_size
    
    header = struct.pack(
        "<IH4H4HIIIIIIH",
        MAGIC_WEXE,
        VERSION,
        width, height, fps, 2, # color_bits = 2
        CYBER_PALETTE_RGB565[0],
        CYBER_PALETTE_RGB565[1],
        CYBER_PALETTE_RGB565[2],
        CYBER_PALETTE_RGB565[3],
        total_frames,
        16000,
        audio_offset,
        audio_size,
        index_offset,
        video_offset,
        0 # reserved uint16_t
    )
    assert len(header) == HEADER_SIZE, f"Header size is {len(header)}, expected {HEADER_SIZE}"
    
    index_bytes = bytearray()
    for off, sz in index_entries:
        index_bytes.extend(struct.pack("<IH", off, sz))
        
    os.makedirs(os.path.dirname(os.path.abspath(output_bin)), exist_ok=True)
    with open(output_bin, "wb") as f:
        f.write(header)
        f.write(adpcm_data)
        f.write(index_bytes)
        f.write(video_data)
        
    total_size = os.path.getsize(output_bin)
    print("\n" + "=" * 60)
    print("【WEXE 二进制打包完成】")
    print(f"目标文件: {output_bin}")
    print(f"总文件大小: {total_size} 字节 ({total_size / 1024 / 1024:.2f} MB)")
    print(f"  - 头部元数据: {HEADER_SIZE} 字节")
    print(f"  - 音频数据:   {audio_size} 字节 ({audio_size / 1024 / 1024:.2f} MB)")
    print(f"  - 帧索引表:   {index_size} 字节 ({index_size / 1024:.1f} KB)")
    print(f"  - 压缩视频帧: {len(video_data)} 字节 ({len(video_data) / 1024 / 1024:.2f} MB)")
    print(f"出厂 APP 分区可用: 7.93 MB (0x7F0000)")
    margin = (0x7F0000 - total_size) / 1024 / 1024
    print(f"Flash 剩余安全余量: {margin:.2f} MB (充足)")
    print("=" * 60)

def main():
    parser = argparse.ArgumentParser(description="《world.execute(me);》WEXE 媒体打包器")
    parser.add_argument("--input", default="", help="输入 MP4 视频文件路径")
    parser.add_argument("--output", default="main/world_execute_data.bin", help="输出二进制路径")
    parser.add_argument("--width", type=int, default=320, help="视频宽度 (默认 320)")
    parser.add_argument("--height", type=int, default=180, help="视频高度 (默认 180)")
    parser.add_argument("--fps", type=int, default=15, help="目标帧率 (默认 15)")
    args = parser.parse_args()
    
    input_file = args.input
    if not input_file:
        candidates = [
            "/home/eryuemu/.gemini/antigravity-cli/brain/54695135-8e5a-49f5-9459-efd53c19a121/scratch/world_execute_me.mp4",
            "world_execute_me.mp4"
        ]
        for c in candidates:
            if os.path.exists(c):
                input_file = c
                break
                
    if not input_file or not os.path.exists(input_file):
        print(f"错误: 找不到输入视频文件，请指定 --input <path_to_mp4>")
        sys.exit(1)
        
    print(f"[*] 使用源视频: {input_file}")
    package_wexe(args.output, input_file, args.width, args.height, args.fps)

if __name__ == "__main__":
    main()
