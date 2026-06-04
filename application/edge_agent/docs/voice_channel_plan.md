# 语音通道 (Voice Channel) 实施计划

> 目标：让语音 ASR 成为与 QQ/微信/飞书/Telegram/WebIM 对等的第一类 IM 通道，按键触发语音输入后，识别文本作为 `message` 事件进入 Event Router → LLM 推理 → 回复自动通过 TTS 朗读。

---

## 1. 现状分析

### 1.1 已实现的部分

**[app_asr_button.c](../../components/common/app_claw/app_asr_button.c)** — 按键触发 ASR 的基本链路已经通了：

```
物理按键 → video_player_set_button_callback → asr_handler_task
  → claw_cap_call("start_asr", ...) 同步阻塞（最多 45s）
  → 拿到识别文本后，发布 claw_event_t 到 Event Router
```

### 1.2 当前实现存在的 3 个问题

| # | 问题 | 详情 |
|---|------|------|
| **P1** | 事件类型不对 | 发布的是 `trigger` 事件（session_policy=TRIGGER），语音输入被认为是"一次性独立任务"，不是"对话消息"。不符合 IM 通道的消息语义 |
| **P2** | 缺少出站绑定 | 没有注册 `"voice" → "get_tts"`，LLM 回复无法自动以语音输出 |
| **P3** | 事件字段不完整 | `channel` 未填，`message_id` 硬编码为 `"asr_voice_input"`（多次输入会 ID 冲突），没有使用标准的 `claw_event_router_publish_message()` API |

### 1.3 各 IM 通道对比（目标对齐）

| | QQ | Telegram | WebIM | Voice（当前） | Voice（目标） |
|---|---|---|---|---|---|
| **入站方式** | WebSocket | HTTP 长轮询 | WebSocket | 按键 + 同步 ASR | 按键 + 同步 ASR |
| **事件 API** | `publish_message` | `publish_message` | `publish_message` | `publish` (bare event) | `publish_message` |
| **source_cap** | `qq_gateway` | `tg_gateway` | `local_gateway` | `cap_asr` | `voice_gateway` |
| **channel** | `qq` | `telegram` | `web` | (空) | `voice` |
| **session 策略** | CHAT | CHAT | CHAT | TRIGGER | CHAT |
| **出站绑定** | `qq_send_message` | `tg_send_message` | `local_send_message` | 无 | `get_tts` |
| **消息 ID** | 自动生成 | 自动生成 | 自动生成 | 硬编码固定值 | 时间戳生成 |

---

## 2. 目标架构

```
                     ┌─────────────────────────┐
硬件按键按下          │   app_asr_button.c      │
                     │   (asr_handler_task)     │
                     │                         │
                     │ 1. 设置按钮状态=监听中    │
                     │ 2. claw_cap_call(        │
                     │      "start_asr", ...)  │
                     │    阻塞等待 ASR 结果      │
                     │ 3. 拿到文本              │
                     │ 4. publish_message(      │  ← 核心改动：用 publish_message
                     │      "voice_gateway",    │    替代裸 claw_event_t
                     │      "voice",            │
                     │      chat_id, text,      │
                     │      sender_id, msg_id)  │
                     │ 5. 按钮状态还原          │
                     └───────────┬─────────────┘
                                 │
                                 ▼
                     ┌─────────────────────────┐
                     │   Event Router           │
                     │   匹配规则 → RUN_AGENT    │
                     └───────────┬─────────────┘
                                 │
                                 ▼
                     ┌─────────────────────────┐
                     │   claw_core              │
                     │   LLM 推理                │
                     │   生成回复文本             │
                     └───────────┬─────────────┘
                                 │
                     ┌───────────▼─────────────┐
                     │   Event Router           │
                     │   出站绑定查找:           │
                     │   "voice" → "get_tts"    │  ← 核心改动：注册出站绑定
                     └───────────┬─────────────┘
                                 │
                                 ▼
                     ┌─────────────────────────┐
                     │   cap_voice              │
                     │   get_tts(text=回复)      │
                     │   → 扬声器朗读            │
                     └─────────────────────────┘
```

