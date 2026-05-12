# 开发者 5 — 智能体逻辑与 Lua 脚本引擎

## 概述

开发者 5 负责 ESP-Claw Edge Agent 的**智能代理逻辑层与脚本运行时**，是整个设备的"大脑"所在。该层包括 Lua 脚本引擎的集成与 API 封装、Skills（技能）系统、Capabilities（能力）系统、Memory（长期记忆/知识库）管理、Scheduler（任务调度）、Router Rules（路由规则）以及语音模块的 Capability 定义。该开发者让设备从一个被动 Web 服务器转变为一个可编程、可扩展的 AI 代理。

---

## 完成的核心模块

### 1. Lua 脚本引擎集成 (managed_components/georgik__lua/)

- **嵌入式 Lua 5.4 解释器**：集成 `georgik__lua` 组件，在 ESP32 上运行标准 Lua 5.4 脚本
- **C API 绑定**：通过 `lua_State` 与设备硬件 API 深度绑定，支持以下驱动模块：

| Lua API 模块 | 功能说明 |
|-------------|---------|
| `adc` | 模数转换器读取 |
| `gpio` | GPIO 输入输出控制 |
| `i2c` | I2C 总线读写 |
| `mcpwm` | 12 通道 PWN 电机控制 |
| `touch` | 触摸传感器读取 |
| `uart` | 串口通信 |
| `audio` | 音频录制与播放 |
| `board_manager` | 板级信息查询 |
| `button` | 按键事件监听 |
| `call_capability` | 调用其他设备 Capability |
| `delay` | 延迟执行 |
| `display` | LCD 显示控制 |
| `esp_heap` | 堆内存状态查询 |
| `event_publisher` | 事件发布系统 |
| `led_strip` | LED 灯带控制 |
| `ssd1306` | SSD1306 OLED 驱动 |
| `storage` | FATFS 文件系统访问 |
| `system` | 系统信息与重启 |
| `arg_schema` | 参数模式校验库 |

#### 内置脚本文档 (fatfs_image/scripts/docs/)
为 11 个 Lua 模块和 6 个硬件驱动提供了完整的 `.md` 文档，说明 API 调用方式。

#### 内置测试脚本 (fatfs_image/scripts/builtin/test/)
包含 14 个完整测试脚本，覆盖所有硬件模块的功能验证：
- `adc_read.lua` — ADC 模数转换
- `audio_record_play.lua` — 音频录制与回放
- `button_events.lua` — 按键事件监听
- `capability_call.lua` — 跨设备能力调用
- `display_shapes.lua` — 绘制图形
- `i2c_scan_rw.lua` — I2C 总线扫描
- `led_strip_rainbow.lua` — LED 彩虹灯效
- `llm_analyze_trigger.lua` — LLM 分析触发
- `mcpwm_12ch.lua` — 12 通道 PWM 舵机控制
- `servo_sweep.lua` — 舵机转向
- `ssd1306_test.lua` — OLED 显示
- `system_info.lua` — 系统信息
- `touch_read.lua` — 触摸读取
- `uart_at.lua` — UART AT 通信

### 2. Skills（技能）系统 (fatfs_image/skills/ + main/skills/)

开发者 5 实现了 13 个内置 Skills:

| Skill | 路径 | 功能 |
|-------|------|------|
| `board_hardware_info` | fatfs/skills/ | 查询开发板硬件信息（芯片、Flash、PSRAM 等） |
| `builtin_lua_modules` | fatfs/skills/ | 通用 Lua 模块调用入口 |
| `cap_im_platform` | fatfs/skills/ | IM 平台消息收发能力 |
| `cap_llm_inspect_image` | fatfs/skills/ | LLM 图像识别分析 |
| `cap_lua` | fatfs/skills/ | 动态 Lua 脚本执行 |
| `cap_router_mgr` | fatfs/skills/ | 路由规则管理 |
| `cap_scheduler` | fatfs/skills/ | 定时任务管理 |
| `cap_time` | fatfs/skills/ | 时间日期查询 |
| `cap_web_search` | fatfs/skills/ | 网络搜索（Tavily） |
| `memory_ops` | fatfs/skills/ | 记忆增删改查 |
| `profile_memory_ops` | fatfs/skills/ | 个人化记忆操作 |
| `light_switch` | main/skills/ | GPIO/LED 灯控制（含 Lua 实现） |
| `weather_search` | main/skills/ | 天气查询 Skill |
| `lua_demo` | main/skills/ | 演示 Skill（含 Flappy Bird、时钟、画板等） |

