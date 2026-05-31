# Edge Agent 项目入职指南

> 基于知识图谱自动生成 | 分析时间: 2026-05-31 | Commit: `ad85396`

---

## 项目概览

**Edge Agent** 是一个基于 **ESP-IDF v5.5** 的 **ESP32-P4** 智能设备固件项目，集成多种能力模块，通过 Wi-Fi 连接实现边缘端的自动化请求处理与事件响应。

| 属性 | 内容 |
|------|------|
| **主要语言** | C, TypeScript, Lua, YAML, Markdown |
| **框架** | ESP-IDF, FreeRTOS, LVGL, ESP-BSP |
| **硬件平台** | ESP32-P4（主），兼容 ESP32-S3/C5/C6 等多芯片 |
| **入口文件** | `main/main.c` → `app_main()` |
| **构建系统** | CMake + Kconfig + idf_component.yml |
| **文件总数** | 269（含 156 代码文件、87 配置文件、40 文档文件） |

### 核心能力模块

设备集成了 **10+ 种能力模块**：即时通讯（QQ/微信/Telegram/飞书）、文件管理、Lua 脚本引擎、MCP 客户端/服务器、技能管理、Web 搜索、语音识别（ASR/TTS）、视频播放、舵机控制、摄像头等。

### 启动流程

```
NVS 初始化 → FATFS 挂载 → Wi-Fi 连接 → HTTP 配置服务 → 
app_claw_start() → 事件路由器/记忆/技能/能力初始化 → 
claw_core 启动 → CLI 就绪
```

---

## 架构层次

项目分为 **7 个架构层次**，从上到下为：

### 1. 🖥️ Web 管理前端层（45 文件）

基于 **SolidJS + TypeScript + Vite** 的单页应用，提供设备管理的图形界面。

**关键文件：**
- `components/http_server/frontend_source/src/App.tsx` — 主应用组件，路由切换、设备重启、状态轮询
- `components/http_server/frontend_source/src/api/client.ts` — API 客户端，封装与设备的 HTTP REST 通信
- `components/http_server/frontend_source/src/state/config.ts` — 全局配置状态管理
- `components/http_server/frontend_source/src/state/dirty.ts` — 脏状态检测，未保存变更防护
- `components/http_server/frontend_source/src/i18n/index.ts` — 中英文国际化（fan-in 最高：44）
- `components/http_server/frontend_source/src/pages/*.tsx` — 12 个功能页面（设置/LLM/IM/搜索/记忆/聊天/能力/脚本/文件/设置向导）

### 2. 🔌 HTTP 服务器与 API 层（16 文件）

ESP-IDF C HTTP 服务器后端，提供 REST JSON API 及 WebSocket 网关。

**关键文件：**
- `components/http_server/http_server_core.c` — 生命周期管理（init/start/stop/captive portal）
- `components/http_server/http_server_priv.h` — 私有头文件，11 个模块共享（fan-in #5）
- `components/http_server/http_server_config_api.c` — 配置 CRUD API
- `components/http_server/http_server_files_api.c` — 文件管理 REST API
- `components/http_server/http_server_webim_api.c` — WebSocket IM 网关（连接池/广播/帧处理）
- `components/http_server/http_server_json.c` — JSON 工具函数（fan-in：8）
- `components/http_server/http_server_utils.c` — URL 解码、路径安全检查（fan-in：4）

### 3. 🎬 媒体处理与视频播放层（40 文件）

四层媒体管线：提取器 → 解码器 → 流适配器 → 播放器。

**关键文件：**
- `components/esp_extractor/` — 9 种容器格式的插件式提取器（MP4/AVI/FLV/HLS/OGG/WAV/TS）
- `components/video_player/app_extractor.c` — 格式注册 + 音频解码 FreeRTOS 任务
- `components/video_player/app_stream_adapter.c` — JPEG 解码封装与帧回调
- `components/video_player/video_player.c` — 顶层播放器（display_arbiter/帧缓冲/触摸 UI/播放列表）
- `components/video_player/ppa_blend.c` — PPA 硬件加速 Alpha 混合
- `components/video_player/emotion_video_controller.c` — 情绪视频控制

