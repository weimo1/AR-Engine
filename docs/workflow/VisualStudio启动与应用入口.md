# Visual Studio 启动与应用入口

主程序与验收入口统一为 **`ar_engine.exe`**（默认启动目标由 [根 CMakeLists.txt](../../CMakeLists.txt) 设置，资产与 Shader 部署由 [应用 CMakeLists.txt](../../examples/triangle/CMakeLists.txt) 维护，入口在 [main.cpp](../../examples/triangle/main.cpp)）。

## 打开与零参数启动（F5 直接运行）

1. 当前本机仓库为 `F:\AR Engine`。Debug 打开 `build/Debug/ArEngine.slnx`（选 `Debug | x64`）；优化版另开 `build/RelWithDebInfo/ArEngine.slnx`（选 `RelWithDebInfo | x64`）。不要在同一个构建目录切换配置。
2. 解决方案按 `App`、`Engine`、`Tests`、`TestSupport` 分组。`App / ar_engine` 是默认启动项目（`DefaultStartup="true"`）；若 VS 缓存了旧选项，右键 `ar_engine` 设为启动项目。
3. **命令参数留空（零参数）直接按 F5**，或直接双击生成的 `ar_engine.exe`：
   - 自动从 EXE 同目录加载 `shaders/` 与 `content/showroom/`；
   - 默认完整启用：展示室场景 + GBuffer + Bindless 材质 + GPU-Driven 间接绘制 + HDR 延迟光照 + 定向 IBL + 2048 方向光阴影 + 自由相机漫游 + `[F1]` ImGui 调试视图/观察预设面板；
   - 交互操作：`F1` 切换调试面板、右键拖动旋转视角、`WASD / QE` 移动、`Shift` 加速、滚轮调速、`Esc` 或点窗口关闭按钮退出。

## 仅用于自动化/对比的极简参数

日常调试与展示不需要任何命令行参数。自动化验收与 A/B 对比时仅支持以下最小覆盖参数：

- `--descriptor-heap`：切换到 Vulkan 1.4 Descriptor Heap 路径（默认 Vulkan 1.3 Descriptor Set）。
- `--scene <JSON> --asset-root <DIR>`：覆盖默认展示室，加载指定场景（须成对传入）。
- `--hidden --frames <N>`：隐藏窗口运行固定帧数后退出（隐藏模式须配 `--frames`）。
- `--screenshot <PNG>` / `--gbuffer-probes <JSON>`：在末帧保存截图或探针（须配 `--frames`）。
- `--output <DIR>`：指定诊断日志输出目录。
- `--benchmark <SPEC_JSON>`：按 JSON 规格运行无 UI/无读回性能基准并导出 `frames.csv` 与 `summary.json`（不能与其他参数混用）。

历史 P1–P5 回归脚本（清屏、旧三角形、单功能测试开关）已独立拆到 `TestSupport / ar_sample_harness`（[sample_harness.cpp](../../tests/triangle/sample_harness.cpp)），不进入产品主程序。

## 单独调试测试 EXE

在 `Tests` 下选择 `ar_core_tests` / `ar_rhi_tests` / `ar_runtime_tests` / `ar_platform_tests` / `ar_render_tests` / `ar_assets_tests` / `ar_app_tests`，设为启动项目即可运行。只跑某个 Catch2 用例时，命令参数填标签如 `"[app-paths-and-showroom-cli]"`。各测试目标的 `AR_TEST_OUTPUT_ROOT` 已由 CMake 自动写入 `VS_DEBUGGER_ENVIRONMENT`。

## 增量构建与恢复

只改已有 C++ 或 GLSL 时在 VS 直接生成 / 按 F5 走增量编译。重新生成解决方案或清理重编时在仓库根执行：

```powershell
python scripts/dev.py fresh -c all
```

只重编 Shader：

```powershell
python scripts/dev.py shaders -c all
```

不带 `-c all` 时默认仅处理 Debug；只处理优化版用 `-c release`。