#### Skill 规范
每个 Skill 目录包含 `SKILL.md` 声明文件，定义：
- Skill 名称和描述
- 触发条件（关键词匹配）
- 所需参数 Schema
- 回调脚本路径

### 3. Capabilities（能力）系统 (fatfs_image/skills/cap_*)

设备作为 AI 代理对外暴露的能力列表：

| 能力 | 说明 |
|------|------|
| `cap_im_platform` | 通过 IM 平台（微信/飞书/QQ/Telegram）收发消息 |
| `cap_llm_inspect_image` | 调用 LLM 分析图像内容 |
| `cap_lua` | 动态执行 Lua 脚本片段 |
| `cap_router_mgr` | 管理消息路由规则 |
| `cap_scheduler` | 管理定时任务 |
| `cap_time` | 获取当前时间和日期 |
| `cap_web_search` | 通过 Tavily API 搜索互联网 |

### 4. Memory（长期记忆/知识库）系统 (fatfs_image/memory/)

开发者 5 设计了设备的知识库系统：

| 文件 | 功能 |
|------|------|
| `identity.md` | 设备身份定义（我是谁） |
| `user.md` | 用户画像信息 |
| `soul.md` | AI 人格设定 |
| `MEMORY.md` | 记忆系统规范文档 |
| `memory_index.json` | 记忆条目的索引，支持快速检索 |
| `memory_records.jsonl` | 逐行 JSON 格式存储的所有记忆记录 |
| `memory_digest.log` | 记忆摘要日志 |
| `user.md` | 用户画像的个人信息 |

记忆系统支持：
- 短期记忆 → 长期记忆的归档
- 基于关键词的检索
- 定时摘要生成

### 5. Router Rules（路由规则） (fatfs_image/router_rules/router_rules.json)

消息路由引擎的规则配置，定义了：

```
用户消息 → 意图识别 → 匹配 Skill → 执行并返回
```

规则语法支持：
- **Intent 匹配**：基于关键词/语义的意图识别
- **Skill 绑定**：每个意图映射到一个特定 Skill
- **参数提取**：从自然语言中提取 Skill 调用参数
- **优先级排序**：多个匹配时的优先级决策

### 6. Scheduler（任务调度） (fatfs_image/scheduler/schedules.json)

定时任务调度系统：

```
{
  "schedules": [
    {
      "name": "每日摘要",
      "trigger": "cron(0 9 * * *)",
      "skill": "memory_ops",
      "params": { "action": "digest" }
    }
  ]
}
```

支持：
- **Cron 表达式**：精确到分钟的定时触发
- **一次性/重复任务**
- **Skill 绑定**：触发时调用指定 Skill

### 7. 语音模块 Capability 定义 (语音模块相关信息/)

开发者 5 还负责了语音交互模块的概念设计：

| 文件 | 内容 |
|------|------|
| `什么是 Capability.md` | Capability 系统的核心理念文档 |
| `如何实现 Capability.md` | 能力实现的技术指南 |
| `火山应用信息.md` | 火山引擎（豆包）语音平台的 API 配置 |

定义了语音交互的 Capability 规范：
- 语音输入 → ASR 识别 → 意图理解 → Skill 执行 → TTS 语音回复
- 支持火山引擎（豆包）等第三方语音平台集成

### 8. 示例应用脚本 (main/skills/lua_demo/)

开发者 5 编写了 5 个 Lua 示例应用，展示设备能力：

| 脚本 | 功能 |
|------|------|
| `flappybird.lua` | Flappy Bird 游戏（LCD 触摸交互） |
| `clock_dial_demo.lua` | 时钟表盘动画 |
| `lcd_touch_paint.lua` | LCD 触摸画板 |
| `camera_preview.lua` | 摄像头预览（需摄像头模组） |
| `audio_fft.lua` | 音频 FFT 频谱可视化 |

