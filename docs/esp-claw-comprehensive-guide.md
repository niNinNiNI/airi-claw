# ESP-Claw 固件项目全面解析

> 本文档从四个维度全面解析 ESP-Claw 项目：开发步骤、核心代码运行流程、与 PC Claude Code Agent 的差异、以及在资源受限开发板上运行的原理。

---

## 目录

1. [项目开发步骤](#1-项目开发步骤)
2. [核心代码运行步骤](#2-核心代码运行步骤)
3. [与 PC Claude Code Agent 的差异](#3-与-pc-claude-code-agent-的差异)
4. [资源受限环境下的运行原理](#4-资源受限环境下的运行原理)

---

## 1. 项目开发步骤

### 1.1 项目初始化与构建系统

ESP-Claw 基于 **ESP-IDF v5.5.4** 构建，项目根位于 `application/edge_agent/`。

**第一步：创建 ESP-IDF 项目骨架**

需要产出：
- `main/CMakeLists.txt` — 构建入口
- `main/main.c` — `app_main()` 固件入口
- 项目级 `CMakeLists.txt` — 全局构建脚本
- `sdkconfig.defaults` — FreeRTOS、LWIP、PSRAM、FATFS、LVGL 等底层配置固化

**第二步：依赖管理**

在 `main/idf_component.yml` 声明外部依赖：
- `esp_board_manager` (^0.5.10) — YAML 驱动的板卡硬件抽象
- `esp_websocket_client` (^1.4.0) — WebSocket 客户端库
- 所有本地组件（app_claw、wifi_manager、http_server 等）以相对路径方式声明

**第三步：CMake 构建脚本**

项目级 `CMakeLists.txt` 的核心职责：
1. **ESP-IDF 补丁** — 通过 `esp_idf_patch.cmake` 修复 v5.5.4 的 LCD 并口和 USB IAD 描述符 bug
2. **分区表自动选择** — 从 `esp_board_manager` 配置检测 Flash 大小（8/16/32MB），自动选择对应分区表
3. **FATFS 镜像生成** — 将 `fatfs_image/` 打包为 Flash 中的 FAT 文件系统
4. **Skill/Lua 文档同步** — 构建时自动将项目中的 SKILL.md 和 Lua 模块文档同步到 FATFS 镜像

**第四步：Kconfig 菜单系统**

通过 `main/Kconfig.projbuild` 定义：
- Wi-Fi 默认凭据（SSID、密码、AP 前缀）
- 搜索 API Key 默认值
- 视频播放器开关
- 能力/Lua 模块的编译时开关

---

### 1.2 板卡支持层

每块开发板需要三个 YAML 描述文件，存放在 `application/edge_agent/boards/<vendor>/<board_name>/`：

| 文件 | 内容 |
|---|---|
| `board_info.yaml` | 芯片型号、板卡名称、硬件版本、制造商 |
| `board_devices.yaml` | 板载设备（LCD、Audio Codec、Camera、Touch、IMU）及引脚映射 |
| `board_peripherals.yaml` | 外设子系统详细引脚分配（SPI/I2C/I2S/SDMMC 等） |

还需要 `setup_device.c` 实现板级初始化（电源管理、上电顺序、IO 扩展器配置）。

构建时 `esp_board_manager` 自动生成 5 个文件到 `components/gen_bmgr_codes/`，无需手写。

---

### 1.3 公共基础设施层（按实现优先级）

| 序号 | 组件 | 职责 |
|---|---|---|
| 1 | **wifi_manager** | 6 状态 Wi-Fi 状态机（PROVISION_AP → APSTA_TRYING → APSTA_OK/AP_FALLBACK → STA_ONLY），指数退避重连（1s~30s 上限），Captive DNS 配网 |
| 2 | **http_reuse** | HTTP Keep-Alive 连接 LRU 池，LLM/Search/MCP 共享 |
| 3 | **display_arbiter** | 多子系统（Emote/Video/LVGL/Painter）LCD 访问仲裁 |
| 4 | **esp_painter** | 轻量级软件绘图库，9 种字号点阵字体内嵌固件 |
| 5 | **captive_dns** | DNS 劫持实现配网门户（AP 模式下劫持所有 A 记录到设备 IP） |
| 6 | **settings** | NVS 键值对读写封装 |
| 7 | **app_config** | 完整应用配置结构体（30+ 字段），NVS 序列化/反序列化 |

---

### 1.4 Lua 模块层（31 个模块）

**6 个硬件驱动模块**（LLM 通过 Lua 脚本直接操控硬件）：

| 模块 | 功能 |
|---|---|
| `lua_driver_gpio` | GPIO 读写、中断、上下拉 |
| `lua_driver_adc` | ADC 模数转换 |
| `lua_driver_i2c` | I2C 总线通信 |
| `lua_driver_mcpwm` | 舵机 PWM 控制 |
| `lua_driver_touch` | 电容触摸传感器 |
| `lua_driver_uart` | UART 串口通信 |

**25 个功能模块**：system、delay、board_manager、display (LVGL)、lcd_touch、led_strip、audio、camera、button、imu、environmental_sensor、esp_heap、storage、event_publisher、call_capability、ir、knob、lcd、magnetometer、sci 等。

每个模块实现模式相同：`luaL_Reg` 数组注册 C 函数 → `luaopen_<name>` 入口 → 受 NVS 白名单 `enabled_lua_modules` 控制。

---

### 1.5 核心引擎层（5 个模块）

这是 Agent 的"大脑"，按实现顺序：

**第一步：能力插件框架 `claw_cap`**

定义 `claw_cap_descriptor_t` 结构体：
```c
typedef struct {
    const char *id;              // 能力唯一标识
    const char *name;            // LLM 可见的工具函数名
    const char *description;     // 工具描述（进入 LLM 上下文）
    const char *input_schema_json; // JSON Schema 参数定义
    claw_cap_execute_fn execute; // 执行函数指针
    claw_cap_lifecycle_fn init;  // 生命周期：初始化
    claw_cap_lifecycle_fn start; // 生命周期：启动
} claw_cap_descriptor_t;
```

核心功能：
- 两级可见性控制（`enabled_cap_groups` 编译时 + `llm_visible_cap_groups` 运行时）
- `claw_cap_call_from_core()` — 接收 LLM 工具调用请求，查表分发
- `claw_cap_tools_provider` — 自动生成 OpenAI/Anthropic 格式的工具 JSON Schema

**第二步：事件路由器 `claw_event_router`**

独立 FreeRTOS 任务（8KB 栈，优先级 5），核心功能：
- **6 种动作类型**：`CALL_CAP`（直调能力）、`RUN_AGENT`（发送给 LLM）、`RUN_SCRIPT`（执行 Lua）、`EMIT_EVENT`（级联触发）、`SEND_MESSAGE`（IM 直接回复）、`DROP`（丢弃）
- **5 种会话策略**：新会话、追加、替换等
- **规则文件**：存储在 `/fatfs/router_rules/router_rules.json`，支持运行时热重载
- **出站通道绑定**：各 IM 平台注册自己的发送函数

**第三步：记忆系统 `claw_memory`**

两种工作模式（通过 Kconfig `CONFIG_APP_CLAW_MEMORY_MODE_FULL` 切换）：

| 特性 | FULL 模式 | LIGHTWEIGHT 模式 |
|---|---|---|
| 存储格式 | 结构化 JSONL 记录 + 索引 | 单个 MEMORY.md 文件 |
| 自动提取 | ✅ 小模型异步提取长期记忆 | ❌ |
| 手动管理 | ✅ memory_ops 技能 | ✅ 简化版 |
| 内存开销 | 较大 | 极小 |

提供 5 个 Context Provider（供 claw_core 每次 LLM 请求时注入上下文）：
1. 档案提供者（用户身份）
2. 长期记忆提供者（检索历史记忆摘要）
3. 会话历史提供者（最近 N 轮对话）
4. 技能列表提供者（可用技能目录）
5. 工具提供者（LLM 可见的能力工具定义）

**第四步：技能系统 `claw_skill`**

- SKILL.md 文件格式：YAML frontmatter（name、description、cap_groups）+ Markdown 正文
- 初始化时扫描 `skills_root_dir` 下所有 SKILL.md，解析元数据
- LLM 调用 `activate_skill` → 读取 SKILL.md 正文注入上下文 + 将 `cap_groups` 设为 LLM 可见
- 这正是 PC Claude Code 中 slash command 的嵌入式对应物

**第五步：ReAct 推理核心 `claw_core`**

最复杂的模块（约 1760 行 C 代码），实现标准 ReAct 循环：

```
队列阻塞等待请求 → 5 个 Context Provider 收集上下文
→ 组装系统提示词 + 消息列表 + 工具定义 JSON
→ HTTP 调用远程 LLM API（支持流式 + 中断）
→ 解析响应：纯文本 → 发送回复，结束
→ 解析响应：tool_calls → 本地执行 Capability → 结果追加到消息列表 → 循环
→ 最多 32 轮迭代（max_tool_iterations 硬保护）
```

LLM 后端支持三种 API 格式：Anthropic Messages API、OpenAI Chat Completions API、Custom 自定义格式。

---

### 1.6 能力插件层（19 个组件）

按功能分组：

| 分组 | 能力 | 功能 |
|---|---|---|
| **核心** | cap_system | 设备信息、重启、OTA |
| | cap_files | FATFS 文件读写/列目录 |
| | cap_time | NTP 时间同步、时区管理 |
| | cap_lua | **最重要** — 同步/异步执行 Lua 脚本 |
| | cap_skill_mgr | 技能激活/反激活管理 |
| | cap_router_mgr | 路由规则增删改查 |
| | cap_session_mgr | 会话创建/切换/清理 |
| **LLM** | cap_web_search | Brave/Tavily 网络搜索 |
| | cap_llm_inspect | LLM 配置和日志查看 |
| **语音** | cap_voice | TTS 文本转语音（WebSocket + Opus→PCM） |
| | cap_asr | ASR 语音识别（麦克风→WebSocket→文本） |
| **IM** | cap_im_qq/feishu/tg/wechat | IM 平台消息接收与发送 |
| | cap_im_local | WebIM WebSocket 通道 |
| | cap_im_attachment | IM 附件管理 |
| **其他** | cap_scheduler | Cron 定时任务调度 |
| | cap_servo | 舵机角度控制 + 思考动画 |
| | cap_mcp_client | MCP 协议客户端（mDNS 发现） |
| | cap_mcp_server | MCP 协议服务端 |
| | cap_boards | 板卡硬件信息技能生成 |

---

### 1.7 应用组装层（app_claw）

`app_claw_start()` 函数是总装入口，15 步组装顺序：

```
1. 检查 LLM 配置完整性（API Key + Model + Backend 三项必须都非空）
2. 初始化 Event Router（规则路径、任务参数 8KB/优先级5）
3. 初始化 Scheduler（1 秒 tick、最大 32 项、6KB 栈）
4. 初始化 Memory（配置 LLM 参数、会话参数）
5. 初始化 Skills（扫描 SKILL.md）
6. 初始化 Capabilities（app_capabilities_init → 遍历注册表 → 启动）
7. 注册 IM 出站绑定（QQ/飞书/Telegram/微信/WebIM → Event Router）
8. 配置 claw_core（LLM 参数 + 5 个 Context Provider 回调）
9. 注册舵机观察者（可选，拦截请求开始/完成事件）
10. 启动 claw_core（仅 LLM 已配置时）
11. 启动 Event Router
12. 启动 Scheduler
13. 启动 NTP 时间同步
14. 启动串口 CLI
15. 发布 boot_completed 启动事件
```

---

### 1.8 HTTP 服务与 Web 管理界面

**后端 API**（ESP-IDF HTTP Server）：
- `GET /api/status` — 设备状态
- `GET /api/config` — 按分组查询配置
- `PATCH /api/config` — 部分更新配置
- `GET /api/capabilities` — 查询能力组状态
- `GET /api/files` — FATFS 文件浏览
- `POST /api/files/upload` — 文件上传
- `WebSocket /api/webim` — WebIM 实时聊天
- `GET /api/wechat/login-status` — 微信登录状态

**前端**：SolidJS + Tailwind CSS → 静态文件嵌入固件

---

### 1.9 从零实现的推荐顺序

| 阶段 | 模块 | 原因 |
|---|---|---|
| 1 | 项目骨架 + 构建 | 没有构建系统一切无从谈起 |
| 2 | 板卡支持 (1块板) | 能点亮屏幕是后续前提 |
| 3 | wifi_manager + captive_dns | 设备需要联网调用 LLM |
| 4 | http_reuse + settings | 网络和存储基础设施 |
| 5 | claw_cap | 插件框架是能力层基础 |
| 6 | claw_event_router | 事件总线是消息流转枢纽 |
| 7 | claw_core（仅 OpenAI 后端） | 先打通最简单的 ReAct 循环 |
| 8 | cap_system + cap_files | 验证能力框架可用 |
| 9 | cap_lua + 1-2 lua_module | Agent 能操控硬件 |
| 10 | claw_memory + claw_skill | 记忆和技能让 Agent 更智能 |
| 11 | app_claw（组装）+ main.c | 串联所有组件 |
| 12 | http_server + Web 界面 | Web 配置和对话入口 |
| 13 | cap_im_platform (1个平台) | 接入 IM 消息 |
| 14 | 其余能力和 Lua 模块 | 按需扩展 |

---

## 2. 核心代码运行步骤

### 2.1 启动流程（`app_main()` → 14 阶段线性启动）

```
阶段 1:  app_allocate_runtime_state()
         ├── 堆分配 app_config_t（Wi-Fi/LLM/IM 配置结构体）
         ├── 堆分配 app_claw_config_t（Agent 配置映射）
         └── 堆分配 app_claw_storage_paths_t（/fatfs/ 下各路径）

阶段 2:  init_nvs()
         └── nvs_flash_init() → 处理分区满/版本更新

阶段 3:  app_config_init() → app_config_load()
         └── 从 NVS namespace "app" 加载保存的配置 → s_config

阶段 4:  app_config_to_claw(s_config, s_claw_config)
         └── 将 30+ 字段的应用配置映射为 Agent 配置结构

阶段 5:  init_timezone(s_config->timezone)
         └── setenv("TZ", timezone, 1) → tzset()

阶段 6:  esp_board_manager_init()
         ├── 初始化 LCD 显示屏
         ├── 初始化触摸屏（I2C）
         ├── 初始化音频 Codec（I2S）
         ├── 初始化 SD 卡（SDMMC）
         └── 初始化其他板载外设

阶段 7:  video_player_init()
         └── 扫描 /sdcard/videos 目录的 MP4 文件

阶段 8:  init_fatfs()
         ├── 挂载 "storage" 分区到 /fatfs（FAT + wear-leveling）
         └── 如果分区不存在则自动格式化

阶段 9:  wifi_manager_init() + http_server_init() + wifi_manager_start()
         ├── 创建 Wi-Fi 状态机任务
         ├── 创建 HTTP 服务器实例
         └── 注册 REST API 路由

阶段 10: wifi_manager_register_state_callback(on_wifi_state_changed)
         └── Wi-Fi 状态变化时打印日志

阶段 11: wifi_manager_start() + http_server_start() + captive_dns_start()
         ├── 尝试连接 NVS 中保存的 Wi-Fi（等待最多 30s）
         ├── 同时启动 AP 模式作为 fallback
         ├── 启动 HTTP 服务器监听
         └── 启动 Captive DNS 配网劫持

阶段 12: app_claw_init_storage_paths() + app_claw_start()
         ├── 计算 /fatfs/sessions、/fatfs/memory、/fatfs/skills 等路径
         └── 进入 15 步 Agent 组装流程（见 1.7 节）

阶段 13: http_server_webim_bind_im()
         └── WebSocket 消息通道绑定到 Event Router

阶段 14: emotion_video_controller_init() + emotion_video_start_default()
         ├── 初始化情绪动画控制器
         └── 播放默认空闲动画（idle_shake）
```

启动完成后，可选启动内存监控任务（每 20s 打印 RAM/PSRAM 使用情况），然后释放临时运行时状态，进入空闲。

---

### 2.2 ReAct 推理循环（`claw_core_task()` → 逐轮详解）

这是 ESP-Claw 最核心的运行时行为，位于 `claw_core.c:1206`。

**外层循环：永久阻塞等待请求**

```c
while (true) {
    claw_core_request_item_t request = {0};
    // 阻塞等待，直到有请求进入队列
    xQueueReceive(s_core->request_queue, &request, portMAX_DELAY);
    // ...
}
```

请求可能来自：
- Event Router 将 IM 消息路由到 Agent（最常见）
- 定时器触发的调度事件
- 串口 CLI 输入
- 其他能力的事件转发

---

**每轮迭代的详细步骤：**

#### 第 1 步：入队与中断准备

```
设置 inflight_request_id → 标记当前正在处理的请求
设置 inflight_abort = false → 清除中断标志
claw_llm_http_arm_abort(&inflight_abort) → 注册传输层中断回调
```

关键设计：`inflight_abort` 是 `volatile bool`，HTTP 传输层在读流式响应时持续检查这个标志。当新请求到来需要取消当前请求时，`claw_core_cancel_request()` 将其设为 true，传输层立即终止读取。

#### 第 2 步：调用 on_request_start 回调

```
s_core->on_request_start(&request.view, ...)
```

被舵机能力拦截：`app_claw_servo_request_start()` → `cap_servo_on_activity()` 让舵机开始"思考"动作（微动）。

#### 第 3 步：构建迭代上下文 — `build_iteration_context()`

这是上下文收集的核心，遍历 5 个 Context Provider：

```
for (i = 0; i < context_provider_count; i++) {
    provider->collect(&request, &context, provider->user_ctx);
    
    switch (context.kind) {
    case SYSTEM_PROMPT: → 追加到 system_prompt 字符串
    case MESSAGES:      → 解析 JSON 数组，追加到 messages
    case TOOLS:         → 解析 JSON 数组，追加到 tools
    }
}
```

5 个 Provider 提供的上下文叠加：

```
┌─────────────────────────────────────────────┐
│ System Prompt (APP_SYSTEM_PROMPT)            │  ← 角色定义 + 对话风格
│   ├── claw_memory_profile_provider           │  ← 用户身份档案
│   ├── claw_memory_long_term_provider         │  ← 检索到的长期记忆摘要
│   └── Core Request (当前消息的元信息)          │
├─────────────────────────────────────────────┤
│ Messages Array                               │
│   ├── claw_memory_session_history_provider   │  ← 最近 N 轮对话历史
│   └── request.user_text                      │  ← 当前用户消息
├─────────────────────────────────────────────┤
│ Tools Array                                  │
│   ├── claw_cap_tools_provider                │  ← LLM 可见的能力工具定义
│   └── (MCP 服务器的远程工具)                   │  ← 如果连接了 MCP 服务器
└─────────────────────────────────────────────┘
```

#### 第 4 步：LLM API 调用

```
claw_core_llm_chat_messages(system_prompt, messages, tools_json, &llm_response, &error)
```

这一调用经过三层：

```
claw_core_llm_chat_messages()        ← 组装最终请求 JSON
  → claw_llm_runtime                 ← 选择后端（Anthropic/OpenAI/Custom）
    → claw_llm_http_transport         ← HTTP 传输层
      → esp_http_client_reuse         ← LRU 连接池
        → TCP/TLS → 远程 LLM 服务      ← 实际网络 I/O
```

流式响应解析（SSE → `data: {...}` 逐行解析），UTF-8 边界安全处理，支持中途取消。

#### 第 5 步：解析 LLM 响应

**情况 A — 纯文本回复（无 tool_call）：**

```
publish_stage_note_for_round()          ← 通知 stage note 收集
claw_core_finish_from_plain_text()     ← 复制文本到 response.text
→ 跳出内层循环，进入完成阶段
```

**情况 B — 包含工具调用（tool_calls）：**

```
log_tool_call_names()                  ← 日志记录工具名
publish_stage_tool_calls()             ← 发布阶段事件（如"Adulting: write_lua(...)"）
append_assistant_tool_calls()          ← 将 assistant 消息 + tool_calls 追加到 runtime_messages

for each tool_call:
    claw_core_call_cap(name, args, request, &output)  ← 分发到 claw_cap 框架
    append_tool_summary_line()                        ← 记录工具执行结果摘要
    append tool_result 消息到 runtime_messages         ← 工具结果追加到对话

iteration++ → 如果 iteration < max_tool_iterations(32) → 回到步骤 3
```

#### 第 6 步：完成阶段

```
if (成功 && 有文本回复):
    response.status = OK
    append_session_turn(session_id, user_text, response_text)  ← 写入会话历史
    通知 completion_observers（舵机做"点头"确认动作）

if (失败):
    response.status = ERROR
    build_session_failure_trace(error_message, tool_summary)  ← 生成失败追踪
    append_session_turn() 写入失败记录

publish_out_message_if_requested()     ← 通过 Event Router 发送回复到 IM 通道
push_response() 或 free_response()     ← 将结果放入响应队列或释放
```

---

### 2.3 完整数据流：从消息到回复

```
用户在微信/QQ/Telegram 发送消息
        │
        ▼
IM 平台后台任务（如 cap_im_tg 长轮询）
        │ 包装为 claw_event_t
        ▼
Event Router（接收事件 → 匹配 router_rules.json）
        │ 规则匹配：RUN_AGENT 动作
        ▼
claw_core 请求队列（xQueueSend）
        │
        ▼
claw_core_task 处理（ReAct 循环）
        │
        ├──→ Context Providers（收集记忆/历史/工具/Skills）
        ├──→ LLM API 调用（HTTP → 远程推理）
        ├──→ 如果 LLM 返回 tool_calls:
        │   ├──→ claw_cap_call_from_core()
        │   │   └──→ cap_lua → 执行 Lua 脚本
        │   │       └──→ lua_module_gpio → gpio_write() → 硬件 GPIO
        │   └──→ 工具结果追加到消息列表 → 继续循环
        │
        └──→ 最终文本回复
                │
                ▼
Event Router 出站通道绑定（register_outbound_binding）
        │ 根据 source_channel 路由到正确的 IM 平台
        ▼
IM 平台发送函数（如 tg_send_message）
        │
        ▼
用户收到回复
```

---

### 2.4 关键并发模型

ESP-Claw 中以下这些 FreeRTOS 任务在优先级 5 上时间片轮转运行：

| 任务名 | 栈大小 | 职责 |
|---|---|---|
| `claw_core` | 16KB | ReAct 推理循环 |
| `event_router` | 8KB | 事件匹配与分发 |
| `cap_scheduler` | 6KB | Cron 定时调度 |
| `cap_im_*` | 各 IM 平台的接收任务 | 消息轮询/WebSocket |
| `http_server` | ESP-IDF 默认 | HTTP 请求处理 |

任务间通信全部通过 FreeRTOS 队列（`xQueueSend`/`xQueueReceive`）和信号量（`xSemaphoreTake`/`xSemaphoreGive`）实现，无锁设计。

---

## 3. 与 PC Claude Code Agent 的差异

### 3.1 架构对比总表

| 维度 | ESP-Claw (嵌入式) | PC Claude Code |
|---|---|---|
| **运行环境** | ESP32 单片机，FreeRTOS | Linux/macOS/Windows，完整 OS |
| **RAM** | 片上 SRAM 520KB + PSRAM 8-16MB | 8-64 GB |
| **存储** | Flash 8-32MB（FATFS + NVS） | SSD/HDD TB 级 |
| **推理** | **100% 云端**（HTTP 调用远程 LLM API） | 可本地（Ollama/llama.cpp），也可云端 |
| **编程语言** | C + Lua 解释器 | TypeScript/Python/Rust 等高级语言 |
| **并发模型** | FreeRTOS 多任务（时间片轮转，显式栈大小） | 多进程/多线程/协程，OS 管理 |
| **文件系统** | FATFS（嵌入式 FAT）+ NVS（键值对 Flash） | ext4/NTFS/APFS，全功能 POSIX |
| **网络** | Wi-Fi STA+AP 双模，自管理状态机 | OS 网络栈，通常即插即用 |
| **用户界面** | LCD 屏幕 + Web 管理界面 + IM 平台 | 终端 CLI（TUI/REPL） |

---

### 3.2 ReAct 循环对比

**ESP-Claw：**
```
FreeRTOS 队列阻塞等待 ──→ 5 个 Context Provider 收集上下文 ──→ LLM HTTP 调用
──→ 如果 tool_call：本地 Capability 执行 ──→ 结果追加 ──→ 循环（最多 32 轮）
──→ 如果纯文本：Event Router 路由回复到 IM ──→ 完成
```

**PC Claude Code：**
```
用户输入/系统触发 ──→ 上下文组装（文件 reads/Greps/对话历史）
──→ LLM API 调用（可本地可云端） ──→ 工具执行（Bash/Read/Write/Edit/Agent...）
──→ 结果追加 ──→ 循环（无硬限制，但有 token 预算）
──→ 最终回复呈现在终端
```

**关键差异：**

1. **请求来源不同**：ESP-Claw 的请求来自 Event Router 的事件分发系统（IM 消息/定时器/传感器），是多源异步的。PC Claude Code 的请求来自终端用户的交互式输入或 IDE 上下文。

2. **轮次硬限制**：ESP-Claw 有 `max_tool_iterations = 32` 的硬保护，防止嵌入式设备无限循环耗尽内存。PC Claude Code 主要通过 token 预算控制。

3. **流式中断机制**：ESP-Claw 有 `inflight_abort` 的 volatile 标志 + `claw_llm_http_arm_abort` 回调机制，在 HTTP 传输层可以中断正在接收的流式响应。这是嵌入式场景下关键的资源保护。

4. **上下文提供者模型**：ESP-Claw 用 5 个固定的 Context Provider 回调注入上下文，每次 LLM 调用重新收集。PC Claude Code 的上下文管理更灵活（文件系统直接读取、RAG 检索、系统提示动态生成）。

---

### 3.3 工具/能力系统对比

**ESP-Claw 的能力系统（19 个 capability 组件）：**

```
capabilities/
├── 物理世界交互: cap_lua (→ Lua → GPIO/I2C/舵机), cap_servo
├── 硬件媒体: cap_voice (TTS), cap_asr (语音识别)
├── 网络: cap_web_search, cap_mcp_client, cap_mcp_server
├── 通信: cap_im_qq/feishu/tg/wechat/local (5 个 IM 平台)
├── 系统: cap_system, cap_files, cap_time, cap_scheduler
└── 管理: cap_skill_mgr, cap_router_mgr, cap_session_mgr
```

**PC Claude Code 的工具系统：**

```
Bash, Read, Write, Edit, Grep, Glob, WebFetch, WebSearch,
NotebookEdit, Task, Skill, Agent, AskUserQuestion...
```

**关键差异：**

1. **物理世界交互**：这是最本质的差异。ESP-Claw 的 capability 能直接控制硬件——舵机转动、播放音频、点亮 LED、读取传感器。PC Claude Code 只能通过软件 API 间接影响物理世界。

2. **Lua 作为硬件桥梁**：ESP-Claw 的核心硬件交互通过 Lua 脚本完成。LLM 写 Lua 代码 → 31 个预注册的 C 模块提供硬件 API → 直接操控 GPIO/I2C/舵机/摄像头/音频。这是一种 "LLM 生成脚本驱动硬件" 的独特架构模式。PC Claude Code 直接执行 Shell 命令或 Python，不需要这种中间层。

3. **两级可见性控制**：ESP-Claw 的 `enabled_cap_groups`（编译时启用）和 `llm_visible_cap_groups`（LLM 是否可见）两级控制。Skills 的激活/反激活可以动态切换后者。PC Claude Code 没有这种细粒度的可见性管理——工具要么可用要么不可用。

4. **JSON Schema 驱动的工具描述**：每个 capability 注册时提供 JSON Schema 描述参数，`claw_cap_build_llm_tools_json()` 自动生成符合 Anthropic/OpenAI tool-use 格式的工具描述。这与 PC Claude Code 的工具定义思路一致，但实现更紧凑（静态 C 字符串而非动态对象）。

---

### 3.4 记忆系统对比

| 特性 | ESP-Claw | PC Claude Code |
|---|---|---|
| **存储介质** | Flash FATFS（8-32MB），NVS 键值对 | 硬盘文件（~/.claude/ 目录） |
| **会话存储** | JSONL 记录，固定槽位（max 20 rounds） | 完整对话历史文件 |
| **长期记忆** | FULL: JSONL + 索引 + 小模型自动提取；LIGHT: 单个 MEMORY.md | 无内置，需用户手动 /remember |
| **记忆检索** | 基于摘要标签的精确匹配 | 文件系统直接读取 |
| **内存限制** | 每条消息最大 4096 字符，每条记忆内容 256 字符 | 无硬限制 |

---

### 3.5 Event Router — PC Agent 完全没有的概念

这是 ESP-Claw 最独特的设计。Event Router 让设备成为**事件驱动的自动化中枢**，而不仅是一个对话机器人。

```
                ┌──────────────────────────────┐
                │       Event Router            │
                │  (规则引擎，JSON 可配置)       │
                │                              │
IM消息 ──────→  │  匹配规则 → 6种动作:           │
定时器 ──────→  │  CALL_CAP / RUN_AGENT /       │──→ LLM Agent
传感器 ──────→  │  RUN_SCRIPT / EMIT_EVENT /    │──→ Capability
启动事件 ────→  │  SEND_MESSAGE / DROP          │──→ IM 回复
                │                              │──→ 级联事件
                └──────────────────────────────┘
```

规则示例（`router_rules.json`）：
```json
{
  "id": "route_im_text_to_agent",
  "match": { "event_type": "in_message", "content_type": "text" },
  "actions": [{ "type": "RUN_AGENT" }]
}
```

PC Claude Code 没有这种事件总线设计——它是单通道的 CLI 交互模型。如果要在 PC 上实现类似功能，需要用户自己编写事件监听和路由逻辑。

---

### 3.6 启动流程对比

**ESP-Claw：** 14 阶段线性启动，每一步有明确的失败回退策略。Wi-Fi 有状态机（6 状态 + 指数退避）。如果 Wi-Fi 连接失败，设备回退到 AP 模式提供配网门户。如果 LLM 未配置，设备正常启动但禁用 Agent 功能。

**PC Claude Code：** 用户运行 `claude` 命令 → 初始化配置 → 进入交互 REPL。网络和文件系统由 OS 保证，不需要设备自管理。

---

### 3.7 核心结论

ESP-Claw 本质上是把 PC Agent 的所有核心概念（ReAct 循环、工具调用、记忆、多模态、多通道通信）都压缩到了 ESP32 上，但：

1. **推理 100% 依赖云端** — ESP32 算力完全不足以运行任何规模的 LLM
2. **Lua 是硬件桥梁** — LLM 不能直接操作 GPIO/I2C/舵机，必须通过 Lua 脚本间接控制
3. **物理世界交互是核心差异** — PC Agent 只能影响软件世界
4. **资源保护是第一优先级** — 栈大小、迭代次数、队列长度、消息大小都有硬上限
5. **Event Router 让设备成为事件自动化中枢** — 不只是对话机器人，还能按规则自动执行动作
6. **多通道 IM 是核心入口** — 面向终端用户通过微信/QQ/Telegram/Web 交互，而不是面向开发者的 CLI

---

## 4. 资源受限环境下的运行原理

这是 ESP-Claw 最精妙的部分。一个 ESP32 只有 520KB SRAM + 8-16MB PSRAM，怎么运行一个完整的 AI Agent？答案是通过**系统性的资源保护策略和架构设计**。

### 4.1 核心原则：推理外置，执行本地

```
┌─────────────────────────────────────────────────────┐
│                    ESP32 设备                        │
│                                                     │
│  ┌─────────────┐    HTTP/HTTPS     ┌──────────────┐ │
│  │ claw_core   │ ←───────────────→ │ 云端 LLM API │ │
│  │ (轻量调度)   │   推理请求/响应    │ (全部推理)    │ │
│  └──────┬──────┘                   └──────────────┘ │
│         │ 工具调用                                   │
│  ┌──────▼──────┐                                    │
│  │ Capability  │ ← 本地执行（GPIO、文件、舵机等）     │
│  └─────────────┘                                    │
└─────────────────────────────────────────────────────┘
```

ESP32 不做任何推理计算。它只做三件事：
1. 组装上下文（拼接文本字符串）
2. HTTP 调用远程 LLM API（纯网络 I/O）
3. 解析 LLM 返回的 tool_call，执行本地能力

这三件事对 CPU 和内存的需求都极低——本质上是一个 "智能消息代理"。

---

### 4.2 内存管理策略

#### 4.2.1 静态预分配 + 显式上限

几乎所有的动态数据结构都有硬上限：

| 资源 | 上限值 | 定义位置 |
|---|---|---|
| claw_core 任务栈 | **16KB** | `app_claw.c:286` |
| event_router 任务栈 | **8KB** | `app_claw.c:195` |
| scheduler 任务栈 | **6KB** | `app_claw.c:227` |
| 请求队列长度 | **4** | `app_claw.c:290` |
| 响应队列长度 | **4** | `app_claw.c:291` |
| 上下文提供者数量 | **8** | `app_claw.c:292` |
| 工具迭代上限 | **32** | `app_claw.c:289` |
| 会话消息上限 | **20** | `app_claw.c:83-84` |
| 单条消息最大长度 | **4096 字符** | `app_claw.c:84` |
| 工具总结最大长度 | **768 字符** | `claw_core.c:42` |
| 日志截断长度 | **96 字符** | `claw_core.c:40` |
| 每条记忆内容 | **256 字符** | `claw_memory.h:45` |
| 记忆总结 ID 数 | **3** | `claw_memory.h:44` |
| 完成观察者 | **4** | `claw_core.c:94` |
| 调度项最大数 | **32** | `app_claw.c:227` |
| FATFS 最大文件句柄 | **8** | `main.c:230` |
| LWIP 最大 socket | **20** | `sdkconfig.defaults` |

#### 4.2.2 栈策略：Internal vs PSRAM

ESP32 有两种内存区域：

| 内存类型 | 大小 | 速度 | 用途 |
|---|---|---|---|
| **Internal SRAM** | 520KB | 快（~100ns） | 任务栈、DMA buffer、中断上下文 |
| **External PSRAM** | 8-16MB | 慢（~50ns） | 大块数据（LVGL 帧缓冲、HTTP buffer） |

`claw_task_create` 提供了 `stack_policy` 参数：
- `CLAW_TASK_STACK_INTERNAL_ONLY` — 栈必须放在内部 SRAM（claw_core 使用，因为它在 HTTP 流式解析时需要快速上下文切换）
- `CLAW_TASK_STACK_PSRAM_ONLY` — 栈可放在 PSRAM（不频繁切换的辅助任务使用）

#### 4.2.3 内存监控

启用 `APP_ENABLE_MEM_LOG` 后，每 20 秒打印：

```
Memory: internal_free=123456 bytes, internal_min_free=98765 bytes, psram_free=4567890 bytes
Task claw_core  high_water=10234
Task event_router  high_water=5432
```

`high_water` 是任务栈的剩余高水位——开发者用这些数据来确定每个任务实际需要多少栈空间，避免浪费珍贵的 SRAM。

---

### 4.3 存储策略：Flash 友好设计

#### 4.3.1 NVS（Non-Volatile Storage）

Flash 的一个小分区（通常 16-64KB），键值对存储。用于：
- Wi-Fi SSID/密码
- LLM API Key/Model/Base URL
- IM 平台 Token
- 启用的能力组/Lua 模块白名单
- 时区设置

**为什么用 NVS 而不是 FATFS**: NVS 支持原子写入、磨损均衡、掉电安全。ESP-IDF 的 NVS 库在底层做了这些保证，适合存储需要频繁更新的配置数据。

#### 4.3.2 FATFS + Wear Leveling

Flash 的 `storage` 分区（通常 2-28MB，取决于 Flash 大小），通过 ESP-IDF 的 **wear-leveling（磨损均衡）** 层挂载为 FAT 文件系统。

**磨损均衡** 是核心：Flash 的每个扇区只能擦除/写入有限次（通常 10 万次）。Wear-leveling 层将逻辑扇区映射到物理扇区，并均匀分布擦除操作，使 Flash 寿命最大化。

FATFS 上存储的内容：
- `/fatfs/sessions/` — 会话历史（频繁写入，小文件）
- `/fatfs/memory/` — 长期记忆
- `/fatfs/skills/` — 技能文件
- `/fatfs/scripts/` — Lua 脚本
- `/fatfs/router_rules/` — 路由规则

#### 4.3.3 两种记忆模式的资源分级

这是一个典型的嵌入式资源分级策略：

**FULL 模式（大 Flash 设备）：**
- JSONL 记录 + 索引文件 + 异步提取
- 消耗更多 Flash 写入和 CPU 资源

**LIGHTWEIGHT 模式（小 Flash 设备）：**
- 仅维护单个 `MEMORY.md` 文件
- 最小化 Flash 写入和内存占用

---

### 4.4 网络策略：Wi-Fi 自管理

PC 上的 Agent 假设网络永远可用，由 OS 管理。ESP32 上的 Agent 必须自己管理 Wi-Fi。

#### 4.4.1 Wi-Fi 状态机

```
                         ┌─────────────────┐
         启动 ─────────→ │ PROVISION_AP     │ (纯 AP，等待配网)
                         └────────┬────────┘
                                  │ 用户配置了 STA 凭据
                         ┌────────▼────────┐
                         │ APSTA_TRYING    │ (AP+STA 同时运行，尝试连网)
                         └───┬────────┬────┘
                    STA 成功  │        │ STA 失败
                ┌────────────▼┐  ┌────▼──────────┐
                │ APSTA_OK    │  │ AP_FALLBACK    │ (回退纯 AP)
                └──────┬──────┘  └────────────────┘
                       │ STA 稳定
                ┌──────▼──────┐
                │ STA_ONLY    │ (关闭 AP，纯 STA)
                └─────────────┘
```

#### 4.4.2 指数退避重连

当 STA 断开时，重连间隔从 1 秒开始，每次失败加倍（1s → 2s → 4s → 8s → 16s → 30s 封顶），在 30 秒上限后保持。这样避免了在弱信号环境下频繁重连导致的功耗和 CPU 浪费。

#### 4.4.3 Captive DNS 配网

首次使用的设备启动在 AP 模式，用户手机连接热点后，任何 HTTP 请求的 DNS 查询都会被设备劫持，重定向到配置页面。用户无需知道设备 IP，也不需要下载 App。这是物联网设备的经典配网模式。

---

### 4.5 HTTP 连接复用

TLS 握手在 ESP32 上是昂贵的操作（RSA/ECDHE 密钥交换需要数秒 CPU 时间）。ESP-Claw 通过 LRU 连接池复用 Keep-Alive 连接：

```
LLM API 调用 ────┐
Web 搜索 ────────┼──→ HTTP 连接池（LRU，按 host:port 索引）──→ 远程服务器
MCP 后端 ────────┘
```

同一个目标主机的多次请求复用同一 TLS 连接，避免重复握手。连接池大小有限（受 LWIP socket 上限 20 的约束）。

---

### 4.6 代码大小控制

ESP32 的 Flash 不仅是存储，更是代码的执行位置（XIP — eXecute In Place）。ESP-Claw 通过以下方式控制固件体积：

1. **Kconfig 条件编译** — 每个能力、每个 Lua 模块都有独立的 `CONFIG_*` 开关，未启用的模块不编译
2. **按需链接** — 只链接被引用的符号
3. **协议栈优化** — LWIP、mbedTLS、WebSocket 库都有精细的配置项（如最大 socket 数、TCP buffer 大小、TLS 加密套件选择）

典型配置下，ESP-Claw 固件约 3-6MB，Flash 占用约 40-60%（在 16MB Flash 设备上）。

---

### 4.7 任务优先级与 CPU 时间分配

所有核心任务运行在优先级 5，FreeRTOS 使用时间片轮转（每个 tick = 1ms）。这确保了：

- 没有单个任务能饿死其他任务
- Wi-Fi 协议栈和 LWIP 在更高优先级运行，保证网络 I/O 不受影响
- 空闲任务在优先级 0，CPU 空闲时自动进入 light sleep 省电

任务间通信通过无锁队列（`xQueueSend`/`xQueueReceive`）和信号量，避免优先级反转和死锁。

---

### 4.8 总结：为什么能在 ESP32 上运行

| 挑战 | 解决方案 |
|---|---|
| **算力不足** | 推理 100% 外置云端，ESP32 只做轻量文本拼接和 HTTP I/O |
| **SRAM 极小（520KB）** | 显式栈大小、固定上限、PSRAM 做大数据缓冲、Internal SRAM 留给关键任务 |
| **Flash 有限（8-32MB）** | Kconfig 条件编译、磨损均衡、LIGHTWEIGHT 记忆模式、固件 XIP 执行 |
| **Flash 写入寿命** | Wear-leveling + NVS 原子写入 |
| **网络不稳定** | Wi-Fi 状态机（6 状态）+ 指数退避重连 + AP fallback + Captive DNS 配网 |
| **TLS 开销大** | HTTP Keep-Alive 连接池（LRU 复用） |
| **FreeRTOS 并发限制** | 无锁队列通信、时间片轮转、优先级分离（网络栈更高优先级） |
| **不能直接操作硬件** | Lua 桥接层 — LLM 写脚本，31 个 C 模块提供硬件 API |
| **多子系统访问 LCD** | Display Arbiter 非抢占式仲裁 |
| **固件更新** | OTA 双槽位 + 启动失败自动回滚 |

**一句话总结：ESP-Claw 不是把 LLM 塞进了 ESP32，而是把 ESP32 变成了一个智能的硬件代理——它负责收集上下文、转发给云端的 LLM 推理、然后执行 LLM 返回的指令来操控物理世界。所有的"重活"都在云端，ESP32 只做它最擅长的事：连接网络、驱动硬件、管理外设。**

---

## 附录 A：关键文件索引

| 文件 | 职责 |
|---|---|
| `application/edge_agent/main/main.c` | `app_main()` — 14 阶段启动 |
| `components/common/app_claw/app_claw.c` | Agent 组装、系统提示词、15 步启动 |
| `components/claw_modules/claw_core/src/claw_core.c` | ReAct 循环：队列→上下文→LLM→工具→循环 |
| `components/claw_modules/claw_core/src/llm/claw_llm_http_transport.c` | HTTP 传输、SSE 解析、UTF-8 清理 |
| `components/claw_modules/claw_core/src/llm/backends/claw_llm_backend_anthropic.c` | Anthropic API 适配（thinking 块处理） |
| `components/claw_modules/claw_core/src/llm/backends/claw_llm_backend_openai_compatible.c` | OpenAI 兼容 API 适配 |
| `components/claw_modules/claw_event_router/src/claw_event_router.c` | 事件总线、规则匹配、6 种动作 |
| `components/claw_modules/claw_memory/src/claw_memory.c` | 记忆系统入口（FULL/LIGHTWEIGHT 双模式） |
| `components/claw_modules/claw_cap/src/claw_cap.c` | 能力插件框架 |
| `components/claw_modules/claw_skill/src/claw_skill.c` | 技能系统（SKILL.md 解析与激活） |
| `components/common/wifi_manager/wifi_manager.c` | Wi-Fi 6 状态机 + 指数退避 |
| `components/common/http_reuse/esp_http_client_reuse.c` | HTTP LRU 连接池 |
| `application/edge_agent/components/app_config/include/app_config.h` | 完整配置结构体（30+ 字段） |

## 附录 B：编译与烧录速查

```bash
# 环境准备
. $IDF_PATH/export.sh

# 构建
cd application/edge_agent
idf.py set-target esp32s3
idf.py -DCONFIG_ESP_BOARD_M5STACK_CORES3=1 build

# 烧录
idf.py -p /dev/ttyUSB0 flash

# 串口监控
idf.py monitor

# 清理重建
idf.py fullclean build
```