### 关键设计决策

**决策 1：button→ASR→publish 链路放哪里？**

| 方案 | 位置 | 优劣 |
|------|------|------|
| A | 保持在 `app_asr_button.c` | 应用层代码，改动最小，职责清晰（按键→触发通道） |
| B | 移到 `cap_asr.c` 新增 voice_gateway Event Source | 架构更"干净"，但需要在 cap_asr 内耦合按键逻辑 |

**选择 A**。理由：
- `app_asr_button.c` 已存在且工作正常，它是"按键→ASR→事件发布"的胶水层，这正是应用层该做的事
- IM 通道的 Event Source（如 `tg_gateway`）需要自己管理网络连接和轮询任务，而语音的"网络连接"是 WebSocket，"轮询"是按键中断——这些都在应用层
- 不需要为了架构的对称性而过度抽象

**决策 2：session 策略用 CHAT 还是 TRIGGER？**

| 策略 | 行为 |
|------|------|
| CHAT | 每次语音输入延续同一会话，LLM 记住之前说了什么 |
| TRIGGER | 每次语音输入是独立的一次性任务，不保留上下文 |

**选择 CHAT**。语音对话理应有上下文连续性（"今天天气怎么样" → "那明天呢"），这与微信/QQ 的体验一致。

**决策 3：chat_id 如何定义？**

语音对话没有"群聊/私聊"的概念，使用固定 ID：`"voice_default"`。后续如需支持多用户声纹识别，可扩展为 `"voice_user_xxx"`。

---

## 3. 详细改动清单

### 3.1 修改 `app_asr_button.c` — 改用 publish_message API

**文件**：[components/common/app_claw/app_asr_button.c](../../components/common/app_claw/app_asr_button.c)

**改动内容**（`asr_handler_task` 函数，约第 29-70 行）：

```c
// ===== 改动前（第 45-60 行）=====
// 构造裸 claw_event_t，字段不完整，session_policy=TRIGGER
claw_event_t event = {0};
strlcpy(event.source_cap, "cap_asr", sizeof(event.source_cap));
strlcpy(event.event_type, "trigger", sizeof(event.event_type));
// ... 缺少 channel、chat_id、sender_id ...
event.session_policy = CLAW_EVENT_SESSION_POLICY_TRIGGER;
claw_event_router_publish(&event);

// ===== 改动后 ======
// 使用标准 publish_message API，与所有 IM 通道一致
static int64_t s_voice_msg_seq = 0;  // 文件顶部新增
char message_id[48];
snprintf(message_id, sizeof(message_id), "voice-%lld", ++s_voice_msg_seq);

claw_event_router_publish_message(
    "voice_gateway",           // source_cap
    "voice",                   // channel
    "voice_default",           // chat_id
    output,                    // text（ASR 识别结果）
    "voice_user",              // sender_id
    message_id                 // message_id
);
// publish_message 内部自动设置 session_policy=CHAT
// 内部会复制 text，无需担心 output 栈内存生命周期
```

**改动要点**：
1. 删除手动构造 `claw_event_t` 的代码块
2. 调用 `claw_event_router_publish_message()` — 与 Telegram/QQ/WeChat 的入站路径完全一致
3. 添加全局计数器 `s_voice_msg_seq` 生成唯一 message_id
4. 移除 `event.text = output` 的栈指针悬挂风险（`publish_message` 内部复制）

### 3.2 修改 `app_claw.c` — 注册 voice 出站绑定

**文件**：[components/common/app_claw/app_claw.c](../../components/common/app_claw/app_claw.c)

**改动位置**：第 248 行之后，`web` 出站绑定注册之后

```c
// ===== 新增（约第 251 行后）=====
#if CONFIG_APP_CLAW_CAP_ASR
    ESP_RETURN_ON_ERROR(
        claw_event_router_register_outbound_binding("voice", "get_tts"),
        TAG, "Failed to bind Voice outbound");
#endif
```