### 4. 📟 板级支持包（BSP）层（102 文件）

支持 **15+ 种开发板**的硬件抽象与驱动适配。

**板卡厂商：** Espressif, DFRobot, M5Stack, LilyGO, Movecall, Waveshare, Nologo.Tech

**每块板卡三文件配置模式：**
- `board_info.yaml` — 芯片型号、制造商元数据
- `board_peripherals.yaml` — I2C/SPI/I2S/GPIO/MIPI/DSI 外设引脚映射
- `board_devices.yaml` — LCD/触摸/音频/摄像头设备驱动配置

**主开发板：** `esp32_p4_wifi6_touch_lcd_4_3`（ST7701 DSI LCD + GT911 触摸 + ES8311/ES7210 音频）

### 5. ⚙️ 应用核心与技能层（20 文件）

固件主入口、配置管理和 Lua 脚本技能模块。

**关键文件：**
- `main/main.c` — 固件入口，13 个函数，完整初始化链
- `components/app_config/app_config.c` — NVS 配置持久化（含旧版 LLM 配置迁移）
- `components/cmd_wifi/cmd_wifi.c` — WiFi 控制台命令（status/scan/set/apply）
- `fatfs_image/scripts/builtin/*.lua` — 6 个 Lua 技能（灯光/FFT/摄像头/表盘/游戏/绘画）

### 6. 🔧 项目构建与配置层（22 文件）

- `CMakeLists.txt` — 顶层项目构建配置
- `main/Kconfig.projbuild` — 应用层可配置选项
- `sdkconfig` / `sdkconfig.defaults` — Kconfig 编译时配置
- `partitions_*.csv` — 8/16/32MB Flash 分区表（NVS + OTA 双槽位 + FATFS）
- `tools/cmake/` — ESP-IDF 源码补丁与分区表自动选择

### 7. 📚 项目文档层（24 文件）

中英文架构文档、开发指南、调试记录。

**关键文档：**
- `ARCHITECTURE_CN.md` — 全景架构文档（123 小节，10 大章节）
- `README.md` — 项目入门指南
- `硬件显示架构.md` — 显示系统全栈架构
- `语音模块架构文档.md` — 语音模块五大层次
- `舵机控制代码架构文档.md` — 舵机三层架构
- `Capability相关信息/` — Capability 概念与实现指南

---

## 关键概念

### Capability 能力系统

**Capability** 是连接 AI Agent 与硬件外设的核心抽象层，定义了设备"能做什么"。

- **16 类内置能力**：分为工具（可调用）、事件源（被动触发）和混合三种角色
- **Group 注册机制**：通过能力组组织注册，LLM 可见性白名单控制暴露范围
- **三条调用路径**：LLM 工具调用（AI 发起）→ Console 命令（用户发起）→ Event Router 规则（自动化触发）
- **渐进式披露**：Skills 文档通过 JSON Schema 描述参数，AI 只在需要时看到完整描述

### Lua 技能系统

通过 **Lua 5.4 引擎**实现可动态加载的应用模块，每个技能配套 **SKILL.md** 文档为 LLM 提供工具调用模式。

内建 6 个技能：灯光控制、音频 FFT、摄像头预览、时钟表盘、Flappy Bird 游戏、触摸绘画。

### Display Arbiter 显示仲裁

管理 LCD 屏幕的显示权竞争，在 video_player、Lua display、Emote、Emotion Video 等多个消费者之间协调帧缓冲访问。

### 板级工厂模式

每块开发板通过 `*_factory_entry_t` 函数创建 LCD 面板和触摸控制器句柄，`setup_device.c` 通过工厂函数提供统一的设备创建入口。

### HTTP 前后端分离

C HTTP 服务器提供 REST JSON API + WebSocket 网关，SolidJS 前端通过 `client.ts` API 客户端通信，共享 `http_server_priv.h` 作为内部契约。

---

## 引导式学习路径

推荐按以下 14 步顺序学习项目：

### 第 1 步：项目概览与快速入门
📄 `README.md`

了解项目全貌：设备启动流程、运行时目录结构、已集成能力列表以及构建烧录步骤。

