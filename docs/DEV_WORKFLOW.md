# ESP-Claw 固件开发步骤指南

> 本文档按**从零构建**的顺序，逐步描述 ESP-Claw 固件每一层需要编写的代码及其职责。不包含具体代码实现，但会精确描述每个文件做什么、为什么需要它、以及它与其他模块的关系。

---

## 目录

1. [项目初始化与构建骨架](#1-项目初始化与构建骨架)
2. [板卡支持层](#2-板卡支持层)
3. [公共基础设施层](#3-公共基础设施层)
4. [Lua 模块层](#4-lua-模块层)
5. [核心引擎层](#5-核心引擎层)
6. [能力插件层](#6-能力插件层)
7. [应用组装层](#7-应用组装层)
8. [HTTP 服务与 Web 管理界面](#8-http-服务与-web-管理界面)
9. [FATFS 预置内容](#9-fatfs-预置内容)
10. [构建与烧录流程](#10-构建与烧录流程)
11. [调试与诊断](#11-调试与诊断)

---

## 1. 项目初始化与构建骨架

### 1.1 创建 ESP-IDF 项目

首先通过 ESP-IDF 工具创建项目骨架，这会产生 `main/CMakeLists.txt`、`main/main.c`、项目级 `CMakeLists.txt` 和 `sdkconfig.defaults` 等基础文件。项目定位在 `application/edge_agent/` 目录下。

### 1.2 配置依赖管理 (idf_component.yml)

创建 `main/idf_component.yml`，声明项目的外部组件依赖：

- **esp_board_manager** (`^0.5.10`): ESP 官方板卡管理框架，通过 YAML 文件定义板卡硬件配置
- **esp_wifi_remote** (`>=0.10,<2.0`): 仅在 ESP32-P4 目标芯片时引入，用于通过协处理器管理 Wi-Fi
- **esp_hosted** (`2.12.3`): 仅在 ESP32-P4 目标芯片时引入，提供 Wi-Fi 协议栈
- **esp_websocket_client** (`^1.4.0`): WebSocket 客户端库，用于语音服务器通信

同时将项目中所有本地组件（`app_claw`、`app_config`、`http_server`、`wifi_manager` 等）以相对路径方式声明为依赖。

### 1.3 配置 CMake 构建脚本

项目级 `CMakeLists.txt` 需要做三件关键事情：

1. **引入 ESP-IDF 补丁脚本** (`tools/cmake/esp_idf_patch.cmake`): 在配置阶段对 ESP-IDF v5.5.4 上游源码打两个补丁 —— 修复 LCD 并口 IO 采样边沿的寄存器值、以及修复 USB IAD 描述符大小定义。这两个补丁必须精确匹配 v5.5.4 版本的源码模式，不同版本需要重新验证。

2. **引入分区表自动选择脚本** (`tools/cmake/flash_partition_defaults.cmake`): 从 `esp_board_manager` 生成的默认配置文件中检测 Flash 大小（`CONFIG_ESPTOOLPY_FLASHSIZE_8MB/16MB/32MB`），据此自动选择对应的分区表 CSV 文件。同时清理 sdkconfig 中任何旧的硬编码分区表路径，写入自动选择的分区表文件名。

3. **FATFS 镜像生成**: 调用 `fatfs_create_spiflash_image` 将 `fatfs_image/` 目录打包为 Flash 中的 FAT 文件系统镜像。此镜像包含预置的 memory、skills、scripts、router_rules、scheduler 配置和静态 Web 资源。

4. **Skill 与 Lua 文档同步**: 使用 `skill_builder_configure_skill_sync` 和 `lua_module_builder_configure_builtin_lua_sync` 两个构建钩子，分别在编译前将项目中的 SKILL.md 文件和 Lua 模块文档自动同步到 `fatfs_image/` 对应目录。

### 1.4 配置 Kconfig 菜单系统

创建 `main/Kconfig.projbuild`，定义应用层的配置菜单：

- **Wi-Fi 默认值**: SSID、密码、AP SSID 前缀（默认 `esp-claw`，后缀为 MAC 地址末6位）、AP 信道、最大连接数、STA 重试次数
- **搜索 API Key**: Tavily 搜索 API Key 默认值
- **视频播放器配置**: 是否启用 SD 卡视频播放、音频轨道、音视频同步

### 1.5 配置 SDK 默认值 (sdkconfig.defaults)

通过 `idf.py save-defconfig` 生成，手动固化以下关键配置：

- **PSRAM 配置**: 启用 Octal SPI PSRAM (200MHz)、启动时初始化、用做 malloc 池、允许 BSS 段放入外部内存、为 Wi-Fi/LWIP 预留内部内存
- **分区表**: 自定义分区表、32MB Flash 对应文件
- **Wi-Fi 协议栈**: 静态/动态 RX buffer 数量和 RX BA Window 大小（均为内存优化配置）
- **FreeRTOS**: IDLE 任务栈 2048、主任务栈 8192、ISR 栈 4096、启用运行时统计追踪
- **FATFS**: 启用堆上的长文件名支持
- **LWIP**: 主机名 `esp-claw`、最大 socket 数 20、TCP 乱序队列限制
- **mbedTLS**: 外部内存分配、硬件 AES 关闭
- **LVGL (显示库)**: 使用系统 malloc（使大分配走 PSRAM）、绘制层 buffer 245KB、图片缓存 64KB
- **板卡具体配置**: 由 `esp_board_manager` 管理，通过 `CONFIG_ESP_BOARD_*` 系列宏指定
- HTTPD WebSocket 支持、控制台 UART 配置等

---

## 2. 板卡支持层

### 2.1 板卡 YAML 描述文件

每块开发板需要三个 YAML 文件，存放在 `application/edge_agent/boards/<vendor>/<board_name>/`:

**board_info.yaml** —— 声明芯片型号（如 `esp32s3`）、板卡名称、硬件版本、制造商、简要描述。这些信息会被 `esp_board_manager` 解析并在构建日志中打印。

**board_devices.yaml** —— 声明板载设备及其引脚映射。顶层 `devices` 节点下列出 display（LCD 屏幕）、audio_codec（音频编解码器）、camera（摄像头）、touch（触摸屏）、imu（惯性测量单元）等设备。每个设备的 `io` 节点记录 SPI/I2C/I2S 接口编号和 GPIO 引脚，`peripherals` 节点引用对应的外设配置。还需要声明 `peripherals` 节点，详细描述每个外设的类型和引脚。

**board_peripherals.yaml** —— 描述外设子系统的详细引脚分配，包括显示接口（SPI/DSI/RGB）、触摸 I2C 地址和中断引脚、音频 I2S 数据线、SD 卡 SDMMC 接口等。

### 2.2 板卡初始化代码

每块板卡需要实现 `setup_device.c`，包含板级初始化函数。其主要职责包括：

- 电源管理芯片初始化（如 M5Stack CoreS3 的 AXP2101 电源管理）
- 外设上电顺序控制（先开显示背光电源、再开音频功放等）
- IO 扩展器初始化（如通过 AW9523B I2C GPIO 扩展芯片控制更多引脚）
- 板卡特定外设的底层配置

部分板卡还需要板级私有驱动组件，例如 M5Stack CoreS3 需要独立的 AW9523B IO 扩展器驱动，Nologo.Tech XingZhi395 需要 FL7707N LCD 控制器驱动。这些放在 `boards/<vendor>/<board>/components/` 下。

### 2.3 板卡代码自动生成

构建时 `esp_board_manager` 会根据 YAML 文件自动生成以下文件到 `components/gen_bmgr_codes/`:

- `gen_board_info.c`: 板卡元信息注册表
- `gen_board_device_config.c`: 设备配置数据
- `gen_board_device_handles.c`: 设备句柄初始化函数
- `gen_board_periph_config.c`: 外设配置数据
- `gen_board_periph_handles.c`: 外设句柄初始化函数

这些文件是自动生成的，开发时不需要手动编写。

---

## 3. 公共基础设施层

这一层是 `components/common/` 下的组件，提供被所有上层模块共享的基础服务。按实现优先级排序：

### 3.1 Wi-Fi 管理器 (wifi_manager)

**职责**: 管理 Wi-Fi 状态机，包括 STA 模式连接、AP 模式热点、以及 AP-STA 共存模式。

**源文件**:
- `wifi_manager.c` —— 核心实现，包含 6 种状态的状态机
- `include/wifi_manager.h` —— 公开 API

**关键设计**:
- 状态机有 6 个状态：`PROVISION_AP`（初始配网 AP 模式）→ `APSTA_TRYING`（尝试同时运行 AP+STA）→ `APSTA_OK`（AP+STA 成功）→ `AP_FALLBACK`（STA 连接失败，退回纯 AP）→ `STA_ONLY`（纯 STA 模式，关闭 AP）
- 指数退避重连：从 1 秒开始，每次失败加倍，上限 30 秒
- 提供回调注册机制，Wi-Fi 状态变化时通知订阅者
- 提供同步等待接口（`wifi_manager_wait_connected`），支持超时

### 3.2 HTTP 连接复用池 (http_reuse)

**职责**: 管理 HTTP Keep-Alive 连接的 LRU 缓存池，避免频繁建立 TLS 连接的开销。

**源文件**: `esp_http_client_reuse.c`

**关键设计**:
- LRU 淘汰策略，池大小可配置
- 连接按目标主机和端口做 key
- LLM API 调用、Web 搜索、MCP 后端共享同一个连接池
- 需要处理连接超时和服务端主动关闭的情况

### 3.3 显示仲裁器 (display_arbiter)

**职责**: 多个子系统（表情动画引擎、视频播放器、Lua LVGL UI、ESP Painter 绘图库）可能同时想要访问 LCD，显示仲裁器负责串行化这些访问请求。

**源文件**:
- `display_arbiter.c` —— 互斥锁 + 条件变量实现的仲裁逻辑
- `include/display_arbiter.h` —— 公开 API

**关键设计**: 采用非抢占式仲裁 —— 持有者主动释放后下一个等待者才能获取。每个客户端需声明自己的用途和优先级。

### 3.4 ESP Painter 绘图库

**职责**: 在 LCD 上绘制文字、图形、表情动画帧。这是轻量级的软件绘图库。

**源文件**:
- `esp_painter.c` / `include/esp_painter.h` —— 核心 API（画线、矩形、圆、文字、位图）
- `include/esp_painter_font.h` —— 字体接口定义
- `font/basic_font_12.c` ~ `basic_font_48.c` —— 9 种字号的基本 ASCII 点阵字体（12px ~ 48px）

**关键设计**: 字体以 C 数组形式嵌入固件，不依赖外部字库文件。

### 3.5 Captive DNS 配网门户

**职责**: 在 AP 模式下劫持所有 DNS 请求，将用户重定向到配网页面。

**源文件**:
- `captive_dns.c` / `include/captive_dns.h`

**关键设计**:
- 监听 UDP 53 端口的 DNS 查询
- 将所有 A 记录查询返回为 AP 自身的 IP 地址
- 用户连接热点后打开任意 URL 会跳转到设备上的配置页面
- 仅在 AP 模式下激活

### 3.6 设置存储 (settings)

**职责**: 提供 NVS (Non-Volatile Storage) 的键值对读写封装。

**源文件**: `settings_store.c` / `include/settings_store.h`

**关键设计**: 封装 ESP-IDF NVS 的打开/读取/写入/提交/关闭流程，简化上层模块对 NVS 的操作。

### 3.7 语音 WebSocket 共享层 (voice_ws_common)

**职责**: TTS（文本转语音）和 ASR（语音识别）共享同一个 WebSocket 连接服务器的通用逻辑。

**源文件**: `src/voice_ws_common.c` / `include/voice_ws_common.h`

**关键设计**: 封装 WebSocket 连接管理、心跳、断线重连、二进制帧收发，供 `cap_voice` 和 `cap_asr` 共同使用。

---

## 4. Lua 模块层

Lua 模块分为两大类别：硬件驱动层（`lua_driver_*`）和功能模块层（`lua_module_*`）。每一个模块对应一个 ESP-IDF 组件。

### 4.1 硬件驱动层 (lua_driver)

这 6 个模块是对 ESP-IDF 底层驱动的 Lua 封装，让 LLM 生成的 Lua 脚本可以直接操控硬件外设。

**lua_driver_gpio** —— 封装 GPIO 的读写、中断、上下拉配置。Lua 侧提供 `gpio.read(pin)`、`gpio.write(pin, value)`、`gpio.set_mode(pin, mode)` 等接口。需要实现一个 C 函数将 GPIO 编号映射到 ESP-IDF 的 `gpio_set_level` / `gpio_get_level`。

**lua_driver_adc** —— 封装 ADC 模数转换。Lua 侧提供 `adc.read(channel)` 接口。需要处理 ADC 校准和单位转换。

**lua_driver_i2c** —— 封装 I2C 总线读写。Lua 侧提供 `i2c.begin(port, sda, scl)`、`i2c.write(addr, data)`、`i2c.read(addr, len)` 等接口。需要管理多个 I2C 端口的句柄。

**lua_driver_mcpwm** —— 封装 MCPWM（电机控制 PWM）外设，用于驱动舵机。Lua 侧提供 `mcpwm.set_duty(channel, duty)` 等接口。

**lua_driver_touch** —— 封装电容触摸传感器。Lua 侧提供 `touch.read(pin)` 接口。仅在具有触摸传感器的芯片上可用。

**lua_driver_uart** —— 封装 UART 串口通信。Lua 侧提供 `uart.begin(port, baud)`、`uart.write(data)`、`uart.read(len)` 等接口。

每个驱动模块的实现模式相同：头文件声明 `luaopen_<name>` 入口函数，源文件实现该函数并通过 `luaL_Reg` 数组注册所有 Lua 可调用的 C 函数。

### 4.2 功能模块层 (lua_module)

这 21 个模块提供更高层次的设备功能封装。

**lua_module_system** —— 系统级操作：获取芯片信息、重启、获取启动原因、查看内存使用。这是最基础的模块，优先级最高。

**lua_module_delay** —— 延时操作：毫秒/微秒延时。极为简单但 LLM 经常使用。

**lua_module_board_manager** —— 暴露板卡管理器的查询接口：获取板卡名称、获取引脚映射表。LLM 写 Lua 脚本前先查询可用的 GPIO 和外设。

**lua_module_display** —— 通过 LVGL 库在 LCD 上绘制 UI：创建按钮、标签、滑块、图表等控件。需要一个显示硬件抽象层（`display_hal.c`）来桥接 LVGL 和具体的 LCD 驱动。

**lua_module_lcd_touch** —— 读取触摸屏坐标和手势。需要通过 I2C 与触摸控制器（如 FT5x06）通信。

**lua_module_led_strip** —— 控制可寻址 RGB LED 灯带（WS2812/SK6812）。通过 RMT 外设发送脉冲序列。

**lua_module_audio** —— 音频播放和录制：播放 PCM/WAV 文件、录制麦克风输入。需要与板载 ES8311/ES7210 音频 Codec 交互。

**lua_module_camera** —— 摄像头拍照和视频流。封装 ESP32-CAM 驱动，提供拍照、设置分辨率、开启预览等功能。还需要一个后台服务任务（`lua_module_camera_service.c`）处理视频流传输。

**lua_module_button** —— 板载物理按键读取：按下、释放、长按事件检测。需要消抖处理。

**lua_module_imu** —— 惯性测量单元（加速度计+陀螺仪）。通过 I2C 读取 MPU6050/QMI8658 等芯片数据。

**lua_module_environmental_sensor** —— 环境传感器：温度、湿度、气压（如 BME280、SHT30）。

**lua_module_esp_heap** —— 查询 ESP32 堆内存状态：内部 RAM 剩余、PSRAM 剩余、历史最低空闲值。用于调试内存问题。

**lua_module_storage** —— FATFS 文件操作：读写文件、列出目录、删除文件、查询磁盘空间。LLM 通过此模块管理 `/fatfs/` 分区中的内容。

**lua_module_event_publisher** —— 向事件总线发布事件。Lua 脚本可以通过此模块触发 Event Router 的规则匹配，间接调用其他能力。

**lua_module_call_capability** —— 在 Lua 中直接调用其他能力（Capability）。避免 LLM 需要多轮 tool call 才能完成一个操作。

**lua_module_ir** —— 红外发射和接收：发送 NEC/RAW 格式红外码、学习红外遥控器按键。

**lua_module_knob** —— 旋转编码器（旋钮）读取：旋转方向、步数、按下事件。通过 GPIO 中断实现。

**lua_module_lcd** —— LCD 底层控制：背光亮度、屏幕旋转方向。

**lua_module_magnetometer** —— 磁力计读取：指南针方向、磁场强度。

**lua_module_sci** —— 串行通信接口模块，用于连接外部 MCU（如 DFRobot RP2040 SCI 采集模块）。

**lua_module_fuel_gauge** 和 **lua_module_ssd1306** —— 目录存在但尚未实现源码。

每个功能模块的实现模式与驱动模块一致：通过在 `luaL_Reg` 数组注册 C 函数表，暴露给 Lua 虚拟机。模块名需要加入 NVS 中的 `enabled_lua_modules` 白名单才能真正对 LLM 可用。

---

## 5. 核心引擎层

核心引擎是 `components/claw_modules/` 下的 5 个模块，它们共同构成了 Agent 的大脑。

### 5.1 能力插件框架 (claw_cap)

**源文件**:
- `src/claw_cap.c` —— 框架核心实现
- `include/claw_cap.h` —— 公开 API 和类型定义

**职责**: 提供一个统一的插件注册和执行框架。每个能力（Capability）以 `claw_cap_descriptor_t` 描述符的形式注册，包含：

- **group_id**: 能力组标识符，用于分组管理
- **capability.name**: LLM 可见的工具函数名
- **capability.description**: 工具的用途描述（会进入 LLM 上下文）
- **capability.parameters**: JSON Schema 字符串，定义工具的输入参数类型和约束
- **call**: 执行函数指针，LLM 调用该工具时框架会调用此函数
- **init/deinit**: 生命周期回调

**关键功能**:
- 根据 `llm_visible_cap_groups` 白名单控制哪些工具的 JSON Schema 暴露给 LLM（通过 `claw_cap_tools_provider` 上下文提供者注入）
- `claw_cap_call_from_core()`: 接收来自 claw_core 的工具调用请求，解析工具名和 JSON 参数，找到对应的描述符，调用其 `call` 函数，返回结果
- `claw_cap_start_all()`: 遍历所有已注册描述符，依次调用 init
- 支持动态增删能力（`claw_cap_register` / `claw_cap_unregister`）

### 5.2 事件路由器 (claw_event_router)

**源文件**:
- `src/claw_event_router.c` —— 核心路由逻辑
- `src/claw_event.c` —— 事件定义和工具函数
- `include/claw_event_router.h` —— 公开 API
- `include/claw_event_publisher.h` —— 事件发布接口
- `include/claw_event.h` —— 事件结构体定义

**职责**: 事件总线，接收来自 IM 平台、定时器、传感器等各种来源的事件，根据 JSON 规则文件 (`router_rules.json`) 进行匹配，执行匹配后的动作。

**6 种动作类型**:
- `CALL_CAP`: 直接调用某个能力（不经过 LLM）
- `RUN_AGENT`: 将事件内容作为消息发送给 LLM Agent 处理
- `RUN_SCRIPT`: 执行一段 Lua 脚本
- `EMIT_EVENT`: 触发另一个事件（级联）
- `SEND_MESSAGE`: 通过 IM 通道直接发送消息
- `DROP`: 丢弃事件

**5 种会话策略**: 决定事件匹配后创建的会话如何处理（新会话、追加到现有会话、替换等）。

**出站通道绑定**: 各 IM 平台的能力可以通过 `claw_event_router_register_outbound_binding` 注册自己的发送函数，当 LLM 响应需要通过特定 IM 通道返回时，Event Router 负责路由到正确的 IM 平台。

**内部架构**:
- 一个独立的 FreeRTOS 任务（`task_stack_size`: 8KB, `task_priority`: 5）
- 两个超时参数：提交超时 1000ms 和接收超时 130000ms
- 规则存储在 FATFS 的文件中，支持运行时热重载
- `default_route_messages_to_agent` 标志控制未匹配规则的消息是否默认发送给 LLM

### 5.3 记忆系统 (claw_memory)

**源文件**:
- `src/claw_memory.c` —— 核心初始化和 API 入口
- `src/claw_memory_session.c` —— 会话管理（消息追加、摘要生成）
- `src/claw_memory_storage.c` —— 文件存储操作（读写 JSONL 记录）
- `src/claw_memory_extract.c` —— 异步自动提取（从对话中提取长期记忆）
- `src/claw_memory_cap.c` —— 记忆能力注册（给 LLM 暴露记忆操作工具）
- `src/claw_memory_profile.c` —— 用户档案管理（identity.md）
- `src/claw_memory_lightweight.c` —— 轻量级记忆模式实现
- `src/claw_memory_utils.c` —— 工具函数
- `src/claw_memory_internal.h` —— 内部头文件
- `include/claw_memory.h` —— 公开 API

**两种工作模式**:
- **FULL 模式**: 结构化记忆。对话以带时间戳的 JSONL 记录存储，每次 LLM 回复后异步执行自动提取（用小模型从对话中提取可长期记住的事实），生成记忆索引文件。LLM 可通过 `memory_ops` 技能手动管理记忆。
- **LIGHTWEIGHT 模式**: 仅维护一个简单的 `MEMORY.md` 文件。没有自动提取，记忆操作更轻量。

**5 个 Context Provider**（供 claw_core 在每次 LLM 请求时注入上下文）:
- 档案提供者：注入用户身份档案
- 长期记忆提供者：注入检索到的长期记忆摘要
- 会话历史提供者：注入当前会话的最近 N 轮对话
- 技能列表提供者：注入可用技能目录
- 工具提供者：注入 LLM 可见的能力工具定义

记忆系统内部使用独立的 LLM 客户端（与小模型对话用于自动提取），复用相同的 LLM API 配置但可以使用更小的模型。

### 5.4 技能系统 (claw_skill)

**源文件**:
- `src/claw_skill.c` —— 技能加载和激活逻辑
- `include/claw_skill.h` —— 公开 API

**职责**: 管理面向用户的技能（Skill）。技能以 `SKILL.md` 文件的形式存储，包含 YAML frontmatter 元数据和 Markdown 正文。

**核心流程**:
- 初始化时扫描 `skills_root_dir` 目录下的所有 `SKILL.md` 文件，解析 YAML frontmatter 提取 `name`、`description`、`metadata.cap_groups`
- LLM 看到技能列表（通过 `skills_list_provider` 注入），可以调用 `activate_skill` 工具激活某个技能
- 激活时读取 `SKILL.md` 正文注入 LLM 上下文，同时将技能声明的 `cap_groups` 设为 LLM 可见
- 支持运行时的技能状态管理（跟踪哪些技能当前已激活）

### 5.5 ReAct 推理核心 (claw_core)

这是整个系统中最复杂的模块，实现了标准的 ReAct (Reasoning + Acting) 循环。

**源文件**:
- `src/claw_core.c` —— 主循环：请求队列 → 上下文收集 → LLM 调用 → 工具执行 → 循环
- `src/claw_core_llm.c` / `src/claw_core_llm.h` —— LLM 调用逻辑，与后端无关的抽象层
- `src/claw_task.c` —— FreeRTOS 任务封装
- `include/claw_task.h` —— 任务创建工具
- `src/llm/claw_llm_types.h` —— LLM 通用类型定义（消息、工具调用、完成结果等）
- `src/llm/claw_llm_runtime.c/h` —— LLM 运行时：组合后端 + 传输，管理完整的 API 调用生命周期
- `src/llm/claw_llm_http_transport.c/h` —— HTTP 传输层：处理请求/响应序列化、UTF-8 清理、流式响应解析
- `src/llm/media/claw_media_pipeline.c/h` —— 媒体管线：处理图片/音频的编码和预处理
- `src/llm/backends/claw_llm_backend_anthropic.c/h` —— Anthropic Messages API 后端
- `src/llm/backends/claw_llm_backend_openai_compatible.c/h` —— OpenAI 兼容 API 后端
- `src/llm/backends/claw_llm_backend_custom.c/h` —— 自定义 API 后端（任意兼容格式）
- `include/claw_core.h` —— 公开 API

**ReAct 主循环的详细流程**:

1. 从请求队列取出一条请求（请求可能来自 Event Router 的消息路由、定时器触发、串口 CLI 输入等）
2. 依次调用所有已注册的 Context Provider，收集当前上下文字符串（档案 + 长期记忆 + 会话历史 + 技能列表 + 工具定义）
3. 拼装完整的消息列表：系统提示词 + 上下文 + 对话历史 + 当前请求
4. 通过 LLM 运行时层调用远程 API，支持流式响应
5. 解析 LLM 返回的内容：可能是纯文本回复，也可能包含工具调用（tool_calls）
6. 如果是纯文本回复：通过 Event Router 的出站绑定将响应发送回 IM 通道，本轮结束
7. 如果是工具调用：调用 `claw_cap_call_from_core()` 执行工具，将工具返回结果作为新的消息追加到消息列表，返回步骤 4 继续循环
8. 最多循环 32 轮（`max_tool_iterations`），超过则强制终止

**LLM 后端架构**:

- 三个后端实现共享一个统一的接口：`claw_llm_runtime` 抽象了与具体 API 的交互
- Anthropic 后端需要特殊处理 `thinking` 块（Claude 的思考过程），以及合并连续的工具结果消息（Anthropic API 要求 user 和 assistant 消息交替，多个 tool_result 需要放在同一条 user 消息中）
- OpenAI 兼容后端支持任何兼容 OpenAI Chat Completions API 的服务
- Custom 后端允许用户定义任意 HTTP 请求格式

**HTTP 传输层**:

- 管理 HTTP 连接（复用 `http_reuse` 连接池）
- 流式响应支持：解析 SSE (Server-Sent Events) 格式，实时推送 token 到上层
- UTF-8 清理：确保流式响应在任意字节边界截断时不会产生无效的 UTF-8 序列
- 支持请求中断：当新请求进入队列时，可以取消正在进行的 LLM 调用

**FreeRTOS 任务配置**:
- 栈大小：16KB
- 优先级：5（与其他模块相同，时间片轮转）
- CPU 核心：不绑定（`tskNO_AFFINITY`）
- 请求队列长度：4
- 响应队列长度：4
- 最大上下文提供者：8

---

## 6. 能力插件层

能力插件是 `components/claw_capabilities/` 下的 17 个组件。每个能力提供 LLM 可调用的工具函数。按功能分组：

### 6.1 核心能力（始终需要）

**cap_system** —— 系统级操作工具。提供：设备重启、获取设备信息（芯片型号、固件版本、运行时间）、获取内存使用、OTA 固件升级。不依赖外部服务。

**cap_files** —— FATFS 文件操作工具。提供：读取/写入/追加文件、列出目录、删除文件、检查文件是否存在、获取磁盘空间。直接操作 FATFS 分区。

**cap_time** —— 时间管理工具。提供：获取当前时间（含格式化）、设置时区、SNTP 时间同步服务。需要启动一个后台任务定期同步 NTP 服务器。与 `cap_scheduler` 通过回调联动——当首次时间同步成功时通知调度器重新校准。

**cap_skill_mgr** —— 技能管理工具。提供：列出所有可用技能、激活指定技能、反激活技能、获取当前已激活技能列表。LLM 通过此能力动态解锁其他能力组的可见性。包含串口 CLI 命令用于调试。

**cap_router_mgr** —— 路由规则管理工具。提供：查看当前路由规则列表、添加/修改/删除规则、重载规则文件。也包含 CLI 命令。规则编辑直接操作 FATFS 上的 `router_rules.json`。

**cap_session_mgr** —— 会话管理工具。提供：创建新会话、切换会话、列出最近会话、清理过期会话。负责生成会话 ID（用于 Event Router 的会话策略）。

**cap_lua** —— Lua 脚本执行工具。最重要的能力之一。提供：同步执行 Lua 脚本、异步执行 Lua 脚本（在独立 FreeRTOS 任务中运行，用于长时间执行的脚本如 LED 灯带循环动画）。包含完整的 Lua 运行时管理、脚本输出捕获、超时控制。内部有 `cap_lua_runtime.c`（Lua VM 管理）、`cap_lua_async.c`（异步脚本任务）、`cap_lua_internal.h`（内部共享状态）。

### 6.2 LLM 相关能力

**cap_llm_inspect** —— LLM 调用检查工具。提供：查看当前 LLM 配置（后端类型、模型名、API 端点）、查看最近的 LLM 请求/响应日志。用于调试 Agent 行为。

**cap_web_search** —— Web 搜索工具。提供：通过 Brave Search API 或 Tavily Search API 进行网络搜索、获取搜索结果摘要、抓取 URL 内容。LLM 可以主动搜索互联网获取实时信息。

### 6.3 语音能力

**cap_voice** —— 文本转语音 (TTS) 工具。提供：将文本发送到本地 WebSocket TTS 服务器、接收 Opus 编码的音频数据、通过音频 Codec 解码并播放 PCM 音频。需要音频 Codec 硬件支持。使用 `voice_ws_common` 管理 WebSocket 连接。

**cap_asr** —— 自动语音识别 (ASR) 工具。提供：启动麦克风录音、实时发送音频流到 WebSocket ASR 服务器、接收识别转写结果、静音检测自动停止。支持最大录音时长和静音超时配置。也需要音频 Codec 硬件支持。

### 6.4 IM 平台能力

**cap_im_platform** —— 最大的能力组件，包含 5 个 IM 平台的实现。每个平台作为一个独立的子组件：

- **cap_im_qq**: QQ 机器人（QQ Bot API），处理消息接收和发送
- **cap_im_feishu**: 飞书机器人（Lark API），处理消息接收和发送
- **cap_im_tg**: Telegram Bot（Telegram Bot API），接收消息和长轮询
- **cap_im_wechat**: 微信机器人（WeChat Official Account API），含二维码登录流程、会话保持
- **cap_im_attachment**: IM 附件管理，处理图片/文件的上传和下载（共享给所有 IM 平台）

每个 IM 平台的实现模式相同：
- 启动一个后台任务轮询/WebSocket 接收消息
- 将收到的消息包装为 `claw_event_t` 事件，通过 Event Router 发布
- 注册出站绑定（`register_outbound_binding`），提供发送消息的函数
- 包含 CLI 命令用于手动测试

**cap_im_local** —— 本地/Web IM 能力。为 Web 管理界面中的 WebIM 功能提供消息通道。通过 WebSocket 与浏览器通信，不依赖外部 IM 平台。依赖 `cap_session_mgr` 提供的 `roll_chat_session` 来管理会话。

### 6.5 其他能力

**cap_scheduler** —— 定时任务调度器。提供：创建/删除/暂停/恢复定时任务，支持 cron 表达式或固定间隔。任务触发时通过 Event Router 发布事件。内部文件：
- `cap_scheduler.c`: 核心调度循环（1 秒 tick）
- `cap_scheduler_parser.c`: 解析调度表达式
- `cap_scheduler_store.c`: 持久化调度配置到 JSON 文件
- `cap_scheduler_internal.h`: 内部数据结构

**cap_servo** —— 舵机控制。提供：设置舵机角度、回中、呼吸动画。通过 MCPWM 外设驱动两个舵机通道。内部文件：
- `cap_servo.c`: 能力注册和 LLM 工具接口
- `servo_mcpwm.c`: MCPWM 硬件驱动封装
- `motion.c`: 运动规划（缓动函数、呼吸动画轨迹）
- `motion.h` / `servo_mcpwm.h`: 接口定义

舵机有一个独特的设计：接收 LLM 请求开始和完成的通知，在 LLM 推理期间让舵机做思考动作（微动），完成后做确认动作（点头），空闲时做呼吸动画。这些通过 `claw_core` 的 `on_request_start` 和 `completion_observer` 回调实现。

**cap_mcp_client** —— MCP (Model Context Protocol) 客户端。提供：通过 mDNS 自动发现局域网内的 MCP 服务器、连接服务器、调用服务器提供的工具、将服务器工具注入 LLM 可见工具列表。通过此能力，LLM 可以调用本地网络中的其他 AI 工具服务。

**cap_mcp_server** —— MCP 服务端。提供：将 ESP-Claw 自身的能力通过 MCP 协议暴露给网络中的其他 AI Agent。这样多个 AI 设备可以组成协作网络。

**cap_boards** —— 板卡硬件信息技能生成器。不提供 LLM 工具，而是在构建时通过 Python 脚本 (`tools/generate_board_skill.py`) 自动生成板卡硬件信息技能文件，告知 LLM 当前设备有哪些 GPIO、外设和传感器可以使用。

### 6.6 能力的通用实现模式

每个能力组件遵循相同的实现模板：

1. 一个头文件（`include/cap_<name>.h`）声明 `init`、`register`、`deinit` 函数
2. 一个源文件（`src/cap_<name>.c`）实现 LLM 工具函数、注册 claw_cap 描述符
3. 可选的 CLI 命令文件（`src/cmd_cap_<name>.c` 和对应头文件）提供串口调试命令
4. 一个 `CMakeLists.txt` 声明编译目标和依赖
5. 在 `app_capabilities.c` 的注册数组中添加条目（受 Kconfig 宏控制条件编译）
6. 能力组 ID 加入 NVS 中的 `enabled_cap_groups` 白名单才能启用

---

## 7. 应用组装层

应用组装层将上述所有组件串联成一个完整的固件。核心代码在 `components/common/app_claw/` 下。

### 7.1 配置管理 (app_config)

**源文件**:
- `application/edge_agent/components/app_config/app_config.c`
- `application/edge_agent/components/app_config/include/app_config.h`

**职责**: 
- 定义完整的应用配置结构体（Wi-Fi、LLM、IM 平台、搜索 API、能力开关、Lua 模块白名单等，约 30+ 字段）
- 实现 NVS 的读写操作：将结构体序列化到 NVS / 从 NVS 反序列化
- 提供配置验证逻辑（如 Wi-Fi SSID 长度检查）
- 提供 `app_config_to_claw()` 函数，将应用配置映射为 `app_claw_config_t` 结构

### 7.2 能力注册表 (app_capabilities)

**源文件**:
- `components/common/app_claw/app_capabilities.c`
- `components/common/app_claw/include/app_capabilities.h`

**职责**: 维护一个静态的能力组注册数组 `s_capability_group_entries[]`，每一项包含：
- 能力组 ID
- 初始化/准备函数指针
- 注册描述符函数指针
- 默认启用状态

初始化时遍历数组：
1. 解析 `enabled_cap_groups` 白名单（逗号分隔字符串）→ 布尔数组
2. 解析 `llm_visible_cap_groups` 白名单 → 布尔数组
3. 对每个启用的条目：调用 `prepare`（如果需要）、调用 `register_descriptors`
4. 收集所有 LLM 可见的能力组 ID → 调用 `claw_cap_set_llm_visible_groups()`
5. 调用 `claw_cap_start_all()` 启动所有已注册的能力

### 7.3 Lua 模块注册表 (app_lua_modules)

**源文件**:
- `components/common/app_claw/app_lua_modules.c`
- `components/common/app_claw/include/app_lua_modules.h`

**职责**: 类似能力注册表，维护一个 Lua 模块注册数组。每个条目包含模块名和 Lua 初始化函数指针。初始化时解析 `enabled_lua_modules` 白名单，将匹配的模块注册到 Lua 虚拟机中。

### 7.4 Agent 组装 (app_claw)

**源文件**:
- `components/common/app_claw/app_claw.c`
- `components/common/app_claw/include/app_claw.h`

**职责**: 定义系统提示词，并实现 `app_claw_start()` 函数——这是整个项目的组装入口。

**系统提示词设计**:
- 定义角色：AIRI，一个有屏幕形象、能通过动画表达情感的虚拟角色
- 对话风格指令：简洁自然、根据语境主动切换情感动画
- 技能使用规则：将 Skills List 视为可选技能目录，用 `activate_skill` 并行加载多个技能
- 记忆使用规则：按 `CONFIG_APP_CLAW_MEMORY_MODE_FULL` 条件编译，区分完整模式（结构化记忆 + 手动管理）和轻量模式

**app_claw_start() 组装流程**:

1. 检查 LLM 配置完整性（API Key + Model + Backend Type 三项都非空才算已配置）
2. 初始化 Event Router —— 配置规则文件路径、任务参数、会话策略
3. 初始化 Scheduler —— 配置定时文件路径、tick 间隔、最大任务数
4. 初始化 Memory —— 配置 LLM 参数（用于自动提取）、会话参数
5. 初始化 Skills —— 指定技能目录和会话状态目录
6. 初始化 Capabilities —— 调用 `app_capabilities_init()` 注册所有能力
7. 注册 IM 出站绑定 —— 将每个 IM 平台（QQ/飞书/Telegram/微信/WebIM）的发送函数绑定到 Event Router
8. 配置 claw_core —— 填充所有 LLM 参数（从 NVS 配置中读取），设置上下文提供者回调：
   - 档案提供者
   - 长期记忆提供者（完整或轻量模式二选一）
   - 会话历史提供者
   - 技能列表提供者
   - 工具定义提供者
9. 注册舵机观察者（如果启用了 servo）—— 拦截请求开始和完成的回调
10. 启动 claw_core（仅在 LLM 已配置时）
11. 启动 Event Router
12. 启动 Scheduler
13. 启动 NTP 时间同步（如果启用了 time 能力）
14. 启动 CLI（如果启用了 CLI）
15. 发布启动完成事件 —— 触发任何匹配启动事件的路由规则（例如自动激活默认技能）

### 7.5 串口 CLI (app_claw_cli)

**源文件**:
- `components/common/app_claw/app_claw_cli.c`
- `components/common/app_claw/app_claw_cli.h`

**职责**: 注册一个 ESP-IDF 控制台命令行，提供运行时诊断和对 Agent 发送消息的能力。各子模块（IM、调度器、Lua 等）也注册自己的 CLI 子命令。

### 7.6 ASR 按键处理 (app_asr_button)

**源文件**:
- `components/common/app_claw/app_asr_button.c`
- `components/common/app_claw/app_asr_button.h`

**职责**: 如果板卡有物理按键，可以将一个 GPIO 引脚配置为 ASR 触发键 —— 按下开始录音，松开停止并识别。

### 7.7 应用入口 (main.c)

这是固件的入口文件，实现 ESP-IDF 要求的 `app_main()` 函数。

**14 步启动序列**:

1. **分配运行时状态**: 在堆上分配三个结构体（应用配置、claw 配置、存储路径）
2. **初始化 NVS**: 调用 `nvs_flash_init()`，处理分区满或版本更新导致的擦除
3. **初始化应用配置**: 从 NVS 加载所有运行时配置
4. **设置时区**: 根据 NVS 中的时区字符串设置系统 TZ 环境变量
5. **初始化板卡管理器**: 调用 `esp_board_manager_init()`，触发所有板卡设备和外设的初始化（LCD 亮起、触摸开始工作、音频 Codec 配置 I2S 等）
6. **初始化视频播放器**: 扫描 SD 卡目录
7. **初始化 FATFS**: 挂载 `storage` 分区到 `/fatfs`，格式化为 FAT（支持 wear-leveling），如果分区不存在则自动格式化
8. **初始化 Wi-Fi 管理器**: 创建 Wi-Fi 任务和状态机
9. **初始化 HTTP 服务**: 创建 HTTP 服务器实例，注册配置读写回调、Wi-Fi 状态查询回调、微信登录相关回调
10. **注册 Wi-Fi 状态回调**: 当 Wi-Fi 状态变化时打印日志
11. **启动 Wi-Fi**: 如果 NVS 中有 STA 凭据则尝试连接（等待最多 30 秒），同时启动 AP 模式作为 fallback；启动 HTTP 服务器和 Captive DNS
12. **启动 Agent 引擎**: 初始化存储路径（计算出 `/fatfs/sessions`、`/fatfs/memory`、`/fatfs/skills` 等路径），调用 `app_claw_start()`
13. **绑定 WebIM**: 如果启用了本地 IM，将 WebSocket 消息通道绑定到 Event Router
14. **启动表情动画**: 初始化情绪视频控制器，开始播放默认空闲动画

启动完成后，可选启用内存监控任务（每 20 秒打印一次内部 RAM 和 PSRAM 使用情况），然后释放临时分配的运行时状态结构体，进入空闲循环。

### 7.8 Wi-Fi CLI 命令 (cmd_wifi)

**源文件**: `application/edge_agent/components/cmd_wifi/cmd_wifi.c` 和 `.h`

注册串口控制台中的 `wifi` 命令组：`wifi status`（查看状态）、`wifi set-ssid`（设置凭据）、`wifi connect`（手动连接）等。

---

## 8. HTTP 服务与 Web 管理界面

### 8.1 HTTP 服务核心 (http_server)

**源文件**:
- `http_server_core.c` —— HTTP 服务器初始化、路由注册、启动/停止
- `http_server_config_api.c` —— 配置 API（GET/PATCH 配置字段）
- `http_server_capabilities_api.c` —— 能力管理 API（启用/禁用能力组）
- `http_server_lua_modules_api.c` —— Lua 模块管理 API
- `http_server_files_api.c` —— 文件管理 API（浏览/上传/下载 FATFS 文件）
- `http_server_status_api.c` —— 设备状态 API（Wi-Fi 连接状态、IP、运行时间）
- `http_server_webim_api.c` —— WebIM WebSocket API（实时聊天）
- `http_server_emotion_api.c` —— 情感动画 API
- `http_server_wechat_api.c` —— 微信登录 API（获取二维码、查询状态）
- `http_server_assets.c` —— 静态资源服务（前端 SPA 的 HTML/JS/CSS）
- `http_server_json.c` —— JSON 解析/序列化工具
- `http_server_utils.c` —— 通用工具函数
- `http_server_priv.h` —— 内部头文件
- `include/http_server.h` —— 公开 API 和配置结构体

**REST API 路由**:
- `GET /api/status` —— 设备状态
- `GET /api/config?groups=wifi,llm` —— 按分组查询配置
- `PATCH /api/config` —— 部分更新配置（只更新传入的字段）
- `GET /api/capabilities` —— 查询能力组状态
- `GET /api/files?path=/fatfs` —— 浏览文件
- `POST /api/files/upload` —— 上传文件
- `WebSocket /api/webim` —— 实时聊天
- `GET /api/wechat/login-status` —— 微信登录状态
- `GET /*` —— 前端 SPA 入口（fallback 到 index.html）

### 8.2 前端 Web 管理界面

前端使用 **SolidJS + Tailwind CSS** 构建，源码位于 `http_server/frontend_source/`。

**构建流程**:
- 开发阶段使用 Vite 开发服务器
- 生产构建输出静态文件到指定目录
- 静态文件会被 `http_server_assets.c` 嵌入固件作为 SPIFFS 资源（或放在 FATFS 分区的 `static/` 目录）

**页面结构**:
- **Layout**: 侧边栏 + 内容区布局，移动端响应式适配，Tab 切换时未保存修改的确认提示
- **控制台 (Chat)**: WebIM 实时聊天界面，通过 WebSocket 与 Agent 对话
- **配置管理**: 按分组（Wi-Fi、LLM、IM、搜索、能力、技能、时间）以表单方式编辑配置，支持保存到 NVS
- **文件管理**: FATFS 文件浏览/上传/编辑
- **状态面板**: 设备运行状态、Wi-Fi 连接信息、内存使用
- **国际化 (i18n)**: 支持多语言切换

### 8.3 视频播放器 (video_player)

**源文件**:
- `video_player.c` / `include/video_player.h` —— SD 卡视频文件扫描和循环播放
- `emotion_video_controller.c` / `include/emotion_video_controller.h` —— 情绪动画控制器，将 10 种情绪（高兴、悲伤、惊讶、愤怒、思考、点头、挥手等）映射到对应的 MP4 视频文件
- `app_extractor.c/h` —— 媒体提取器封装（解析 MP4/AVI 容器格式）
- `app_stream_adapter.c/h` —— 视频流适配器（将解码后的帧送到 LCD 显示仲裁器）
- `ppa_blend.c/h` —— PPA (Pixel Processing Accelerator) 混合加速（视频帧与 UI 叠加的硬件加速）

---

## 9. FATFS 预置内容

`fatfs_image/` 目录的内容会在构建时打包成 Flash 中的 FAT 文件系统镜像。这些文件构成了设备的"出厂设置"。

### 9.1 记忆系统初始化文件

```
fatfs_image/memory/
├── identity.md          # 用户身份档案（空模板，用户可通过对话填充）
├── user.md              # 用户偏好配置
├── soul.md              # Agent 角色设定和核心价值观
├── MEMORY.md            # 轻量级记忆模式的主文件
├── memory_records.jsonl # 结构化记忆记录（空，逐行 JSON）
├── memory_index.json    # 记忆索引（空模板）
└── memory_digest.log    # 记忆提取日志
```

### 9.2 路由规则 (router_rules.json)

定义事件匹配和分发规则。JSON 数组格式，每条规则包含：
- `id`: 规则唯一标识
- `description`: 规则用途描述
- `match`: 匹配条件（事件类型、来源能力、消息内容等）
- `actions`: 匹配后的动作列表（类型 + 参数）

默认规则通常包括：
- 将 IM 文本消息路由到 Agent
- 将启动事件路由到一个欢迎技能
- 将定时器事件路由到对应的处理能力

### 9.3 调度配置 (schedules.json)

定时任务配置文件，JSON 数组格式。每条调度包含：
- `id`: 任务标识
- `cron` 或 `interval`: 触发时间表达式
- `action`: 触发时执行什么（发送消息、调用能力、运行脚本）

### 9.4 预置技能 (skills/)

每个子目录包含一个 `SKILL.md` 文件（可能还有 `scripts/` 子目录）：
- `light_switch`: 通过 GPIO 或 LED 灯带控制灯光
- `weather_search`: 结合网络搜索获取天气信息
- `lua_demo`: Lua 脚本编写示例
- 各种能力对应的技能文档（告诉 LLM 如何使用该能力）

### 9.5 内置 Lua 脚本 (scripts/)

```
fatfs_image/scripts/
├── builtin/       # 内置 Lua 脚本（编译时从 lua_modules 同步）
│   └── *.lua      # 各模块的示例脚本
└── docs/          # Lua 模块 API 文档（Markdown 格式，编译时自动生成）
    └── *.md       # 每个 Lua 模块的 API 参考文档
```

这些文档会在 LLM 上下文不足时作为参考注入，帮助 LLM 写出正确的 Lua 代码。

### 9.6 静态 Web 资源 (static/)

Web 管理界面的前端构建产物以及品牌图片（如 ESP-Claw.png Logo）。

---

## 10. 构建与烧录流程

### 10.1 构建命令

```bash
# 标准增量构建
cd application/edge_agent
idf.py build

# 完整重构建（Kconfig 变更后建议）
idf.py fullclean build

# 指定目标芯片 + 板卡
idf.py set-target esp32s3
idf.py -DCONFIG_ESP_BOARD_M5STACK_CORES3=1 build

# 生产构建（加密 + 签名）
idf.py -DSECURE_FLASH_ENC_ENABLED=1 -DSECURE_BOOT_ENABLED=1 build
```

### 10.2 构建过程详解

1. **CMake 配置阶段**:
   - 检测 `IDF_PATH` 环境变量
   - 应用 ESP-IDF 补丁（两块修复）
   - 检测 Flash 大小 → 选择分区表 CSV
   - 生成 FATFS 镜像构建目标
   - 注册 Skill/Lua 文档同步钩子

2. **编译阶段**:
   - 编译所有 ESP-IDF 组件（FreeRTOS、LWIP、Wi-Fi、SPI、I2C 等）
   - 编译 5 个核心引擎模块
   - 编译 19 个能力插件（受 Kconfig 开关控制）
   - 编译 27 个 Lua 模块（受 Kconfig 开关控制）
   - 编译公共基础设施
   - 编译应用层组件
   - 编译板卡管理器和板卡初始化代码（含自动生成的代码）

3. **链接阶段**: 将所有 .o 文件链接为 ELF，再转换为 BIN

4. **镜像生成阶段**:
   - 打包 FATFS 镜像（将 `fatfs_image/` 目录内容写入二进制文件）
   - 合并所有分区镜像（bootloader + 分区表 + ota_0 + storage）

### 10.3 烧录命令

```bash
# 完整烧录（首次或分区表变更后）
idf.py flash

# 指定串口
idf.py -p /dev/ttyUSB0 flash

# 烧录 + 打开串口监控
idf.py flash monitor

# 仅烧录 app 分区（开发阶段快速迭代）
esptool.py --chip esp32s3 write_flash <ota_0_address> build/edge_agent.bin
```

---

## 11. 调试与诊断

### 11.1 串口日志

在代码中使用 `ESP_LOGE` / `ESP_LOGW` / `ESP_LOGI` / `ESP_LOGD` / `ESP_LOGV` 5 个级别的日志宏。运行时可通过 `esp_log_level_set()` 动态调整。

### 11.2 Crash 分析

当发生 Guru Meditation 或 Panic 时，串口会输出寄存器 dump 和 backtrace 地址列表。使用 `xtensa-esp32s3-elf-addr2line -e build/edge_agent.elf <地址>` 将地址解析为函数名和行号。

### 11.3 Core Dump

在 16/32MB 设备上可以启用 core dump 存储到 Flash。崩溃后通过 `idf.py coredump-info` 查看崩溃时的调用栈和变量值，或通过 `idf.py coredump-debug` 启动 GDB 交互分析。

### 11.4 内存监控

将 `main.c` 中的 `APP_ENABLE_MEM_LOG` 宏改为 `1`，固件会启动一个每 20 秒打印内存使用和任务栈高水位的监控任务。

### 11.5 CLI 命令

串口控制台提供运行时诊断命令：
- `event_router --rules` / `--reload` / `--rule <id>`: 路由规则管理
- `wifi status` / `connect` / `set-ssid`: Wi-Fi 管理
- `lua exec <script>`: 手动执行 Lua 脚本测试
- `mcp discover` / `connect`: MCP 客户端操作

### 11.6 Web 界面

设备联网后浏览器访问 `http://<设备IP>/`，可以：
- 实时查看/修改所有配置
- 通过 WebIM 与 Agent 直接对话
- 查看设备状态（IP、运行时间、内存）
- 浏览和管理 FATFS 文件

---

## 附录：从零实现的推荐顺序

如果要从零搭建一个 ESP-Claw 的简化版，推荐的实现顺序如下：

| 阶段 | 模块 | 原因 |
|---|---|---|
| 1 | 项目骨架 + 构建系统 | 没有构建系统一切无从谈起 |
| 2 | 板卡支持 (1块板) | 能点亮屏幕、驱动外设是后续所有工作的前提 |
| 3 | wifi_manager + captive_dns | 设备需要联网才能调用 LLM API |
| 4 | http_reuse + settings | 网络和存储的基础设施 |
| 5 | claw_cap | 插件框架是能力层的基础 |
| 6 | claw_event_router | 事件总线是消息流转的枢纽 |
| 7 | claw_core（仅 OpenAI 后端） | Agent 推理核心，先用最简单的后端跑通 ReAct 循环 |
| 8 | cap_system + cap_files | 最基础的两个能力，验证能力框架可用 |
| 9 | cap_lua + 1-2 个 lua_module | 让 Agent 能操控硬件（点灯/读传感器） |
| 10 | claw_memory + claw_skill | 记忆和技能系统让 Agent 更智能 |
| 11 | app_claw（组装） + main.c | 将所有组件串联 |
| 12 | http_server + Web 界面 | 提供配置和对话的 Web 入口 |
| 13 | cap_im_platform (1个平台) | 连接 IM 平台 |
| 14 | 其余能力和 Lua 模块 | 按需逐步扩展 |
