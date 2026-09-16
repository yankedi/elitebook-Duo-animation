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

## 10. Fold Effect（最终目标，已实现）

```powershell
.\build\Release\DragonflySensorDiag.exe --fold-effect
```

屏幕合到**激活角（110°）以下**时，桌面保持激活角的姿态并逐渐模糊；开回激活角以上，桌面完全不受影响。

### 策略来源：lid-plane

效果语义取自 `jh3y/lid-plane`（GPL-3.0，**仅策略参考，未复制代码**）：

> *"holds your desktop at an apparent fixed angle and progressively blurs it as you close the lid below 110°. Open it above that angle and your desktop is untouched."*

| 屏幕角 | 行为 |
|---|---|
| **> 110°** | **桌面完全不处理**（这就是正常使用的姿态） |
| **= 110°** | 效果从零开始 |
| **< 110°** | `delta = 110° - hingeAngle` 越大，效果越强（正在合盖） |

关键点是**零点在激活角而不是 180°**：笔记本不会、也不需要合到 180°；
180° 只是「摊平」的边界，而效果属于**合盖这个动作**。
之前把零点放在 180°，结果是正常使用时就有模糊、而合盖方向反而越来越"干净"。

### 数据流

```
OrientationSensor → hingeAngle → delta = 110° - hingeAngle
        │
        ├─ delta > 0 → 抓屏（仅一次）→ 折叠 shader → overlay 显示
        └─ delta ≤ 0 → 隐藏 overlay，完全停止渲染
```

### 组成

| 模块 | 职责 |
|---|---|
| `capture/DesktopCapture` | DXGI Desktop Duplication 抓取主显示器 |
| `graphics/FoldRenderer` | HLSL 折叠 shader（全屏三角形，运行时编译） |
| `graphics/OverlayWindow` | 全屏、置顶、鼠标穿透的覆盖窗口 |
| `app/FoldEffectMode` | 主循环与 overlay 生命周期 |

### Shader 模型

ray-plane 模型（`Duo-animation` / `duo-open` 共用）+ lid-plane 的**模糊标定**：

```
固定的内容平面（捕获的桌面，保持激活角的姿态）
        ↑
        │   逐像素：眼睛 → 玻璃点 → 延伸到平面求交
   [玻璃]│   模糊半径 ∝ sin(delta)，按屏高归一化
  ╱     │   变暗 ∝ 模糊半径（散射吸收）
 ╱      │   卷积核完全错过内容 → 黑色
────────┴──────── 铰链线（面板底边）
```

针对本机的改动：铰链是**水平线**（笔记本盖），因此工作坐标做了转置（duo-open 的 `axisSwap` 思路）；没有 pane side / hinge position 需要解析，铰链恒为面板底边。

### 角度映射

| hingeAngle | delta | 效果 |
|---|---|---|
| 140° | −30° | 旁路 shader，桌面原样通过 |
| **110°** | **0°** | **临界点：效果从零开始** |
| 100° | 10° | 轻微透视压缩 + 顶部模糊 |
| 50° | 60° | 满量程（`maxDeltaDegrees` 上限） |
| < 50° | 60° | 保持满量程直到合盖 |

`delta ≤ 0` 时**直接旁路**，所以正常使用时不会有任何残留模糊；
越过摊平（Lid Mode 3+）同样关闭效果。

### 模糊标定

```
radius = blurStrength × smoothstep(0.08, 1.0, height) × sin(delta) × 屏高/1000
```

- `height = 1 - uv.y`：**0 在屏幕顶部，1 在铰链处**
- `smoothstep(0.08, 1.0, …)`：靠近铰链的最后一段保持清晰（参考 lid-plane）
- 按屏高归一化 → 换分辨率不会改变观感

### 生命周期与安全门（DisplaySafetyGate）

`delta > 0` 只是必要条件，**不是**充分条件。真正决定 overlay 能否出现在屏幕上的是一道安全门，
同样来自 lid-plane 的策略（`DisplaySafetyGate`：*fail closed, then wait for a stable display
before restarting capture*）：

| 状态 | 触发 | 行为 |
|---|---|---|
| `Paused - lid closed` | 盖子开关说已合上，或 `hingeAngle ≤ 8°` / Lid Mode 0（带迟滞） | 隐藏 overlay，停止渲染 |
| `Paused - display unavailable` | 显示器电源状态变为 off | 同上 |
| `Paused - capture lost` | duplication 返回 `DXGI_ERROR_ACCESS_LOST` 等 | 同上，并重建 duplication |
| `Paused - sensor unavailable` | OrientationSensor 超过 1 s 没有新读数 | 同上 |
| `Waiting for display...` | 以上全部恢复 | 继续等待 0.12 s 稳定期 |
| `Ready` | 稳定期满 | 才允许重新抓屏并显示 |

为什么需要它：**合盖时 `delta` 正好是最大值**。没有这道门，overlay 会在整个合盖期间一直挂着，
显示的是"合盖前那一刻"的冻结快照（满量程模糊，且 50° 以下 `sin(delta)` 饱和所以看起来毫无变化），
开盖后第一眼看到的就是它 —— 现象就是"卡在模糊界面，直到回到激活角才恢复"。

其余保障：

