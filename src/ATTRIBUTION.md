# 参考项目与许可证归属

`src\` 下的代码是独立实现，**没有复制**任何参考项目的源码。参考项目保持只读。

| 参考目录 | 许可证 | 本项目中如何使用 |
|---|---|---|
| `Duo-animation` | **无 LICENSE 文件**（默认保留所有权利） | 仅作算法/思路参考。未复制任何代码或着色器文本。 |
| `duo-open` | MIT (Copyright (c) 2026 marcoazeem) | 仅作架构参考（overlay 生命周期、capture 流程）。未复制代码。 |
| `iphone-duo` | MIT（代码与 SVG 素材）；Apple 模型/壁纸不受 MIT 覆盖 | 仅作视觉目标参考（投影/blur/darkening 的观感）。未复制代码或素材。 |
| `iphone-duo-animation` | MIT (Copyright (c) 2026 Akash T) | 仅作视觉目标参考（折叠几何、曲面过渡）。未复制代码。 |

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

以上概念在 `src\` 中的 Windows 实现（第二阶段）会以 HLSL 重新编写，不会复用 AGSL/Metal 源码。

## 本项目的移植性判断

| 参考内容 | 处理方式 |
|---|---|
| AGSL / Metal 着色器源码 | **只作算法参考**，第二阶段用 HLSL 重写 |
| Android `graphicsLayer` / `RuntimeShader` 宿主代码 | 不可移植（平台 API 不同） |
| Android `SensorManager` / rotation-vector 逻辑 | **算法参考**：零位标定 + 陀螺预测 + 慢速基线回中的思路可借鉴 |
| Android 截图 / AccessibilityService overlay | **架构参考**：capture → overlay → 运动结束销毁的流程；Windows 侧用 DXGI + 顶层分层窗口 |
| Web/Three.js 折叠几何与材质 | 仅作视觉目标参考 |
