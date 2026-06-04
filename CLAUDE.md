# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Flash

```bash
# One-time ESP-IDF environment setup
. $IDF_PATH/export.sh  # or: . ~/esp/esp-idf/export.sh

# Build (from application/edge_agent/)
cd application/edge_agent
idf.py build

# Build for a specific board (set via Kconfig or -D)
idf.py -DCONFIG_ESP_BOARD_M5STACK_CORES3=1 build

# Flash to device (USB serial)
idf.py flash

# Serial monitor
idf.py monitor

# Clean rebuild
idf.py fullclean build
```

- `idf.py menuconfig` — interactive Kconfig for board selection, feature toggles, LLM/Wi-Fi defaults
- CI pre-check: `copyright` headers, `astyle` formatting, commit message format
- Partition table auto-selected by [flash_partition_defaults.cmake](application/edge_agent/tools/cmake/flash_partition_defaults.cmake) based on Flash size (8/16/32 MB)

## Architecture

ESP-Claw is an AI Agent runtime for ESP32-series chips (ESP-IDF v5.5.4, FreeRTOS). It completes the sense→decide→act loop locally on-device, using LLM APIs for reasoning.

**8-layer architecture (bottom-up):**

1. **ESP32 hardware + peripherals** (LCD, touch, audio codec, camera, SD card, LEDs)
2. **Hardware abstraction** — Board Manager (`espressif/esp_board_manager` v0.5.11) via YAML-defined boards; Display Arbiter for LCD contention
3. **Lua modules** (31 modules in `components/lua_modules/`) — GPIO, I2C, ADC, camera, audio, display (LVGL v9.4.0), IMU, LED strip, IR, environmental sensors, etc.
4. **Capability layer** (19 components in `components/claw_capabilities/`) — Lua exec, MCP client/server, files, voice (TTS via Opus→PCM), web search (Brave/Tavily), IM platforms (Telegram/QQ/Feishu/WeChat), scheduler (cron), emotion animations, CLI
5. **Core Agent engine** (5 modules in `components/claw_modules/`):
   - `claw_core` — ReAct loop: request queue → context providers → LLM API call → tool execution → repeat (max 32 rounds). Supports OpenAI/Anthropic/Custom backends, streaming with inflight abort.
   - `claw_event_router` — Event bus: 6 action types (CALL_CAP, RUN_AGENT, RUN_SCRIPT, EMIT_EVENT, SEND_MESSAGE, DROP), 5 session strategies, outbound channel bindings
   - `claw_cap` — Plugin descriptor system: cap groups register descriptors with JSON Schema inputs, LLM visibility gated per-group
   - `claw_memory` — Two modes: FULL (structured JSONL records + index + async auto-extract) or LIGHTWEIGHT (single MEMORY.md)
   - `claw_skill` — User-facing Markdown skills (SKILL.md with YAML frontmatter) that dynamically unlock cap group visibility for LLM
6. **Application layer** (`application/edge_agent/`) — `main.c` 14-phase boot sequence, HTTP REST API, emotion video player
7. **HTTP server** — REST API for config/Wi-Fi/files/capabilities + WebSocket for WebIM + Captive DNS portal for Wi-Fi provisioning
8. **IM communication** — Multi-platform chat (Telegram, QQ, Feishu, WeChat, WebIM)

**Boot sequence** (`app_main()` in [main.c](application/edge_agent/main/main.c)): alloc runtime → NVS → load config → timezone → board init → UI/emote → video player → FATFS mount → Wi-Fi manager → HTTP server → Wi-Fi connect → Agent engine (`app_claw_start()`) → WebIM bind → idle

**Key data flow:** External input (IM/timer/sensor) → Event Router (rules in `/fatfs/router_rules/router_rules.json`) → claw_core (LLM reasoning with context from 5 providers) → Capability execution (local) or LLM API (remote) → Outbound response via IM channel bindings

## Key Patterns