**说明**：
- 使用条件编译 `CONFIG_APP_CLAW_CAP_ASR`，仅在启用 ASR 能力时注册
- 语义：当 Agent 回复时 `target_channel = "voice"`，Event Router 自动调用 `get_tts` 朗读回复文本
- 不需要新增 Kconfig 选项，复用已有的 `APP_CLAW_CAP_ASR`

### 3.3 可选：路由规则配置

当前 Event Router 配置了 `default_route_messages_to_agent = true`（LLM 启用时），且 `voice_gateway` 发布的是标准 `message` 事件，**无需额外路由规则即可自动进入 Agent**。

如果后续需要对语音消息做特殊处理（如：先播放"正在思考"的音效），可以在 `/fatfs/router_rules/router_rules.json` 中添加：

```json
{
  "id": "rule_voice_message",
  "enabled": true,
  "description": "Route voice messages to agent with TTS reply",
  "match": {
    "source_channel": "voice"
  },
  "actions": [
    {
      "kind": "RUN_AGENT",
      "session_policy": "CHAT"
    }
  ]
}
```

**Phase 1 不需要此步骤。**

### 3.4 可选：处理 TTS 回复中的递归问题

LLM 的回复通过 `get_tts` 朗读时，`get_tts` 本身会返回一个结果文本（如 `"Done: tts complete"`）。这个结果文本会作为能力调用结果返回给 Event Router/LLM，但 **不会** 再次触发 TTS（因为它是能力返回值，不是出站消息）。

**无需额外处理。**

---

## 4. 完整数据流演练

```
[1] 用户按下硬件按键
     │
     ▼
[2] video_player IRQ → asr_button_pressed() → xQueueSend(s_asr_queue)
     │
     ▼
[3] asr_handler_task 被唤醒
     ├─ video_player_set_button_state(1)   // 按钮 LED 变为"监听中"
     ├─ claw_cap_call("start_asr", "{}")   // 阻塞等待，最多 ~45s
     │   ├─ ensure_adc_dev()              // 打开 ADC 麦克风
     │   ├─ xTaskCreate(asr_capture_task) // 启动音频采集
     │   ├─ WebSocket 发送 asr_start
     │   ├─ 持续发送 20ms PCM chunks
     │   ├─ 静音检测 / 超时检测
     │   ├─ WebSocket 发送 asr_stop
     │   ├─ 等待 asr_result (final=true)
     │   └─ 返回识别文本 output = "今天天气怎么样"
     │
[4] 判断返回结果
     ├─ 成功 (ret==ESP_OK, 不以"Error:"开头):
     │   ├─ video_player_set_button_state(2)   // 按钮 LED "成功"
     │   └─ claw_event_router_publish_message(
     │         "voice_gateway", "voice", "voice_default",
     │         "今天天气怎么样", "voice_user", "voice-1")
     │        → 内部构造 claw_event_t:
     │          event_type = "message"
     │          source_channel = "voice"
     │          session_policy = CHAT
     │        → 入队 Event Router
     │
     ├─ 失败:
     │   ├─ video_player_set_button_state(3)   // 按钮 LED "错误"
     │   └─ (不发布事件)
     │
[5] Event Router 处理 "voice-1" 事件
     ├─ 匹配规则 (default_route_messages_to_agent=true)
     ├─ 动作: RUN_AGENT
     └─ → claw_core_submit(request {
            source_channel = "voice",
            chat_id = "voice_default",
            user_text = "今天天气怎么样"
          })
     │
     ▼
[6] claw_core LLM 推理
     ├─ 收集上下文 (记忆/技能/工具)
     ├─ 调用 LLM API
     └─ 生成回复: "北京今天晴天，气温25度"
     │
     ▼
[7] claw_core 通过 Event Router 发布回复
     ├─ target_channel = "voice"  (继承自 source_channel)
     ├─ Event Router 查找出站绑定: "voice" → "get_tts"
     └─ → claw_cap_call("get_tts", {"text": "北京今天晴天，气温25度"})
     │
     ▼
[8] cap_voice TTS 处理
     ├─ WebSocket 发送 tts_request
     ├─ 接收 Opus 音频流
     ├─ 解码 → 48kHz 立体声 PCM
     └─ → 扬声器播放 "北京今天晴天，气温25度"
     │
     ▼
[9] 回到空闲状态
     ├─ video_player_set_button_state(0)   // 按钮 LED 恢复
     └─ 等待下一次按键
```

