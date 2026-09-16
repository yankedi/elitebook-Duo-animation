# HP Elite Dragonfly G2 铰链姿态感知方案调研记录

## 1. 项目目标

目标是在 **HP Elite Dragonfly G2 360° 翻转本**上实现类似 Surface Duo / 折叠设备的交互效果：

- 感知屏幕开合过程
- 尽可能估算屏幕与底座之间的铰链角度
- 根据开合程度实时驱动 UI 动画
- 区分普通笔记本、展开、帐篷、平板等姿态
- 尽量依赖设备现有传感器，不增加外部硬件

理想情况下希望得到连续的：

`0° → 90° → 180° → 270° → 360°`

但当前机器并没有向 Windows 公开标准的连续铰链角传感器，因此最终方案需要结合 IMU 与 Lid Mode 状态进行估算。

---

## 2. 机器与传感器平台

设备：

- HP Elite Dragonfly G2
- Intel Core i5-1135G7
- 360° 翻转屏
- Windows 11
- Intel Sensor Hub / Intel ISS

Windows 中可以看到多个：

`HID 传感器集合 V2`

这些并不等于存在多颗物理传感器，而是 Intel Sensor Hub 将不同物理传感器、融合结果和算法功能暴露为多个 HID Sensor Collection。

---

## 3. 已确认的物理传感器

### 3.1 STMicroelectronics LSM6DSOX

已经从 PnP 接口中明确确认存在：

- Manufacturer: `ST_MICRO`
- Model: `LSM6DSOX Sensor`
- Accelerometer Sensor
- Gyrometer Sensor

LSM6DSOX 是一颗六轴 IMU：

- 3 轴 Accelerometer
- 3 轴 Gyroscope

目前实测表明，**只转动屏幕、不移动底座时，Windows/浏览器里的姿态数据会随屏幕变化**。

因此有较强证据表明：

> 这套 IMU 数据对应屏幕侧姿态，或者至少 Windows 向应用暴露的是经过坐标转换后的屏幕参考系姿态。

### 3.2 STMicroelectronics LIS2MDL

还确认存在：

- Manufacturer: `ST_MICRO`
- Model: `LIS2MDL Sensor`
- Magnetometer Sensor

因此机器实际上具备：

`Accelerometer + Gyroscope + Magnetometer`

Windows / Intel Sensor Hub 可以利用它们进行姿态融合。

---

## 4. Windows 暴露出来的传感器

通过 WinRT 测试，已经确认存在：

- Accelerometer
- Gyrometer
- Inclinometer
- Compass
- OrientationSensor
- SimpleOrientationSensor
- LightSensor

另外 Intel Sensor Hub 还提供大量算法/派生接口，例如：

- Device Movement Sensor
- Shake Gesture Sensor
- Significant Motion Sensor
- Tilt Gesture Sensor
- On Table Detection
- Linear Acceleration
- Gravity Vector
- Relative Orientation
- Activity
- Pedometer
- Geomagnetic Orientation

因此不能按照 Windows 中显示的“传感器数量”推断物理芯片数量。

整体结构更接近：

`物理 MEMS → Intel Sensor Hub → 大量逻辑传感器`

---

## 5. 第二颗 IMU 调查结果

最初存在一种可能：

`13719565` 和 `1201CBE5` 分别代表两颗不同的 IMU。

后续检查发现：

### `13719565`

包含明确的：

- Accelerometer Sensor → ST LSM6DSOX
- Gyrometer Sensor → ST LSM6DSOX
- Magnetometer Sensor → ST LIS2MDL

此外还有：

- Uncalib Gyrometer
- Uncalib Magnetometer
- Device Movement
- Shake Gesture

### `1201CBE5`

包含：

- DVS Accelerometer Sensor
- DVS Gyrometer Sensor
- Lid Mode Sensor
- Significant Motion Sensor
- Tilt Gesture Sensor
- Simple DMD Sensor
- ST On Table Detection Sensor

其中 `ST On Table Detection Sensor` 仍然直接引用 `LSM6DSOX Sensor`。

因此目前最合理的拓扑是：

`LSM6DSOX → Intel Sensor Hub → 标准 Accel/Gyro + DVS + Gesture + Lid Mode 等`

