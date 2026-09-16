# Dragonfly Sensor Diagnostic + Fold Estimator

HP Elite Dragonfly G2 / Windows 11 x64 的传感器诊断与**折叠进度估计器**。

当前包含两个里程碑：

- **Milestone 1–3**：真实、可靠地读取本机传感器并记录日志（已完成并验证）
- **Milestone 5A**：`HingeEstimator` v1 + 实时调试可视化（本文件后述部分）

---

## 0. 最重要的一句话

程序输出的是 **foldProgress（视觉折叠进度，0.0 ~ 1.0）**，
**不是物理铰链角度**。

本机只有一个 IMU + 一个离散的 5 态 Lid Mode 传感器，没有 `HingeAngleSensor`。
单 IMU 测量的是自身相对地球的姿态，因此**物理上无法**严格区分「屏幕相对底座转动」与
「整机一起转动」。所以本项目不输出 `angleDegrees`，而是输出渲染器真正需要的、
连续、响应快、视觉正确的 progress。

---

## 1. 架构

```
src\
  app\
    main.cpp                     命令行 / 主循环 / 键盘 / 编排
    ConsoleUi.*                  终端启动报告 + 实时帧
  sensors\
    SensorTypes.h                SensorSample 等基础类型
    SensorManager.*              7 个标准 WinRT 传感器
    CustomSensorManager.*        Intel Lid Mode（CustomSensor 通道）
    HidSensorEnumerator.*        原生 SetupAPI/HID 取证通道
    SensorLogger.*               异步 CSV（独立线程）
  hinge\
    HingeTypes.h                 HingeState / RawLidMode / 配置结构
    HingeEstimator.*             v1 估算器（陀螺积分 + Lid 锚点）
  animation\
    FoldAnimationController.*    平滑，唯一的渲染器输入
  graphics\
    D3DDevice.*                  D3D11 设备 + swap chain
    DebugFoldWindow.*            Win32 + D3D11 调试折叠窗口
  config\
    ConfigLoader.*               Windows.Data.Json 读取 config.json
    config.json                  全部可调参数
```

数据流（渲染器永远不接触传感器层）：

```
SensorManager + CustomSensorManager
        -> SensorSample
        -> HingeEstimator            (foldProgress / direction / region / confidence)
        -> FoldAnimationController   (平滑 + 速度)
        -> ConsoleUi / DebugFoldWindow
        -> SensorLogger (异步 CSV，独立路径)
```

线程模型：传感器回调只写入带小互斥锁的最新值；主循环 60 Hz 采样、估算、渲染。
CSV 由独立线程每 100 ms 批量落盘。

---

## 2. 依赖与构建

- Visual Studio 2022 Build Tools，含 MSVC v143 + Windows 11 SDK
- CMake ≥ 3.21

不使用 NuGet，不需要 `cppwinrt.exe` 代码生成（直接消费 SDK 自带投影头文件）。

```powershell
cd src
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

或：

```powershell
.\build.ps1
```

产物：`src\build\Release\DragonflySensorDiag.exe`

---

## 3. 运行

```powershell
# 正常运行（会打开调试折叠窗口）
.\build\Release\DragonflySensorDiag.exe

# 只枚举设备后退出
.\build\Release\DragonflySensorDiag.exe --enumerate

# 不打开调试窗口，只用控制台
.\build\Release\DragonflySensorDiag.exe --no-window

# 以手动模式启动（不依赖传感器即可测试动画）
.\build\Release\DragonflySensorDiag.exe --manual

# 采集 60 秒
.\build\Release\DragonflySensorDiag.exe --seconds 60

# 把当前默认配置写出来（作为模板）
.\build\Release\DragonflySensorDiag.exe --write-config

