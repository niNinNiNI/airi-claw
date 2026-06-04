---
{
  "name": "cap_servo",
  "description": "控制双舵机：设置角度、播放动作序列、停止运动。通过 MCPWM 驱动两个舵机通道。",
  "metadata": {
    "cap_groups": ["cap_servo"],
    "manage_mode": "readonly"
  }
}
---
# cap_servo

舵机控制能力，支持两个通道（0 和 1），通过 MCPWM 50Hz PWM 信号驱动。

## 双舵机物理布局与旋转定义

本设备为双自由度云台结构（Pan-Tilt），两个舵机构成上下两层：

| 通道 | 舵机位置 | 旋转轴 | 控制方向 | 英文术语 |
|------|----------|--------|----------|----------|
| servo0 (channel=0) | 下层（底座） | 垂直轴 | 左转（负角度）/ 右转（正角度），水平扫视 | Yaw (Pan) |
| servo1 (channel=1) | 上层（支架） | 水平轴 | 抬头（正角度）/ 低头（负角度），垂直俯仰 | Pitch (Tilt) |

- **servo0 (Yaw/Pan)**: 控制摄像头/云台的水平左右旋转，底座固定，旋转轴垂直于地面。
- **servo1 (Pitch/Tilt)**: 控制摄像头/云台的垂直上下俯仰，支架固定于底座之上，旋转轴平行于地面。
- 归零角度: servo0=0°（正前方），servo1=60°（略向下倾斜的默认视角）。

## 可用能力

### set_servo_angle

设置指定通道的舵机角度，支持缓动过渡。

**参数：**
- `channel` (int, 必须): 舵机通道，0 或 1
- `angle` (number, 必须): 目标角度（度），范围取决于配置的 max_angle（默认 ±135°）
- `duration_ms` (int, 可选): 缓动过渡时间（毫秒），默认 1000，设为 0 表示立即到位

**返回：** `"Servo <channel> set to <angle>°"`

### play_motion_sequence

播放一组动作序列，异步执行（调用后立即返回）。

**参数：**
- `actions` (array, 必须): 动作列表，每组包含：
  - `duration_ms` (int): 该动作持续时间
  - `servo0` / `servo1` (object): 每个通道的动作配置
    - `mode` (string): `"fixed"` / `"sweep"` / `"ease"`
    - `angle` (number): fixed/ease 模式的目标角度
    - `min_angle` / `max_angle` (number): sweep 模式的扫摆范围
    - `step` (number): sweep 模式的步进量
    - `step_delay_ms` (int): sweep 模式的每步延时
    - `ease_type` (string): ease 模式的缓动曲线 (`"linear"`, `"ease_in"`, `"ease_out"`, `"ease_in_out"`, `"ease_in_out_cubic"`)
- `loop` (bool, 可选): 是否循环播放，默认 false

**返回：** `"Playing N motions"` 或 `"Playing N motions in loop"`

### stop_servo

立即停止所有舵机运动。

**参数：**
- `reset` (bool, 可选): 是否归零到 0°，默认 true

**返回：** `"Servo stopped and reset to 0°"` 或 `"Servo stopped"`

## 调用示例

```
cap call set_servo_angle {"channel":0,"angle":45,"duration_ms":1000}
cap call stop_servo {"reset":true}
```

## 注意事项

- 舵机是物理执行器，操作有真实副作用，请谨慎调用
- play_motion_sequence 一次只能运行一个序列，新调用会自动停止旧序列
- set_servo_angle 会先停止正在运行的 motion 序列再执行
- 默认 GPIO: 通道0→GPIO4, 通道1→GPIO5（可通过 menuconfig 修改）