而不是两颗独立 IMU。

### 当前结论

**没有发现第二颗可独立读取、可作为底座姿态参考的 Accel + Gyro IMU。**

因此不能直接使用：

`hinge_angle = orientation_screen - orientation_base`

来获得严格意义上的铰链角度。

---

## 6. Windows HingeAngleSensor 测试

Windows 自带：

`Windows.Devices.Sensors.HingeAngleSensor`

API 类型可以正常加载。

但是：

`HingeAngleSensor.GetDefaultAsync()`

最终返回 `null`。

因此：

> HP Elite Dragonfly G2 没有向 Windows 注册标准 `HingeAngleSensor` Provider。

也就是说无法直接获得：

`137.2°`

这种连续的官方铰链角度。

---

# 7. Lid Mode Sensor

这是目前最有价值的发现之一。

Intel Sensor Hub 明确暴露：

`Lid Mode Sensor`

接口 GUID：

`{00000300-766d-4333-8262-27e82dd158b1}`

设备路径位于：

`HID\VID_8087&PID_0AC2\6&1201CBE5&0&0000`

---

## 8. Lid Mode Sensor 的 WinRT 读取

通过：

`Windows.Devices.Sensors.Custom.CustomSensor`

可以成功：

1. 枚举 Lid Mode Sensor
2. 打开传感器
3. 订阅 `ReadingChanged`
4. 收到真实的状态变化事件

其基本参数：

- MinimumReportInterval = 10 ms
- ReportInterval = 250 ms
- MaxBatchSize = 512

---

## 9. Lid Mode 返回的数据

每个事件包含 6 个属性。

最重要的是：

`{C458F8A7-4AE8-4777-9607-2E9BDD65110A} 161`

它对应一个 UInt32 状态值。

实测值为：

`1 / 2 / 3 / 4 / 5`

另一个有明显关联的字段：

`{C458F8A7-4AE8-4777-9607-2E9BDD65110A} 162`

其规律基本为：

| Value1 | Value2 |
|---:|---:|
| 1 | 0 |
| 2 | 0 |
| 3 | 1 |
| 4 | 2 |
| 5 | 0 |

另外：

`187`

目前一直为：

`0`

而：

`188`

持续递增，且变化速度与时间高度对应，更像：

- 内部时间计数
- monotonic counter
- 驱动时间基准

暂时不认为它代表铰链角度。

---

# 10. Lid Mode 的关键实验结果

在一次从普通形态连续翻转到完全平板模式的测试中，得到非常干净的：

`1 → 2 → 3 → 4 → 5`

这证明：

> `Value1` 与屏幕从正常笔记本模式向 360° 平板模式翻转的进程高度相关。

另外，在从普通状态逐渐将屏幕平放时：

`1 → 2`

从平放状态继续向后翻转时出现：

`2 → 3 → 4`

因此目前可以建立一个初步模型：

| Lid Mode | 大致区域 | 当前解释 |
|---:|---|---|
| 1 | 正常笔记本侧 | Laptop / 前半区 |
| 2 | 接近摊平 | 180° 附近区域 |
| 3 | 已越过 180° | 后翻区域 I |
| 4 | 继续后翻 | Stand / Tent 附近区域 |
| 5 | 接近完全翻转 | Tablet / 360°附近 |

目前**具体切换角度还没有精确标定**，因此上述名称只是功能解释，不应硬编码成 Windows 官方定义。

最可靠的是：

`1 → 2 → 3 → 4 → 5`

代表一次完整的正向翻转过程。

---

# 11. 浏览器 Gyroscope / Motion 测试结果

通过浏览器里的 Gyroscope & Motion Sensor Test，可以观察到 X/Y/Z 随屏幕位置连续变化。

但进一步实验确认：

> 当保持铰链角度不变，而把整台电脑一起倾斜时，X/Y/Z 同样会变化。

因此可以确认：

这些数据反映的是：

`屏幕/设备相对于世界坐标系或重力方向的姿态`

而不是：

`屏幕相对于底座的角度`

即：

`Orientation ≠ Hinge Angle`

---

## 12. 为什么网站可以“看 X 判断屏幕位置”