# 用合成陀螺序列验证估算器（不需要转动屏幕）
.\build\Release\DragonflySensorDiag.exe --selftest
```

`--selftest` 会打印分阶段结果（静止 / 快速开 / 保持 / 慢速开 / 保持），
用来确认「运动开始后 progress 是否立即响应、方向是否正确、停止后是否漂移」。

其他参数：`--rate HZ`（默认 50）、`--report-ms MS`（默认 16）、`--log DIR`、`--no-log`、`--hid-raw`。

日志：`src\build\Release\logs\sensor-<时间戳>.csv` 与 `startup-report.txt`

---

## 4. 快捷键

| 键 | 功能 |
|---|---|
| `R` | 重置估算器（progress 回到当前 Lid Mode 的锚点） |
| `A` | 切换 Auto / Manual 模式 |
| `+` / `=` | progress 增加一步（自动切到 Manual） |
| `-` / `_` | progress 减少一步（自动切到 Manual） |
| `C` | 校准（v1 未实现，会提示；自动轴检测已生效） |
| `Q` / `ESC` | 退出 |

控制台和调试窗口都接受这些键（窗口需要获得焦点）。

**Manual 模式很重要**：即使传感器算法暂时不理想，也能用它手动驱动调试窗口验证动画。

---

## 5. 配置（`src/config/config.json`）

构建时会把该文件复制到 exe 旁边（`build\Release\config\config.json`）。
程序按 exe 目录 → 上级目录 → 源码目录的顺序查找，编辑源码那份后重新构建即可生效。

主要参数：

| 参数 | 含义 |
|---|---|
| `hingeAxis` | `"auto"` 或 `"x" / "-x" / "y" / "-y" / "z" / "-z"` 强制指定 |
| `medianFilterLength` | 中值滤波长度（奇数）。**本机 X 轴有 ~19°/s 的脉冲噪声，必须靠中值剔除** |
| `rateSmoothingTauSeconds` | 速率短时平均时间常数，用于区分噪声与慢速意图运动 |
| `gyroDeadZoneDegPerSec` | 低于此角速度不积分 |
| `gyroProgressGain` | progress 增益；`1/180` 表示 180° 行程正好填满 progress |
| `idleRateFloorDegPerSec` / `idleFreeze*` | 静止冻结：低于该速率一段时间后把速率衰减到 0，彻底消除随机游走漂移 |
| `biasLearning*` | 陀螺零偏学习（限速 / 延迟 / 时间常数） |
| `motionStartThreshold` / `stableThreshold` / `settleTimeMs` | 状态机与滞回（单位是 progress/s，不是 deg/s） |
| `state1MaxProgress` / `state2MinProgress` / `state3ForcesOpen` | Lid Mode 对 progress 的区间约束 |
| `state1DefaultProgress` / `state2DefaultProgress` | 冷启动锚点 |
| `signProbe*` | 用 Lid Mode 跳变解析轴符号的窗口与最小速率 |
| `axisWindowSeconds` / `axisSwitchRatio` / `axisEnergyFloor` / `axisEnergyMinRateDegPerSec` | 自动轴选择（只有真实运动才能投票） |
| `accelDeviationLimitG` / `axisShareFloor` / `motionPenalty` / `motionConfirmRateDegPerSec` | 整机运动抑制 |
| `manualStepPerPress` | 手动模式下每次按键的步长 |
| `animation.smoothingTauSeconds` | 渲染平滑时间常数 |
| `animation.snapThresholdProgress` | 超过此跳变直接吸附（不产生速度尖峰） |
| `debugWindow.*` | 调试窗口开关与尺寸 |

---

## 6. HingeEstimator v1 算法

### 6.1 各传感器职责

| 来源 | 用途 |
|---|---|
| **Gyroscope** | 唯一的连续运动来源：滤波后积分成 progress，判断开合方向 |
| **Lid Mode** | 离散锚点：progress 区间约束、冷启动锚、**轴符号解析参考** |
| **Accelerometer** | 重力参考：`| |a| - 1g |` 超限时判定为线性运动 → 抑制更新 |
| **OrientationSensor** | **不参与** progress 计算；仅用于置信度（是否存在融合姿态） |

### 6.1.1 信号链（这是本机可用性的关键）

本机陀螺仪有一个必须处理掉的实际特性 —— 静止时 X 轴的**零均值噪声标准差高达 19°/s、峰值 ±60°/s**，
而 Y/Z 轴只有 3°/s 和 2.4°/s。用简单的均值滤波会把这种脉冲抹成一个持续假速度并积分成漂移。
因此信号链是：

```
原始 rad/s → 度/秒
    ↓ 中值滤波（medianFilterLength，剔除孤立脉冲）
    ↓ 零偏学习（仅在明确静止时更新，用峰值而非模长判定）
    ↓ 短时平均（rateSmoothingTauSeconds，抹平残余噪声）
    ↓ 静止冻结（低于 idleRateFloor 一段时间后把速率衰减到 0）
    ↓ 死区 → 轴选择 / 符号 → 积分
