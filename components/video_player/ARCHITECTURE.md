# 视频播放模块架构

整个视频播放模块位于 `components/video_player/`，采用**四层分层架构**，共有 7 个源文件。

## 层次结构（自上而下）

```
┌─────────────────────────────────────────────┐
│           video_player.c/h                   │  ← 顶层：播放器入口
│   (播放列表管理、SD卡扫描、VSync同步)          │
└──────────────────┬──────────────────────────┘
                   │
┌──────────────────▼──────────────────────────┐
│        app_stream_adapter.c/h                │  ← 第二层：流适配器
│   (JPEG硬解码、提取任务控制、帧缓冲轮转)       │
│   依赖: driver/jpeg_decode.h                  │
└──────────────────┬──────────────────────────┘
                   │
┌──────────────────▼──────────────────────────┐
│           app_extractor.c/h                  │  ← 第三层：媒体提取器
│   (文件I/O、多格式解封装、音频解码/播放)       │
│   依赖: esp_extractor, esp_mp4/avi/ts/...    │
│         esp_audio_simple_dec (AAC/MP3/FLAC)  │
└──────────────────┬──────────────────────────┘
                   │
┌──────────────────▼──────────────────────────┐
│             ppa_blend.c/h                    │  ← 横向辅助：PPA硬件混合
│   (RGB565帧间Alpha混合：淡入淡出过渡)         │
│   依赖: driver/ppa.h                          │
└─────────────────────────────────────────────┘
```

## 各模块职责

| 文件 | 职责 |
|---|---|
| `video_player.c` / `include/video_player.h` | 播放器主控：SD卡扫描 `.mp4/.avi` 文件 → 播放列表轮播 → 管理 LCD 帧缓冲和 VSync → 创建 `video_task` 循环播放 |
| `app_stream_adapter.c` / `app_stream_adapter.h` | 流适配层：持有 JPEG 硬件解码器 + extractor，运行 `extract_task` 逐帧读取 → JPEG 解码（RGB565/RGB888）→ 回调上层送显 |
| `app_extractor.c` / `app_extractor.h` | 媒体提取层：POSIX 文件 I/O 封装 → 注册 MP4/AVI 等解封装器 → 分离音视频流 → 视频帧回调上层，音频帧入队 → `audio_task` 解码播放 |
| `ppa_blend.c` / `ppa_blend.h` | PPA 硬件加速：利用 ESP32-P4 的 PPA 引擎做 RGB565 帧间 Alpha 混合（淡入淡出效果） |

## 数据流

```
SD卡文件 ──open/read──▶ app_extractor ──JPEG帧──▶ app_stream_adapter
                              │                        │
                              ▼                        ▼
                        音频帧入队              JPEG硬件解码
                              │                        │
                         audio_task              frame_cb回调
                      (解码→codec_dev)               │
                                                     ▼
                                              video_player
                                           (draw_bitmap + VSync)
                                                     │
                                                     ▼
                                               LCD面板
```

## 关键设计点

### 1. 双缓冲 + VSync

`video_player.c` 分配两个 480×800×2 的 PSRAM 帧缓冲，配合 LCD DPI 的 `on_refresh_done` 回调实现无撕裂渲染。

### 2. JPEG 硬件解码

`app_stream_adapter` 使用 ESP32-P4 的硬件 JPEG 解码器，只支持 MJPEG 视频格式（不支持 H.264）。

### 3. 音频异步处理

`app_extractor` 将音频帧推入 FreeRTOS 队列（深度6），由独立的 `audio_task`（优先级7）异步解码播放。

### 4. display_arbiter 集成

播放器通过仲裁器获取/释放显示所有权，与 Emote UI 共享屏幕。

### 5. 播放列表轮播

扫描 SD 卡根目录，支持最多 50 个视频文件循环播放，连续失败 3 次则退出。

### 6. PPA 硬件混合

`ppa_blend` 是独立的辅助模块，利用 ESP32-P4 PPA 引擎做帧级淡入淡出。目前代码中已定义但主播放流程暂未直接调用。

## 外部依赖

| 依赖 | 用途 |
|---|---|
| `esp_board_manager` | 获取 LCD panel 和 audio codec 句柄 |
| `display_arbiter` | 多任务显示仲裁 |
| `esp_extractor` 系列 | MP4/AVI/TS/FLV/WAV/HLS/OGG 等格式解封装 |
| `esp_audio_codec` / `esp_audio_simple_dec` | 音频解码（AAC/MP3/FLAC/OPUS/VORBIS/ADPCM） |
| `driver/jpeg_decode` | 硬件 JPEG 解码 |
| `driver/ppa` | PPA 硬件 Alpha 混合 |

## Kconfig 配置项

| 配置项 | 说明 |
|---|---|
| `VIDEO_PLAYER_ENABLE` | 启用 SD 卡视频播放器 |
| `VIDEO_PLAYER_AUDIO_ENABLE` | 启用视频音频轨道播放（依赖 ES8311 codec） |
| `VIDEO_PLAYER_SYNC_ENABLED` | 启用音视频同步（依赖 AUDIO_ENABLE） |

## FreeRTOS 任务一览

| 任务名 | 优先级 | 栈大小 | 所属模块 | 职责 |
|---|---|---|---|---|
| `video_task` | 6 | 8KB | video_player.c | 播放列表轮播主循环 |
| `extract_task` | 5 | 4KB | app_stream_adapter.c | 逐帧提取 + JPEG 解码 |
| `audio_task` | 7 | 4KB | app_extractor.c | 音频帧解码 + codec 输出 |