因为在正常桌面使用时：

`底座姿态 ≈ 固定`

于是：

`屏幕绝对姿态 ≈ 屏幕相对于底座的角度`

因此只要底座保持平放或姿态基本不变：

`Orientation X`

就可以很好地近似表示屏幕开合程度。

这也是为什么浏览器测试中：

- 打开屏幕 → X 连续变化
- 合上屏幕 → X 连续返回
- 翻到另一侧 → 姿态继续变化

从用户体验角度看，它已经非常适合驱动开合动画。

---

# 13. 单 IMU 的物理限制

假设：

`真实铰链角 = 120°`

保持铰链不动，然后将整台电脑后仰 30°。

屏幕 IMU会看到：

`屏幕姿态变化 30°`

但实际上：

`铰链角仍然是 120°`

由于不存在第二颗底座 IMU，因此没有办法从单个屏幕姿态传感器严格区分：

`屏幕转动`

和：

`整机一起转动`

这是物理上的不可观测问题，而不是简单通过滤波算法就能完全解决的问题。

---

# 14. 为什么 Lid Mode 很重要

单独使用 Orientation X/Y/Z 时还存在一个问题：

例如姿态角可能发生：

`0 → 90 → 180 → -90 → 0`

因此同一个姿态值可能对应两个不同的铰链区域。

比如：

`X = 70°`

可能是：

- 普通笔记本侧
- 已经翻过 180°之后的另一侧

这时 Lid Mode 可以提供离散区间。

例如：

`Orientation X = 70° + LidMode = 1`

可以判断为普通笔记本区域。

而：

`Orientation X = 70° + LidMode = 4/5`

则说明已经翻到背面。

因此：

> Lid Mode 非常适合解决单个姿态角的 180° / 360° 歧义。

---

# 15. 推荐的最终传感器融合方案

建议将整个角度系统设计为：

`Orientation + Accelerometer + Gyroscope + Lid Mode`

其中：

### Orientation / Accelerometer

负责：

- 提供长期稳定的绝对姿态
- 根据重力方向获得屏幕倾角
- 避免纯陀螺积分漂移

### Gyroscope

负责：

- 判断当前是否正在开合
- 判断开合方向
- 在快速移动时提供低延迟角速度
- 用于平滑动画

### Lid Mode Sensor

负责：

- 判断当前位于 360° 开合范围的哪个区域
- 解决角度回绕
- 判断是否已经越过 180°
- 区分 Laptop / Flat / Tent / Tablet 等区域

最终逻辑可以抽象为：

`IMU 连续姿态 + LidMode 离散区间 → EstimatedHingeAngle`

---

# 16. 推荐的软件结构

建议把传感器逻辑单独抽象为：

`HingeEstimator`

对上层 UI 只暴露：

- `Angle`
- `Velocity`
- `Direction`
- `Mode`
- `Confidence`

例如：

- `Angle: 214.6`
- `Velocity: +37.2 deg/s`
- `Direction: Opening`
- `Mode: RearHalf`
- `Confidence: 0.91`

UI 不应该直接依赖：

- 原始 X/Y/Z
- Lid Mode 的 1/2/3/4/5
- 具体 GUID

底层负责完成转换。

---

# 17. 推荐状态机

可以建立：

`Closed → Laptop → Flat → RearHalf → Tablet`

大致对应：

| 内部状态 | Lid Mode |
|---|---:|
| Laptop | 1 |
| Flat / Transition | 2 |
| Rear transition 1 | 3 |
| Rear transition 2 | 4 |
| Tablet | 5 |

需要注意：

真正使用时最好不要把某一次实验得到的对应关系完全写死。

建议利用：

- Lid Mode
- 上一次状态
- Gyro 旋转方向
- Orientation
- hysteresis

共同决定状态。

---

# 18. 防止状态抖动

Lid Mode 在临界位置可能出现：

`3 → 4 → 3 → 4`

因此必须加入滞回。

例如：

`Mode 3 → 4`

只有满足：

- Lid Sensor 报告 4
- 持续一定时间
- 或者连续收到数次 4

才正式切换。

推荐：

- debounce：50～200 ms
- 连续确认：2～3 次
- 切换后设置短暂锁定时间