```

轴统计同样只接受**真实运动**（速率 ≥ `axisEnergyMinRateDegPerSec`）的样本：
否则 X 轴的噪声会赢得能量投票，估算器就会去积分错误的轴。

### 6.2 每次更新的顺序

1. **Lid Mode 记录** —— 状态变化时启动符号探测窗口
2. **冷启动锚定** —— 第一次已知 Lid Mode 时把 progress 设到对应默认值，避免每次启动都从 0 开始
3. **陀螺零偏学习** —— 角速度低于 `biasLearningRateLimit` 且持续 `biasLearningDelay` 后，用 `biasLearningTau` 缓慢学习各轴零偏
4. **轴能量统计** —— 仅当角速度模长超过 `axisEnergyMinRateDegPerSec` 时才累计（静止噪声不污染轴选择）
5. **整机运动评估** —— 线性加速度超限 或 轴能量占比低于 `axisShareFloor` → 判定为整机运动
6. **抑制平滑** —— 抑制系数按 `orientationCorrectionGain` 决定的时间常数变化，避免单帧噪声卡死估算
7. **求铰链角速度** —— `rate = (gyro[axis] - bias[axis]) * sign`，低于死区归零
8. **积分** —— `progress += rate * gain * (1 - suppression) * dt`（dt 有限幅）
9. **Lid 锚点约束** —— 按状态把 progress 拉回允许区间
10. **状态机** —— 更新 Opening/Closing/Stable（带滞回与 settle 时间）
11. **置信度** —— 综合轴占比、符号是否解析、零偏是否学习、线性加速度等

### 6.3 Lid Mode 如何参与（关键）

`1 → 2 → 3 → 4 → 5` 只被当作**不透明的区间锚点**，代码里没有把它们映射成任何角度：

```cpp
enum class RawLidMode : uint32_t { Unknown = 0, State1 = 1, ... State5 = 5 };
```

| 状态 | 对 progress 的作用 |
|---|---|
| State1 | 只设**上限** `state1MaxProgress`（允许一路降到 0，因为可能是接近关闭状态） |
| State2 | 约束到 `[state2MinProgress, state2MaxProgress]` |
| State3+ | 直接视为视觉上已完成（`progress → 1.0`），因为 v1 只做「开盖 → 正常显示」这一段 |

另外，**状态码上升 = 正在打开** 是被当作「本机观测到的行为」使用的，
仅用于解析轴符号，而不是 Intel 官方契约；可用配置覆盖。

### 6.4 Auto hinge-axis 如何工作

1. 每帧把「明显运动」样本的 `g²·dt` 累加到三个轴的能量桶
2. 每 `axisWindowSeconds` 评估一次：哪一轴能量占比最高
3. 只有当最优轴占比 ≥ 0.5，**且** 相对当前轴超过 `axisSwitchRatio` 倍时才切换（迟滞，避免每帧抖动）
4. 能量桶每次评估后减半，使近期运动占主导但短暂静止不会丢失结论
5. **符号**：Lid Mode 跳变后的 `signProbeWindowSeconds` 内，如果主轴角速度超过 `signProbeMinRateDegPerSec`，就用「状态码上升 = 打开」这条观测来定符号，并标记为已解析
6. 配置 `hingeAxis` 为显式值时，整个自动过程被禁用

调试 UI 会显示当前轴、符号、占比与「符号是否已解析」。

### 6.5 防止整机运动误判

- `| |a| - 1g | > accelDeviationLimitG` → 线性运动，抑制最高 70%
- 主轴能量占比 < `axisShareFloor`（旋转分散在多轴，典型整机旋转）→ 抑制 ≥ 50%
- 抑制系数本身经过平滑（时间常数由 `orientationCorrectionGain` 决定），且最多抑制 `motionPenalty` 比例
- 被抑制时仍然继续更新方向和置信度，只是 progress 几乎不动

---

## 7. 调试折叠窗口

- Win32 窗口 + D3D11（`d3d11 / dxgi / d3dcompiler`），HLSL 运行时编译
- 两个四边形：底座固定，屏幕绕铰链线旋转
- 屏幕角度 = `5° + progress × 90°`（不取 0°/90° 是为了让两个面板始终可辨认）
- 屏幕亮度随 confidence 变化
- 标题栏显示 `progress / confidence / AUTO|MANUAL`

它**不是**最终的 iPhone Duo 折叠 shader，只用来肉眼判断
progress 是否连续、方向是否正确、静止时是否漂移。

---

## 8. 本机实测结论（2026-09-16）

| 项目 | 结果 |
|---|---|
| Accelerometer / Gyrometer | 存在，16 ms / min 10 ms，事件正常 |
| OrientationSensor / Inclinometer / Compass | 存在，事件正常 |
| SimpleOrientationSensor | 存在（软件设备） |
| **HingeAngleSensor** | **不存在**（`GetDefaultAsync()` 返回 null） |
| **Intel Lid Mode Sensor** | **已找到并读取成功** |

Lid Mode 关键事实（程序实测）：

```
设备名   : Lid Mode Sensor
设备路径 : \\?\HID#Vid_8087&Pid_0AC2#6&1201cbe5&0&0000
           #{00000300-766d-4333-8262-27e82dd158b1}\{03000000-0000-0000-0000-000000000000}
