# PSRAM + NVS Flash 操作导致崩溃的修复记录

## 崩溃现象

```
assert failed: spi_flash_disable_interrupts_caches_and_other_cpu cache_utils.c:127
(esp_task_stack_is_sane_cache_disabled())
```

系统启动后约 25 秒（SNTP 时间同步完成后）立即崩溃重启。

## 根因

ESP32 上执行 SPI Flash 读写（NVS、OTA、分区操作等）时，ESP-IDF 会暂时关闭 cache。PSRAM 依赖 cache 才能访问，如果当前任务的**栈**分配在 PSRAM 中，cache 一关闭栈本身就不可访问了，直接触发断言崩溃。

本项目中有两个任务的栈分配在 PSRAM，且都会触发 NVS Flash 写入：

### 崩溃路径 1（cap_scheduler 任务）

```
cap_scheduler_task (PSRAM栈)
  → cap_scheduler_fire_due_entries()
    → cap_scheduler_persist_runtime_state_locked()
      → NVS blob write → Flash操作 → cache关闭 → CRASH
```

### 崩溃路径 2（cap_time_sync 任务）— 实际触发崩溃的路径

```
cap_time_sync_service_task (PSRAM栈)
  → cap_time_sync_now()
    → SNTP同步成功
    → cap_time_notify_sync_success()
      → app_time_sync_success()          [app_claw.c:171]
        → cap_scheduler_handle_time_sync()
          → cap_scheduler_persist_runtime_state_locked()
            → NVS blob write → Flash操作 → cache关闭 → CRASH
```

## 修复

将两个任务的栈策略从 `CLAW_TASK_STACK_PREFER_PSRAM` 改为 `CLAW_TASK_STACK_INTERNAL_ONLY`，确保栈永远分配在内部 SRAM 中。

### 修改文件 1

[components/claw_capabilities/cap_scheduler/src/cap_scheduler.c:819](../../components/claw_capabilities/cap_scheduler/src/cap_scheduler.c#L819)

```diff
- .stack_policy = CLAW_TASK_STACK_PREFER_PSRAM,
+ .stack_policy = CLAW_TASK_STACK_INTERNAL_ONLY,
```

### 修改文件 2

[components/claw_capabilities/cap_time/src/cap_time.c:408](../../components/claw_capabilities/cap_time/src/cap_time.c#L408)

```diff
- .stack_policy = CLAW_TASK_STACK_PREFER_PSRAM,
+ .stack_policy = CLAW_TASK_STACK_INTERNAL_ONLY,
```

## 经验法则

> 任何可能触发 Flash 读写的任务（NVS、OTA、分区 API 等），其栈必须放在内部 SRAM 中。

项目中已有 `claw_task.c` 的 override 表 `s_task_configs[]` 对此类任务做了集中管控（`claw_core`、`event_router`、`claw_mem_extract`、`cap_lua_async`），`cap_scheduler` 和 `cap_time_sync` 之前遗漏了。

## 验证方法

重新编译烧录后，系统应在 SNTP 时间同步后不再崩溃，日志正常输出 `scheduler task started` 并持续运行。

```bash
idf.py fullclean && idf.py build && idf.py flash monitor
```