这样 UI 不会在临界角度抖动。

---

# 19. 桌面场景可以做到的效果

当：

`底座基本水平`

或者：

`底座姿态基本不变`

时，当前传感器组合已经足够实现非常好的连续效果。

例如：

- 30°：UI 处于接近闭合状态
- 90°：标准 laptop layout
- 150°：界面开始拉伸/转换
- 180°：flat layout
- 220°：进入反向区域
- 270°：tent / stand layout
- 330°：接近平板
- 360°：tablet layout

在这种情况下，实际体验有望非常接近真正带铰链编码器的设备。

---

# 20. 自由移动场景的限制

如果用户：

- 拿起整台电脑
- 倾斜底座
- 旋转整机
- 在空中改变姿态

则 Orientation 会同时受到整机运动影响。

因此 `EstimatedHingeAngle` 可能出现误差。

建议：

### 高置信度

满足：

- 底座推测静止
- Lid Mode 稳定
- Gyro 与开合轴一致

则：

`Confidence ≈ High`

### 低置信度

满足：

- 多轴同时旋转
- 整机明显运动
- Lid Mode 与姿态结果冲突

则：

- 降低角度更新速度
- 保持上一可信结果
- 只根据 Lid Mode 切换大模式

这样即使无法获得严格角度，UI 也不会严重跳动。

---

# 21. 下一步建议

目前传感器能力已经基本调查完成，可以开始正式实现。

### 第一阶段：实时数据层

建立一个 Windows 原生传感器模块，同时读取：

- Orientation
- Accelerometer
- Gyroscope
- Lid Mode

统一输出时间戳。

### 第二阶段：确认铰链旋转轴

固定底座，只开合屏幕。

观察：

- Gyro X
- Gyro Y
- Gyro Z
- Orientation X/Y/Z

确定哪个轴主要对应屏幕开合。

### 第三阶段：建立连续角度映射

采集大致：

`0 / 30 / 60 / 90 / 120 / 150 / 180 / 210 / 240 / 270 / 300 / 330 / 360°`

的传感器值。

获得：

`Orientation → hinge angle`

的基本转换函数。

### 第四阶段：标定 Lid Mode 阈值

重点寻找：

- `1 → 2`
- `2 → 3`
- `3 → 4`
- `4 → 5`

大约分别发生在多少角度。

不一定需要精确到 1°。

只需要知道它们大致属于某些区间即可。

### 第五阶段：融合算法

最终建立：

`angle = f(orientation, gyro, lidMode, previousState)`

加入：

- complementary filter
- low-pass filter
- hysteresis
- debounce
- 状态机
- confidence

---

# 22. 当前最终判断

目前已经确认：

- 存在真正的 ST LSM6DSOX 六轴 IMU
- 存在独立 LIS2MDL 磁力计
- Windows 可以获得连续姿态信息
- 屏幕运动会反映到 Orientation / Motion 数据
- Orientation 是世界坐标姿态，而不是严格铰链角
- 没有发现可直接使用的第二颗底座 IMU
- 标准 Windows `HingeAngleSensor` 不可用
- Intel `Lid Mode Sensor` 可直接通过 CustomSensor API 读取
- Lid Mode 可以真实产生 `ReadingChanged`
- Lid Mode 至少存在 5 个明显状态
- 一次完整正向翻转会稳定出现：

`1 → 2 → 3 → 4 → 5`

因此：

> **已经具备实现实用级 0～360° 开合姿态估算的基础条件。**

它不会是严格意义上的硬件铰链角编码器，但对于驱动折叠式 UI、模式切换、开合动画等用途，现有传感器能力已经足够值得进入开发阶段。

---

# 23. 建议的最终技术路线

`LSM6DSOX`

→ Continuous Orientation

→ Accelerometer gravity correction

→ Gyroscope motion / direction

→ Intel Lid Mode 1～5

→ HingeEstimator

→ Estimated 0～360° angle

→ UI transition / layout / animation

最终目标不是追求：

`真实铰链角 = 217.381°`

而是得到：

`稳定、连续、低延迟、视觉上正确的 0～360° 开合状态`

对于这个项目来说，这已经足够。