- **Skill vs Capability**: Skills are user-facing Markdown docs that tell the LLM *when/how* to use capabilities. Capabilities are C functions with JSON Schema that the LLM can call as tools. A skill activation unlocks cap group visibility.
- **Cap groups have two visibility layers**: `enabled_cap_groups` (compile/config-time enable) and `llm_visible_cap_groups` (whether LLM sees the tools). Skills dynamically toggle the latter.
- **Lua is the execution layer**: LLM writes Lua scripts calling 31 pre-registered C modules to interact with hardware. `run_script` (sync) vs `run_script_async` (separate FreeRTOS task, for long-running loops).
- **FATFS (`/fatfs/`) is the writable filesystem** on the `storage` flash partition, with wear-leveling. Contains sessions/, memory/, skills/, scripts/, router_rules/, scheduler/. Pre-populated from `fatfs_image/` at build time.
- **NVS namespace `app`** stores all runtime config (Wi-Fi, LLM keys, IM tokens). Priority: NVS > menuconfig defaults.
- **Wi-Fi state machine**: 6 states (PROVISION_AP → APSTA_TRYING → APSTA_OK/AP_FALLBACK → STA_ONLY), exponential backoff reconnect (1s–30s cap), Captive DNS for initial provisioning.
- **Display Arbiter** serializes LCD access from multiple subsystems (Emote engine, Video Player, Lua LVGL, ESP Painter).
- **HTTP connection reuse**: LRU pool of Keep-Alive connections shared across LLM, search, and MCP backends.
- **OTA dual-slot** on 16/32 MB Flash: ota_0 ↔ ota_1 with automatic rollback on boot failure.
- **Race condition note**: FreeRTOS tasks at equal priority 5 use time-sliced round-robin. Be careful with shared state — use queues (`xQueueSend`/`xQueueReceive`) for IPC between tasks like claw_core, event_router, and scheduler.

## Directory Layout

```
application/edge_agent/          # Main firmware application
  main/main.c                    # app_main() entry — 14-phase boot
  main/skills/                   # Built-in skills (light_switch, lua_demo, weather_search)
  components/                    # App-specific: app_config, http_server, video_player, emotion_video, cmd_wifi
  boards/                        # 17 board configs across 7 vendors (Espressif, M5Stack, DFRobot, LilyGO, Movecall, Waveshare, Nologo.Tech)
  fatfs_image/                   # Pre-seeded FATFS contents (memory, skills, scripts, router_rules, static web UI)
  tools/cmake/                   # esp_idf_patch.cmake, flash_partition_defaults.cmake
components/
  claw_modules/                  # claw_core, claw_event_router, claw_cap, claw_memory, claw_skill
  claw_capabilities/             # 19 capability plugins (cap_lua, cap_mcp_*, cap_voice, cap_files, cap_web_search, cap_im_*, cap_scheduler, ...)
  common/                        # app_claw, wifi_manager, http_reuse, display_arbiter, captive_dns, esp_painter, emote, skill_builder, lua_module_builder, ...
  lua_modules/                   # 31 Lua-callable C modules (6 hardware drivers + 18 device modules + 7 system modules)
docs/                            # Astro documentation site
Hiyori/                          # 10 emotion animation MP4 assets for virtual character "Hiyori"
```

## Notable Files

- [app_claw.c](components/common/app_claw/app_claw.c) — Agent assembly, system prompt, context provider registration, cap init order
- [app_config.h](application/edge_agent/components/app_config/include/app_config.h) — Full config struct (Wi-Fi, LLM, IM, search, caps, Lua modules, timezone)
- [claw_core.c](components/claw_modules/claw_core/src/claw_core.c) — Main ReAct loop: dequeue → collect context (5 providers) → LLM call → tool dispatch → repeat
- [claw_llm_backend_anthropic.c](components/claw_modules/claw_core/src/llm/backends/claw_llm_backend_anthropic.c) — Anthropic Messages API with thinking block preservation and merged consecutive tool results
- [claw_llm_http_transport.c](components/claw_modules/claw_core/src/llm/claw_llm_http_transport.c) — HTTP transport with UTF-8 sanitization and streaming support
- [wifi_manager.c](components/common/wifi_manager/wifi_manager.c) — 6-state Wi-Fi state machine with exponential backoff
- [esp_http_client_reuse.c](components/common/http_reuse/esp_http_client_reuse.c) — LRU HTTP connection pool