上报间隔 : 16 ms / min 10 ms，MaxBatchSize 512
状态属性 : {C458F8A7-4AE8-4777-9607-2E9BDD65110A} 161  (UInt32)
```

完整读数示例：`161 = 1`、`162 = 0`、`187 = 9`、`188 = 单调递增`、
`2 = DateTime(100ns)`、`{B14C764F-…} 5 = 768`。

其他发现：

- Intel Sensor Hub 的三个 HID 集合都不是标准 HID 顶层集合（`HidD_GetPreparsedData` 失败），
  它们用私有接口类 GUID 注册，而不是 `GUID_DEVINTERFACE_HID`
- 全机没有 usage page 0x20 的 HID 接口，也没有 `GUID_DEVINTERFACE_SENSOR` 接口
  → WinRT CustomSensor 是读取 Lid Mode 的唯一可行通道
- **Accelerometer 单位是 g**，不是 m/s²

### 静止与响应实测（修复后）

| 场景 | 结果 |
|---|---|
| 静止 30 秒 | progress 恒为 0.7500，**漂移 0.0000** |
| 静止 45 秒 | 波动 ±0.005，漂移 -0.0014 |
| 合成 90°/s 开合 | 运动后约 0.1 s 内开始响应，方向 Opening |
| 合成 20°/s 慢速开合 | 仍能识别（约 4 帧后启动） |
| 停止后 | 速率归零，progress 保持不变 |

本机陀螺仪噪声实测（静止，机器完全未动）：

```
gyro_x:  mean  0.084   stddev 19.33   min -54.6   max +61.0   deg/s
gyro_y:  mean -0.050   stddev  3.12   min -12.0   max +23.6   deg/s
gyro_z:  mean -0.015   stddev  2.41   min -27.2   max +12.2   deg/s
accel |a|: 0.9907 ± 0.045 g
```

X 轴是**零均值**噪声（不是偏置），因此中值滤波 + 静止冻结是本机可用的前提。

---

## 9. 已知限制

1. **没有物理角度**。`HingeState::estimatedAngleDegrees` 恒为空，v1 不输出任何角度数字。
2. **整机运动抑制是启发式的**。拿起来走动时 progress 会被压低但不会立即归零，
   停稳后按零偏与轴占比恢复。
3. **符号解析需要一次 Lid Mode 跳变**。首次使用前若从未发生过状态切换，
   符号置信度较低（confidence 会体现，UI 显示 `sign resolved no`）。
4. **轴在首次真实运动前保持 `n/a`**。这是刻意的：本机噪声太大，静止时不允许噪声决定轴。
   一旦发生真实开合，轴和符号都会被解析，confidence 随之上升。
5. **响应有约 0.1 秒的滤波延迟**，来自中值滤波 + 短时平均。
   若要更快，可把 `medianFilterLength` 降到 3、`rateSmoothingTauSeconds` 降到 0.06，
   代价是静止稳定性下降。
6. **冷启动锚点是猜测**。程序启动时按 Lid Mode 取默认 progress（State1 → 0.75），
   同一次会话内的相对变化是准确的，但绝对值依赖这个假设。
7. **State3+ 一律视为已完全打开**。v1 不做 Flat → Tent → Tablet 段的可视化。
8. **校准（`C`）尚未实现**。自动轴检测已经可用；未来会用一次真实开合自动生成 `calibration.json`。
9. 调试窗口不是最终效果，只是数值可视化的载体。

---

## 10. 下一步：Fold Shader 如何接入

渲染器只需要 `FoldAnimationState`：

```cpp
struct FoldAnimationState {
    float progress;    // 0..1  —— shader 用它决定折叠程度
    float velocity;    // progress/s，可用于运动模糊强度或阻尼
    bool  opening;     // 方向
    bool  moving;      // 是否正在运动（可用于触发 overlay 生命周期）
    float confidence;  // 0..1，可用于淡入淡出或降级
    bool  manual;      // 是否手动驱动
};
```

接入方式（Milestone 6）：

1. 用 `progress` 驱动 Duo-animation / duo-open 那套 ray-plane 投影数学的 HLSL 重写版：
   `angle = f(progress)`，`progress → 1` 时投影回到 identity、blur → 0、darkening → 0
2. `moving == true` 时才启动桌面捕获与 overlay，`moving == false` 且 `progress` 稳定后销毁 overlay
   （对应 duo-open 的生命周期模型：检测运动 → capture → 显示 → 跟随 → 静止 → 隐藏）
3. 渲染器只依赖 `FoldAnimationState`，不感知 `SensorManager`、`HingeEstimator` 或任何 WinRT 类型