- **没有有效帧就绝不显示 overlay**（fail closed）：抓屏失败时宁可不做效果，也不放一张旧图上去
- 每次进入效果只抓**一次**屏（先隐藏 overlay 再抓），避免 shader 采样自身输出形成 feedback loop；
  抓屏失败会在下一轮重试
- 合盖/开盖、显示器电源切换、`WM_DISPLAYCHANGE`、睡眠唤醒都会**重置**稳定期，
  所以开盖后不可能复用合盖前的帧
- 分辨率/输出变化时按新的模式重建 capture、交换链、内容纹理与渲染器
- overlay 为 `WS_EX_TRANSPARENT`，鼠标可穿透，不影响正常操作
- `delta ≤ 0` 或越过摊平（Lid Mode 3+）时同样隐藏并完全停止渲染（空闲 CPU 接近 0）
- `[ESC]` / `[F10]` 全局退出（`GetAsyncKeyState`，不依赖窗口焦点）

### 为什么开盖后效果能立刻出现

第一版实现里开盖后有一段"桌面是正常的"空档，原因有两个，都不是效果本身的问题：

1. **稳定期太长**：参考实现用 0.5 s，但一次开盖动作只有约 1 秒，
   0.5 秒会吃掉大半个动作。这里用 0.12 s，只够跳过合成器重建中的那一两帧。
2. **误以为"抓不到新帧 = 不能用旧帧"**：
   `IDXGIOutputDuplication::AcquireNextFrame` **只在合成画面发生变化时才给帧**，
   超时（`DXGI_ERROR_WAIT_TIMEOUT`）的含义恰恰是"画面和上次交付给你的那一帧完全一致"——
   而上次交付的那一帧就存在 `content` 纹理里。所以**超时不是失败，手里那帧仍然是当前桌面**。
   真正不能用的是 duplication 失效（`DXGI_ERROR_ACCESS_LOST` 等），那种情况才必须重建并等待。

因此现在的策略是：

- 暂停期间**保留**最后一张真实桌面快照，不清空
- overlay 隐藏期间每秒**顺带刷新**一次（`AcquireFrame(0)`，有不花时间就更新，
  所以下一轮开合拿到的是"最多 1 秒前"的画面，而不是合盖前那一刻的）
- 效果出现的条件只是"环境健康 0.12 s"，抓到帧就立刻显示；
  只有 duplication 真的失效时才需要重建

另外，Windows 空闲超时熄灭屏幕后，开盖时面板本身需要时间点亮 —— 那段时间里
屏幕是黑的，程序无能为力。所以运行时用
`SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED)`
**不让空闲超时把面板关掉**（否则开盖第一眼看到的是正在唤醒的面板，效果不可能比面板更早）。
副作用是程序运行期间屏幕不会自动熄灭；用 `--allow-display-off` 可以恢复系统默认行为。

启动横幅会打印这道门实际拿到的信号源，便于判断是哪一条在起作用：

```
monitor power on (notify yes), lid switch unknown (notify yes), settle 0.5 s
```

`notify yes` 表示 Windows 接受了该电源通知的订阅；`lid switch unknown` 表示还没收到过
盖子开关事件（该 API 只在状态变化时通知，不提供初始值），此时门依赖角度与显示器电源。

逻辑部分有自检，不需要真的去合盖子（`--selftest`）：

```
=== display safety gate self test ===
  lid shut  (no settle allowed)        hidden   ok
  state after a lid close              Paused - lid closed    ok
  reopened, 0.1 s                      hidden   ok
  reopened, 0.6 s                      ready    ok
  ...
gate self test: 19 checks, 0 mismatches -> PASS
```

### 可调参数

在 `FoldEffectOptions`（`app/FoldEffectMode.h`）与 `FoldEffectParameters`（`graphics/FoldRenderer.h`）中：

| 参数 | 默认 | 说明 |
|---|---|---|
| `activationAngleDeg` | 110 | 激活角；≥ 它时桌面完全不处理（lid-plane 的默认值） |
| `maxDeltaDegrees` | 60 | 交给 shader 的最大角度差 |
| `blurStrength` | 65 | 每 1000 px 屏高、在最大 delta 时的模糊半径（lid-plane 的常量） |
| `darkening` | 0.015 | 每像素模糊半径造成的亮度衰减 |
| `eyeDistancePx` | 12 288 | 视点到内容平面的距离（= 6.4 × 屏宽），保持投影接近恒等 |
| `recoverySettleSeconds` | 0.12 | 环境恢复后到允许效果重新出现之间的稳定期（参考实现 0.5 s，对笔记本开盖太慢） |
| `keepDisplayAwake` | true | 运行期间阻止空闲超时熄灭屏幕；`--allow-display-off` 关闭 |

### 实测

```
display 1920x1080, 120 dpi -> eye distance 12288 px
activation 110 deg, blur 65/1000px, darken 0.015, max delta 60 deg
idle   hinge  114.1   delta   -4.1 (act 110)   lid 1   overlay off   frames 0
```

---

## 11. 下一步

1. **性能**：模糊循环固定 32 次采样，可按半径自适应减少
2. **过渡**：给 tilt 加低通，让快速开合更柔和
3. **多显示器**：目前只抓主输出（`EnumOutputs(0)`）
4. **打包**：加 LICENSE、CI 构建

## 12. 附：渲染器接口设计（原始记录）

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
