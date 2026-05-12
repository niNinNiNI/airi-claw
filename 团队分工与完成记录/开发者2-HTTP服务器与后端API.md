# 开发者 2 — HTTP 服务器与后端 API

## 概述

开发者 2 负责 ESP-Claw Edge Agent 的**HTTP 服务器后端层**，包含嵌入式 HTTP 服务器的核心框架、RESTful API 实现、WebSocket 实时通信以及静态资源服务。该层是设备与用户浏览器之间沟通的桥梁。

---

## 完成的核心模块

### 1. HTTP 服务器核心框架 (components/http_server/http_server_core.c)

- **服务器生命周期管理**：`http_server_init()` / `http_server_start()` / `http_server_stop()`
  - 使用 ESP-IDF 原生的 `httpd` API 创建 HTTP 服务，监听端口 80
  - 配置并发连接数（max_open_sockets=12）、URI 处理句柄上限（max_uri_handlers=32）、任务栈（8192 字节）
  - 启用 LRU 清理机制，防止连接泄漏
- **Captive Portal 集成**：`http_server_captive_404_handler()` — 在 AP 模式下将任意 404 请求重定向到设备管理页面
- **连接生命周期管理**：`http_server_close_fn()` — 关闭 WebSocket 连接时自动清理资源
- **全局上下文**：`http_server_ctx_t` — 持有存储路径、服务回调函数指针集合

### 2. 路由注册体系

在 `http_server_start()` 中，统一注册了 8 组路由：

| 路由组 | 注册函数 | 功能 |
|--------|---------|------|
| 静态资源 | `http_server_register_assets_routes()` | 提供前端打包文件的解压与 HTTP 服务 |
| 能力配置 | `http_server_register_capabilities_routes()` | 能力（Capabilities）的增删改查 |
| Lua 模块 | `http_server_register_lua_modules_routes()` | Lua 脚本模块的管理 |
| 设备配置 | `http_server_register_config_routes()` | WiFi、时区、LLM 等配置的读写 |
| 设备状态 | `http_server_register_status_routes()` | WiFi 状态、存储状态等系统信息 |
| 文件管理 | `http_server_register_files_routes()` | FATFS 文件系统的浏览与操作 |
| 微信登录 | `http_server_register_wechat_routes()` | 微信二维码扫码登录流程 |
| Web IM | `http_server_register_webim_routes()` | 即时聊天的 WebSocket 端点 |

### 3. 配置 API (components/http_server/http_server_config_api.c)

- **完整 RESTful CRUD 接口**，管理以下设备配置：
  - WiFi STA 配置（SSID/密码）
  - WiFi AP 配置（SSID/密码/行为模式）
  - 时区设置（POSIX TZ 格式）
  - 6 个 LLM 提供商配置（OpenAI、Bailian、DeepSeek、Anthropic、OpenAI Compatible、Anthropic Compatible）
  - 4 个 IM 平台配置（微信、飞书、QQ、Telegram）
  - 搜索引擎配置（Tavily API Key）
- **配置验证**：保存前调用 `main_save_config()` 验证 WiFi 配置
- **脏检测支持**：配合前端实现未保存提示

### 4. 状态 API (components/http_server/http_server_status_api.c)

- **WiFi 状态查询**：当前连接状态、IP 地址、AP 模式、AP SSID、AP IP
- **设备重启接口**

### 5. 文件管理 API (components/http_server/http_server_files_api.c)

- FATFS 文件系统的远程管理接口
- 文件浏览、上传、下载、删除

### 6. WebSocket 即时通信 (components/http_server/http_server_webim_api.c)

- **WebSocket 服务端**：实现 `/ws/im` 端点
- **WebSocket 连接管理**：`http_server_webim_ws_fd_remove()` — 断开时自动清理
- **消息路由**：将 WebSocket 消息桥接到 `app_claw` 的 IM 处理管道
- 心跳保活机制

### 7. 能力管理 API (components/http_server/http_server_capabilities_api.c)

- **Capabilities（能力）CRUD**：设备对外提供的能力列表管理
- 支持动态增删能力条目

### 8. Lua 模块管理 API (components/http_server/http_server_lua_modules_api.c)

- Lua 脚本模块的上传、列表、删除
- 与设备上运行的 Lua 解释器集成

### 9. 微信登录 API (components/http_server/http_server_wechat_api.c)

- **微信扫码登录全流程**：
  - 启动登录：生成二维码数据 URL
  - 轮询状态：查询扫码进度
  - Token 持久化：确认登录后标记持久化
  - 取消登录

### 10. 辅助模块

- **`http_server_json.c`** — JSON 编解码工具函数，基于 `cJSON`
- **`http_server_utils.c`** — URL 解析、路径拼接、MIME 类型映射等通用工具
- **`http_server_assets.c`** — 静态资源服务，Vite 构建的前端文件打包为 `.gz` 内嵌 Flash，启动时解压提供 HTTP 服务
- **`http_server_priv.h`** — 内部头文件，定义服务器上下文结构体和各模块的私有函数声明

---

## 技术难点与解决方案

| 难点 | 解决方案 |
|------|---------|
| 嵌入式资源受限 | 前端压缩为 `.html.gz` 存放 Flash，按需解压 |
| 多模块路由组织 | 分层注册架构，每个模块独立注册路由 |
| WebSocket 并发管理 | 连接关闭回调中自动清理 FD |
| Captive Portal | 404 错误拦截 + 302 重定向到管理页面 |
| 前端静态服务 | Vite 构建 → gzip 压缩 → 内嵌到固件 → HTTP 解压服务 |

## 代码规模估算

| 文件 | 预估代码量 |
|------|-----------|
| http_server_core.c | ~180 行 |
| http_server_config_api.c | ~300 行 |
| http_server_status_api.c | ~100 行 |
| http_server_files_api.c | ~200 行 |
| http_server_webim_api.c | ~250 行 |
| http_server_capabilities_api.c | ~150 行 |
| http_server_lua_modules_api.c | ~150 行 |
| http_server_wechat_api.c | ~200 行 |
| http_server_assets.c | ~100 行 |
| http_server_json.c | ~150 行 |
| http_server_utils.c | ~200 行 |
| http_server_priv.h | ~80 行 |
| include/http_server.h | ~120 行 |
| **合计** | **~2,180 行 C 代码** |

---

## 与我相关的关键函数

```c
// 核心
esp_err_t http_server_init(const http_server_config_t *config);
esp_err_t http_server_start(void);
esp_err_t http_server_stop(void);
esp_err_t http_server_captive_404_handler(httpd_req_t *req, httpd_err_code_t error);

// Web IM
esp_err_t http_server_register_webim_routes(httpd_handle_t server);
void http_server_webim_ws_fd_remove(int sockfd);

// 微信
esp_err_t http_server_register_wechat_routes(httpd_handle_t server);