---

## 5. 需要修改的文件汇总

| 文件 | 改动类型 | 改动量 | 说明 |
|------|----------|--------|------|
| [app_asr_button.c](../../components/common/app_claw/app_asr_button.c) | **重写事件发布逻辑** | ~15 行 | `claw_event_t` 手动构造 → `claw_event_router_publish_message()` |
| [app_claw.c](../../components/common/app_claw/app_claw.c) | **新增 5 行** | ~5 行 | 注册 `"voice" → "get_tts"` 出站绑定 |

**总计：2 个文件，~20 行有效改动。**

---

## 6. 测试计划

### 6.1 基本功能测试

| 用例 | 步骤 | 期望结果 |
|------|------|----------|
| 语音→LLM→TTS 全链路 | 按键→说话→松手 | 扬声器朗读 LLM 回复 |
| 短语音 | 说 1-2 个字 | 正确识别并回复 |
| 长语音 | 说 20 秒以上 | 静音检测自动停止，正确识别 |
| 静音场景 | 按键但不说话 | ASR 返回 "Error: no result"，不发布事件，按钮状态恢复 |
| 连续对话 | 按键→问"A"→收到回复→再按键→问"那B呢" | LLM 记住 A 的上下文，B 的回复正确关联 |

### 6.2 错误处理测试

| 用例 | 步骤 | 期望结果 |
|------|------|----------|
| 服务器断连 | 按键时 voice server 不可达 | ASR 返回 Error，按钮显示错误状态，不崩溃 |
| 快速连按 | 快速按两次键 | 第二次按键被队列缓存，等第一次 ASR 完成后执行 |
| TTS 失败 | ASR 成功但 TTS 服务器出错 | LLM 仍然完成推理，TTS 报错但不影响系统 |

### 6.3 与其他通道并存测试

| 用例 | 步骤 | 期望结果 |
|------|------|----------|
| WebIM + Voice 交替 | WebIM 发消息→按键语音→WebIM 再发 | 两个通道各自维护独立 session，不互相干扰 |
| 同时多通道触发 | WebIM 正在推理时按键 | Voice 请求排队，等 WebIM 推理完成后处理 |

---

## 7. 后续演进方向（Phase 2 / Phase 3）

| 阶段 | 功能 | 依赖 |
|------|------|------|
| **Phase 2** | VAD 自动触发（无需按键，检测到语音自动开始录音） | 将 `asr_capture_task` 改造为常驻后台，使用现有静音检测逻辑 |
| **Phase 2** | 语音打断（按键可中断正在进行的 TTS 播放） | 在 `cap_voice` 中添加 `stop_tts` 能力 |
| **Phase 3** | 唤醒词（"你好小艾" 触发交互模式） | 需要轻量级唤醒词模型 |
| **Phase 3** | 多用户声纹识别（不同用户不同 chat_id） | 需要声纹模型或云端识别 |

---

## 8. 一句话总结

**只改 2 个文件 ~20 行代码**，把 `app_asr_button.c` 中的裸 `claw_event_t` 构造替换为标准 `claw_event_router_publish_message("voice_gateway", "voice", ...)` 调用，再在 `app_claw.c` 注册一行 `"voice" → "get_tts"` 出站绑定，语音就成为与微信/QQ/飞书完全对等的第一类 IM 通道。