---

## 技术难点与解决方案

| 难点 | 解决方案 |
|------|---------|
| ESP32 上运行 Lua | 使用 `georgik__lua` 轻量级 Lua 5.4 移植，裁剪无关模块 |
| 硬件 API 绑定 | 每个驱动模块封装为 Lua C API，注册到 `lua_State` |
| 意图识别准确度 | 基于关键词匹配 + 参数 Schema 校验的多层路由 |
| 记忆持久化 | JSONL 格式存储到 FATFS，支持增量追加和索引检索 |
| 技能热加载 | Skill 以独立文件形式存在，启动时扫描注册 |
| 定时精度 | Cron 表达式解析 + FreeRTOS 定时器 |
| 跨平台 Capability 调用 | 统一 Capability 接口协议，支持本地和远程调用 |

## 代码规模估算

| 模块 | 文件数 | 预估代码量 |
|------|-------|-----------|
| Lua 引擎集成 | 1（managed） | ~50,000 行（Lua 解释器本身） |
| Lua C API 绑定 + 测试脚本 | ~25 个 | ~5,000 行 C + ~2,000 行 Lua |
| Skills (13 个) | ~13 个 SKILL.md + 脚本 | ~2,000 行 |
| Memory 系统 | ~7 个文件 | ~1,500 行 |
| Router Rules | 1 个 JSON | ~200 行 |
| Scheduler | 1 个 JSON | ~150 行 |
| Capability 文档 | 3 个 .md | ~500 行 |
| 示例应用 (5 个 Lua 脚本) | 5 个 | ~1,500 行 Lua |
| 内置库 (arg_schema, ssd1306) | 2 个 Lua + 2 个 .md | ~1,000 行 |
| **合计（不含 Lua 引擎本身）** | **~57 个** | **~13,850 行** |

---

## 与我相关的关键 API

```lua
-- Lua 驱动 API 示例
gpio.setup(pin, direction)            -- GPIO 配置
adc.read(channel)                     -- ADC 读取
i2c.scan(bus, sda_pin, scl_pin)      -- I2C 扫描
mcpwm.setup(ch, freq, duty)          -- PWM 设置
display.draw_text(x, y, text)        -- LCD 文字绘制
audio.record(duration)                -- 音频录制
audio.play(filepath)                  -- 音频播放
button.on_press(pin, callback)        -- 按键回调注册
event_publisher.publish(event, data)  -- 事件发布
call_capability(name, params)         -- 跨设备能力调用
storage.read(path)                    -- 文件读取
system.restart()                      -- 系统重启
esp_heap.get_free()                   -- 获取空闲堆内存

-- Skills 声明规范 (SKILL.md)
-- 每个 SKILL.md 定义技能的名称、触发条件、参数模式和回调
```

---

## 与我相关的关键文件

```
fatfs_image/
├── memory/
│   ├── identity.md          # 设备身份
│   ├── user.md              # 用户画像
│   ├── soul.md              # AI 人格
│   ├── MEMORY.md            # 记忆系统规范
│   ├── memory_index.json    # 记忆索引
│   ├── memory_records.jsonl # 记忆记录
│   └── memory_digest.log    # 摘要日志
├── router_rules/
│   └── router_rules.json    # 消息路由规则
├── scheduler/
│   └── schedules.json       # 定时任务配置
├── scripts/
│   ├── builtin/lib/         # 内置 Lua 库
│   ├── builtin/test/        # 硬件测试脚本 (14个)
│   └── docs/                # Lua API 文档 (17个 .md)
├── skills/                  # 技能定义 (11个)
└── static/                  # 静态资源

main/skills/                  # 运行时技能 (3个)
  ├── light_switch/
  ├── lua_demo/              # 示例脚本 (5个 Lua 应用)
  └── weather_search/

语音模块相关信息/           # 语音 Capability 定义
  ├── 什么是 Capability.md
  ├── 如何实现 Capability.md
  └── 火山应用信息.md