### 第 2 步：全景架构蓝图
📄 `ARCHITECTURE_CN.md`

核心架构文档（123 小节，10 大章节）：总体架构分层、核心 Agent 引擎、能力层、通信层、Lua 系统、硬件显示、存储架构、构建系统。

### 第 3 步：固件入口与启动流程
📄 `main/main.c`

`app_main` → 13 个初始化函数，从头到尾走一遍启动链：NVS → FATFS → Wi-Fi → HTTP → `app_claw_start()` → claw_core。

### 第 4 步：应用配置管理模块
📄 `components/app_config/app_config.c` + `include/app_config.h`

了解 NVS 持久化配置的加载/保存/默认值机制，以及旧版 LLM 配置的自动迁移逻辑。

### 第 5 步：Wi-Fi 管理命令行接口
📄 `components/cmd_wifi/cmd_wifi.c`

ESP 控制台命令模式：status/scan/set/apply 四个子命令，理解嵌入式"服务终端"设计模式。

### 第 6 步：HTTP 服务器后端核心
📄 `components/http_server/http_server_core.c` + `http_server_priv.h`

HTTP 服务器生命周期 + 9 个子模块路由注册，`http_server_priv.h` 被 11 个模块引用。

### 第 7 步：HTTP API 服务与 JSON 工具层
📄 `components/http_server/http_server_json.c` + `http_server_utils.c`

两大公共服务层（fan-in 8 和 4），体现"提取公共依赖"的架构实践。

### 第 8 步：Web 管理前端应用入口
📄 `App.tsx` + `index.tsx` + `client.ts`

SolidJS 单页应用入口：路由切换、状态轮询、API 客户端。

### 第 9 步：前端状态管理与国际化
📄 `config.ts` + `configTab.ts` + `dirty.ts` + `toast.ts` + `i18n/index.ts`

四个状态管理模块 + 国际化系统（fan-in 44）。

### 第 10 步：媒体处理与视频播放引擎
📄 `video_player.c` + `app_extractor.c` + `app_stream_adapter.c` + `ppa_blend.c`

四层视频播放架构：提取器 → 解码器 → 流适配器 → 播放器。

### 第 11 步：Lua 脚本技能系统
📄 6 个 Lua 脚本 + SKILL.md 文档

理解 LLM Agent 工具调用的关键机制：Skills 文档驱动 → Lua 执行 → 硬件交互。

### 第 12 步：Capability 能力系统概念
📄 `什么是 Capability.md` + `如何实现 Capability.md`

连接 AI Agent 与硬件的核心抽象：描述符、Group、注册、生命周期钩子。

### 第 13 步：板级支持包与硬件抽象
📄 `board_info.yaml` + `board_peripherals.yaml` + `board_devices.yaml` + `setup_device.c`

YAML 三文件分离的板卡配置体系，工厂函数模式。

### 第 14 步：构建系统与 Flash 分区
📄 `CMakeLists.txt` + `Kconfig.projbuild` + `partitions_*.csv` + `tools/cmake/`

ESP-IDF 构建系统、分区表（NVS/OTA/FATFS）、CMake 工具补丁。

---

## 复杂度热点 ⚠️

以下是复杂度评估为 **complex** 的关键文件，新开发者应优先关注：

### 核心代码（建议深度阅读）

| 文件 | 说明 |
|------|------|
| `main/main.c` | 应用入口，13 个函数，完整初始化链 |
| `components/app_config/app_config.c` | 配置管理核心，含 LLM 配置迁移逻辑 |
| `components/http_server/http_server_webim_api.c` | WebSocket IM 网关，连接池与帧处理 |
| `components/http_server/http_server_files_api.c` | 文件管理 REST API（CRUD + 上传/下载） |
| `components/http_server/http_server_config_api.c` | 配置 CRUD API + CSV 字段过滤校验 |
| `components/video_player/video_player.c` | 视频播放器顶层，显示器仲裁 + 触摸 UI |
| `components/video_player/app_extractor.c` | 媒体格式解析 + 音频解码 FreeRTOS 任务 |
| `components/video_player/app_stream_adapter.c` | JPEG 解码 + 帧回调 + 播放控制 |

