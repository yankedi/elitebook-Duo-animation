# 参考项目与许可证归属

`src\` 下的代码是独立实现，**没有复制**任何参考项目的源码。参考项目保持只读。

| 参考目录 | 许可证 | 本项目中如何使用 |
|---|---|---|
| `Duo-animation` | **无 LICENSE 文件**（默认保留所有权利） | 仅作算法/思路参考。未复制任何代码或着色器文本。 |
| `duo-open` | MIT (Copyright (c) 2026 marcoazeem) | 仅作架构参考（overlay 生命周期、capture 流程）。未复制代码。 |
| `iphone-duo` | MIT（代码与 SVG 素材）；Apple 模型/壁纸不受 MIT 覆盖 | **世界锚定窗口模型**（眼睛固定在身体坐标系、射线打到不动的平面、`mix(screenUv, planeUv, parallax)`）。未复制代码。 |
| `iphone-duo-animation` | MIT (Copyright (c) 2026 Akash T) | 仅作视觉目标参考（折叠几何、曲面过渡）。未复制代码。 |
| `lid-plane` (jh3y/lid-plane) | **GPL-3.0-or-later** | **仅作策略与数学形式参考**：激活角语义、角度差驱动、`DisplaySafetyGate`、以及**投影数学本身**（`Renderer.swift` 的 `eye/physical/t/hit` 射线-平面求交、按模糊 3σ 羽化的画面边界、画面之外的虚空底色）。GPL 源码**未复制、未改写、未链接**；HLSL 实现、常量缓冲、采样方式均为本项目自行编写。 |

## 从参考实现中提炼、并在本项目中重新实现的概念

这些是**思想**而非代码，且已按 Windows / D3D11 的语境重新设计：

1. **虚拟相机 + 射线求交**（来源：`Duo-animation` / `duo-open` 的 AGSL 着色器注释与结构）
   - 内容位于参考平面；玻璃绕铰链线旋转；从固定视点发出射线，穿过玻璃像素，延伸到参考平面求交；用交点 UV 采样内容。
2. **间隙比例模糊**（gap-proportional blur）
   - 模糊半径与「玻璃到参考平面的距离」成正比。
3. **散射导致的变暗**（darkening proportional to scattering）
   - 衰减系数与模糊半径线性相关；整个卷积核落在内容之外时输出黑色。
4. **Vogel 圆盘采样 + 每像素旋转**
   - 用作低成本 disk blur，抖动避免 banding。
5. **on-change 传感器 + overlay 生命周期**
   - 运动时显示 overlay，稳定后隐藏，避免长期用透明顶层窗口覆盖桌面。

6. **激活角 + 角度差驱动**（来源：`lid-plane` 的 README 与 `EffectDefaults` / `MotionPolicy`）   - 效果不以「绝对屏幕角」为参数，而是以「低于激活角多少」为参数：
     `delta = 激活角 - 屏幕角`。激活角以上桌面完全不处理（正常使用），
     低于激活角才开始出现效果，delta 越大越强。
   - 效果语义：内容保持激活角，物理屏幕绕它倾斜（`holds your desktop at an
     apparent fixed angle`）。
   - 模糊标定：半径 ∝ `sin(delta)`，按屏高（每 1000 px）归一化，靠近铰链处保持清晰。
   - 以上仅为**策略与数学形式**；HLSL 实现、常量缓冲布局、采样方式均为本项目自行编写。
7. **安全门 / fail-closed 生命周期**（来源：`lid-plane` 的 `DisplaySafetyGate` 与其 `update()` 中的
   `lidClosed / builtInAvailable / sensorAvailable` 判定）
   - 合盖（ACPI 盖子开关或角度阈值，带迟滞）、显示器电源关闭、duplication 失效、
     传感器超过 1 s 无读数 → **暂停效果并移除 overlay**，而不是让它留在屏幕上；
   - 全部恢复后需连续稳定 0.5 s 才允许重新抓屏（`recoveryDelay`）；
   - 没有有效帧就不显示 overlay。
   - Windows 侧实现完全不同：`RegisterPowerSettingNotification` +
     `GUID_CONSOLE_DISPLAY_STATE` / `GUID_LIDSWITCH_STATE_CHANGE`、`WM_DISPLAYCHANGE`、
     `IDXGIOutputDuplication` 的失效检测；`DisplaySafetyGate` 为纯逻辑类并带 `--selftest` 用例。
8. **世界锚定窗口模型**（来源：`iphone-duo` 的 `screen-material.ts` 片元着色器）
   - 眼睛固定在**身体坐标系**（笔记本底座/房间），不随屏幕转动；
   - 内容是一个**不动的平面**；逐像素从眼睛发出射线，穿过**转动中的**屏幕点，
     与那个平面求交，用交点采样画面；
   - 屏幕因此只是一扇"窗户"：屏幕动，画面留在它的虚拟位置上；
   - `parallax` 在"贴屏"与"世界锚定"之间插值。
   - 关键换算：眼距必须用**真实距离**（mm），并用面板物理宽度（EDID）换算像素；
     参考项目里 `450mm/70mm` 那个比例放到笔记本上等于把眼睛放到 1.9 m 外，视差会消失。
   - 本项目实现为 HLSL + D3D11，几何（铰链在底边、1D 倾斜）与参考的 3D 折叠模型不同。

9. **玻璃材质线索**（**本项目自行设计，不来自任何参考项目**）
   - 色散（红/蓝反向径向偏移）、菲涅尔式反射光晕、扫动高光带、边缘高光、散射去饱和、
     带下限的衰减：依据真实玻璃的光学行为（折射率差、菲涅尔反射、散射褪色）设计。
   - 参考项目只有「模糊 + 压暗」，观感偏塑料；这些线索是为"玻璃感"新增的，
     参数集中在 `FoldEffectParameters` 并提供 `--glass=frost|clear|plain` 预设与
     `--glass-preview=DEG` 预览模式。

以上概念在 `src\` 中的 Windows 实现（第二阶段）会以 HLSL 重新编写，不会复用 AGSL/Metal 源码。

## 本项目的移植性判断

| 参考内容 | 处理方式 |
|---|---|
| AGSL / Metal 着色器源码 | **只作算法参考**，第二阶段用 HLSL 重写 |
| Android `graphicsLayer` / `RuntimeShader` 宿主代码 | 不可移植（平台 API 不同） |
| Android `SensorManager` / rotation-vector 逻辑 | **算法参考**：零位标定 + 陀螺预测 + 慢速基线回中的思路可借鉴 |
| Android 截图 / AccessibilityService overlay | **架构参考**：capture → overlay → 运动结束销毁的流程；Windows 侧用 DXGI + 顶层分层窗口 |
| Web/Three.js 折叠几何与材质 | 仅作视觉目标参考 |
