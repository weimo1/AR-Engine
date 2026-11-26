AR Engine — 独立便携运行包说明（Windows x64）
================================================

一、环境前置要求
1. 操作系统：Windows 10 / 11 (x64)。
2. 显卡驱动：支持 Vulkan 1.3 及以上的 GPU 驱动（默认 Descriptor Set 路径）；
   启用 `--descriptor-heap` 需要支持 Vulkan 1.4 与 `VK_EXT_descriptor_heap` 的驱动。
3. 运行库：Microsoft Visual C++ Redistributable 2015–2022/2026 (x64)
   （仅动态依赖 MSVC CRT 与 Windows 系统 DLL，第三方图形/UI/数学库均已静态链接）。

二、目录结构
- ar_engine.exe          主程序（零参数双击即可启动，不依赖源码树或 Visual Studio）
- shaders/               同次构建并经 spirv-val 校验的 13 个产品 SPIR-V 着色器
- content/showroom/      默认材质展示室场景、资产与 LICENSE.txt
- presets/               预置只读观察预设（showroom_default.json、showroom_closeup_metallic.json）
- benchmark/             便携性能基准规格模板（Set / Descriptor Heap）

三、快速启动与交互操作
1. 直接双击 `ar_engine.exe`（默认 Vulkan 1.3 Descriptor Set）：
   - 自动打开默认展示室（GBuffer + Bindless + GPU-Driven 间接绘制 + HDR + 定向 IBL + 2048 方向光阴影）。
   - 如需验证 Vulkan 1.4 Descriptor Heap 路径，在终端执行：`.\ar_engine.exe --descriptor-heap`
2. 键鼠操作：
   - 按住右键拖动：旋转自由相机视角
   - W / A / S / D / Q / E：移动（Shift 加速，滚轮调节移动速度）
   - F1：显示 / 隐藏左上角 `AR Engine Inspector` 调试面板
   - Esc 或点击窗口关闭按钮（X）：安全退出
3. 调试视图与观察预设：
   - 面板支持 6 通道实时切换：Final (ACES HDR)、BaseColor、View Normal、Metallic/Roughness、Main Depth (Reverse-Z)、Shadow Depth (1:1)。
   - 保存预设默认写入同目录 `output/presets/`（若程序目录只读或无权限，自动回退至 `%LOCALAPPDATA%\AR Engine\presets\` 或 `%TEMP%\AR Engine\presets\`；也可用 `AR_ENGINE_OUTPUT_DIR` 或 `--output <DIR>` 指定）。
   - 加载预设优先读取上述可写预设目录，若不存在则自动回退读取包内 `presets/`（例如输入 `showroom_closeup_metallic.json` 点击 Load Preset）。

四、命令行截图与性能基准复现
1. 离线截图对比（Set vs Descriptor Heap）：
   .\ar_engine.exe --hidden --frames 2 --screenshot showroom_set.png
   .\ar_engine.exe --descriptor-heap --hidden --frames 2 --screenshot showroom_heap.png
2. 运行包内便携性能基准（结果自动输出至同目录 `output/benchmark/`，只读目录自动回退用户数据/临时目录）：
   .\ar_engine.exe --benchmark benchmark\showroom_set_gpu.json
   .\ar_engine.exe --benchmark benchmark\showroom_heap_gpu.json
   输出包含逐帧明细 `frames.csv` 与汇总元数据 `summary.json`（含设备名、Vulkan 版本、build_id、中位数与 P95）。
   若本机不支持 Vulkan 1.4 Descriptor Heap，`showroom_heap_gpu.json` 会在 `summary.json` 中显式标记 `skipped_unsupported` 与原因。

五、已知限制
- 本版本聚焦单方向光 2048 正交 Shadow Map（3x3 PCF）、不透明 PBR 延迟渲染与静态场景四配置对比基准；
  不含多级联 CSM、半透明前向通道、音频、物理或完整场景编辑工作台。
- 若需查看详细 Vulkan Validation 诊断日志，请使用源码 Debug 构建配置。