### 前端核心（SolidJS/TypeScript）

| 文件 | 说明 |
|------|------|
| `App.tsx` | 主应用组件，路由 + 重启 + 状态轮询 |
| `client.ts` | API 客户端，封装全部 HTTP REST 通信 |
| `config.ts` | 全局配置状态管理 |
| `configTab.ts` | 配置标签页状态工厂 |
| `FilesPage.tsx` | 文件管理页面（浏览/上传/下载/编辑） |
| `CapabilitiesPage.tsx` | 能力管理页面 |
| `FormField.tsx` | 表单字段组件集合 |

### 板级驱动

| 文件 | 说明 |
|------|------|
| `setup_device.c` (LilyGo T-Display) | ST7789 I80 总线 LCD 完整初始化流程 |
| `setup_device.c` (ESP32-S3 面包板) | USB 主机共享资源 + UAC 音频 + 摄像头 |
| `uac_codec.c` | USB 音频类编解码器驱动（dB/百分比转换） |

### 关键文档（推荐阅读）

| 文件 | 说明 |
|------|------|
| `ARCHITECTURE_CN.md` | 全景架构，123 小节 |
| `如何实现 Capability.md` | Capability 完整实现指南（6 步） |
| `语音模块架构文档.md` | 语音五大层次全栈架构 |
| `舵机控制代码架构文档.md` | 舵机三层架构 |
| `硬件显示架构.md` | 显示系统全栈解析 |
| `voice_channel_plan.md` | 语音通道升级方案（20 章节） |
| `情绪切换系统架构文档.md` | 情绪视频系统完整描述 |

---

## 快速参考：目录结构

```
application/edge_agent/
├── main/                        # 固件入口
│   ├── main.c                   # app_main() — 系统初始化
│   ├── CMakeLists.txt
│   ├── Kconfig.projbuild        # 应用 Kconfig 选项
│   └── idf_component.yml
├── components/
│   ├── http_server/             # HTTP 服务器 + Web 前端
│   │   ├── http_server_*.c      # C 后端 API 模块
│   │   └── frontend_source/     # SolidJS 前端
│   ├── video_player/            # 视频播放引擎
│   ├── app_config/              # 应用配置（NVS）
│   ├── app_claw/                # 核心框架
│   ├── esp_extractor/           # 媒体容器提取器
│   ├── cmd_wifi/                # WiFi 控制台命令
│   ├── common/                  # 公共模块
│   │   ├── display_arbiter/     # 显示仲裁
│   │   └── voice_ws_common/     # 语音 WebSocket 共享
│   └── claw_capabilities/       # 能力模块
├── boards/                      # 15+ 板卡 BSP
│   └── <vendor>/<board>/
│       ├── board_info.yaml
│       ├── board_peripherals.yaml
│       └── board_devices.yaml
├── fatfs_image/                 # FATFS 镜像
│   ├── skills/                  # 技能文档
│   ├── scripts/builtin/         # Lua 脚本
│   └── memory/                  # 长期记忆
├── tools/cmake/                 # CMake 构建工具
├── partitions_*.csv             # Flash 分区表
├── sdkconfig                    # SDK 配置
├── CMakeLists.txt               # 顶层构建
└── README.md
```

---

## 开发环境搭建

### 前置条件

- ESP-IDF v5.5.4 已安装并导出
- Python 3.x + `esp-bmgr-assist`

### 快速开始

```bash
# 1. 进入项目
cd application/edge_agent

# 2. 生成板级支持文件
idf.py gen-bmgr-config -c ./boards -b esp32_p4_wifi6_touch_lcd_4_3

# 3. 配置（WiFi/LLM/IM/Search 等）
idf.py menuconfig

# 4. 构建 & 烧录
idf.py build
idf.py flash monitor
```

---

## 相关资源

- 知识图谱仪表盘：运行 `/understand-dashboard` 启动交互式可视化
- 代码问答：运行 `/understand-chat` 基于图谱提问
- 差异分析：运行 `/understand-diff` 分析 git diff 影响范围

---

*本指南由 `/understand-onboard` 基于知识图谱自动生成。建议提交到团队仓库供新成员参考。*
