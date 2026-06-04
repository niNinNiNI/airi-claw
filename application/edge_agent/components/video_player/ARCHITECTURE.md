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

### 4. display_arbiter 集成与显示切换

播放器通过 `display_arbiter` 与 Lua/其他模块共享显示屏。核心机制如下：

**所有权模型**
- `display_arbiter` 采用互斥所有权（`lua_depth` 计数器 + 优先权）
- Lua 可重入获取（`lua_depth++`），Video 仅当 `lua_depth == 0` 时可获取
- 默认 owner 为 `DISPLAY_ARBITER_OWNER_VIDEO`

**Video → Lua 切换（播放器失去 display）**
```
Lua 调用 acquire(LUA) → lua_depth++ → owner 改为 LUA
  → video_player 的 inner loop 条件 `is_owner(VIDEO)` 变为 false → 退出 inner loop
  → 进入等待循环（每200ms 尝试 acquire(VIDEO)）
  → extract_task 继续运行，但 frame_cb 检测到非 owner 直接 return（不绘制）
  → 不调用 stop_current_playback()，避免 SD 卡 I/O 阻塞时停止失效
```

**Lua → Video 切换（播放器拿回 display）**
```
Lua 调用 release(LUA) → lua_depth--，当 depth=0 时 owner 改为 NONE
  → 等待循环中 acquire(VIDEO) 成功 → 设置 display_was_taken = true
  → 回到主循环顶部 → 跳过 play_single_file()（播放还在跑）
  → 直接进入 inner loop 继续正常绘制
```

**为什么不能 stop extract task**

ESP-IDF 上，`vTaskDelete()` 一个阻塞在 SD 卡 `read()` 的 FreeRTOS 任务会导致 FATFS DMA 状态损坏。因此 display 被 Lua 抢走时**不停止 extract task**，而是让它继续解码，仅通过 frame callback 中的 `is_owner()` 检查跳过绘制。这避免了：
- `vTaskDelete()` 导致 SD 卡 DMA 损坏（所有后续 I/O 失败）
- 遗留 zombie task 访问已释放的 extractor（use-after-free）
- `vTaskDelete()` 可能留下未释放的 `frame_mutex`（死锁）

### 5. 播放列表轮播

扫描 SD 卡根目录，支持最多 50 个视频文件循环播放，连续失败 3 次则退出。

### 6. 卡顿检测与自动恢复

inner loop 中每 50ms 检查帧计数器：连续 5 次（250ms）帧数无变化 → 判定为卡顿 → 退出 inner loop → 停止播放 → 重新开始（loop 模式）或等待命令。连续 3 次卡顿 → 放弃当前播放，等待新命令。

### 7. PPA 硬件混合

`ppa_blend` 是独立的辅助模块，利用 ESP32-P4 PPA 引擎做帧级淡入淡出。目前代码中已定义但主播放流程暂未直接调用。

## video_task 状态机

```
                    ┌──────────┐
           START    │  WAIT FOR │   SWITCH_FILE
         ─────────▶ │  COMMAND  │ ◀──────────────
                    └─────┬─────┘
                          │ SWITCH_FILE
                          ▼
                    ┌──────────────┐
                    │  ACQUIRE     │── retry/500ms
                    │  DISPLAY     │
                    └──────┬───────┘
                           │ OK
                           ▼
                    ┌──────────────┐
                    │ play_single  │  (display_was_taken 时跳过)
                    │ _file()      │
                    └──────┬───────┘
                           │
                           ▼
              ┌────────────────────────┐
              │   INNER LOOP           │
              │   while is_owner(V):   │
              │   - 处理 STOP/SWITCH   │
              │   - 检测 EOS           │
              │   - 检测 stall         │
              └───┬──────┬──────┬──────┘
                  │      │      │
               EOS/   显示被   STALL
               STALL  抢走       │
                  │      │      │
                  ▼      ▼      ▼
              stop()  WAIT  stop()
                     LOOP       │
                  (200ms)       ▼
                     │    ┌──────────┐
                     │    │  loop?   │
                     │    └─┬─────┬──┘
                     │    Y │     │ N
                     │      │     └──▶ WAIT COMMAND
                     │   re-acquire?  │
                     │   ┌──┴──┐      │
                     │ Y │     │ N    │
                     │   │     │      │
                     ▼   ▼     ▼      ▼
                  CONTINUE  STOP/   WAIT
                            SWITCH  COMMAND
```

## 外部依赖

| 依赖 | 用途 |
|---|---|
| `esp_board_manager` | 获取 LCD panel 和 audio codec 句柄 |
| `display_arbiter` | 多任务显示仲裁（Lua/Video 互斥） |
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

## 关键事件流程

### display 被 Lua 抢走（完整流程）

```
1. Lua: display_arbiter_acquire(LUA)
   → lua_depth++, owner = LUA
2. video_player inner loop: is_owner(VIDEO) → false
   → 退出 inner loop（不调 stop_current_playback）
3. 进入等待循环:
   while s_running && !is_owner(VIDEO):
     - 处理 STOP/SWITCH_FILE 命令（正常处理）
     - acquire(VIDEO) — 每 200ms 重试
4. 同时 extract_task 继续运行:
   - read_frame → decode_jpeg → frame_cb → is_owner? NO → return
   - SD 卡 I/O 不受影响，DMA 保持正常
5. Lua: display_arbiter_release(LUA)
   → lua_depth--, owner = NONE
6. 等待循环: acquire(VIDEO) → OK → display_was_taken = true
7. 回到主循环顶部 → 跳过 play_single_file → 继续 inner loop
```

### 播放卡顿检测与恢复

```
inner loop:
  every 50ms:
    stats.frames_processed == last_frames?
      YES → stable_count++
        stable_count >= 5? → playback_stalled = true → break
      NO  → stable_count = 0

  playback_stalled:
    stop_current_playback()
    consecutive_stalls++
    consecutive_stalls >= 3? → give up, wait for command
    otherwise → loop back, re-acquire display, play again
```

### 修复历史（关键 bug 与解决）

| 时间 | 问题 | 根因 | 修复 |
|---|---|---|---|
| 初始 | `Extract task did not stop within timeout` 无限循环 | video_player stall → stop → loop_enabled → 重试 → 无限循环 | 添加 `MAX_CONSECUTIVE_FAILURES=3` 限制 |
| 尝试1 | 修复后 Lua display 卡死 | `vTaskDelete()` 阻塞的 extract task 导致死锁/内存损坏 | 回退 |
| 尝试2 | 修复后 video 无法播放 | zombie task 访问已释放的 extractor → use-after-free | 回退 |
| 最终 | display_was_taken 方案 | extract task 不停止，frame_cb skip 绘制，等待循环 poll acquire | 当前方案 |
