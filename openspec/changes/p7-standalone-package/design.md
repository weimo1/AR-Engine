# P7：独立运行包与复现技术方案

主责模块为 `examples/triangle` 与 `tests/triangle`，不向 `core / rhi / render / runtime / platform / scene / assets` 引入任何“安装模式”开关，保持单向依赖与 Set/Heap 双路径不变。

## 1. 运行时依赖与目录布局

`ar_engine.exe`（`x64-windows-static-md`）已将 `glfw3 / imgui / nlohmann_json / glm / entt / vma / stb / volk` 全部静态链接，PE 导入表仅含 Windows 系统 DLL 与 MSVC 14.x 动态 CRT（`VCRUNTIME140*.dll / MSVCP140*.dll / api-ms-win-crt-*`），`vulkan-1.dll` 运行期经 `rhi::SystemLoader` 动态加载。

`cmake --install <build-dir> --config RelWithDebInfo --prefix <staging-dir>` 产出布局：

```text
<prefix>/
├── ar_engine.exe
├── README.txt
├── shaders/                     # 13 个产品 SPIR-V（无旧 P1/探针测试着色器）
├── content/showroom/            # scene.json + assets/* + LICENSE.txt
├── presets/                     # showroom_default.json, showroom_closeup_metallic.json
└── benchmark/                   # showroom_set_gpu.json, showroom_heap_gpu.json
```

## 2. 路径解析与只读安全设计

- **只读输入**：`AppPaths` 由 `currentExecutableDirectory()` 派生 `shaders = exeDir/shaders`、`content = exeDir/content`、`presets = exeDir/presets`；显式 `--scene` / `--asset-root` / `--benchmark` 按调用方工作目录解释。
- **可写输出**：移除 `AR_ENGINE_OUTPUT_ROOT` 编译宏。命令行 `--output` 与环境变量 `AR_ENGINE_OUTPUT_DIR` 显式优先且不静默回退；未显式指定时（如用户双击便携包），`defaultOutputDirectoryFor(*exeDir)` 依次探测 `<exeDir>/output` → `%LOCALAPPDATA%\AR Engine` → `%USERPROFILE%\AppData\Local\AR Engine` → `%TEMP%\AR Engine` 并选用首个可写目录（既保证绿色便携目录免触碰系统受控文件夹，又保证只读安装目录或 `AppData` 受限环境自动安全回退）。VS 调试目标通过 `VS_DEBUGGER_ENVIRONMENT` 传入 `AR_ENGINE_OUTPUT_DIR=${CMAKE_BINARY_DIR}/engine-output/$<CONFIG>`；`wmain` 入口在 Windows 启用 `SetConsoleOutputCP(CP_UTF8)` 以无乱码输出中文路径。
- **失败可定位**：启动阶段在创建诊断会话前调用 `verifyWritableDirectory(options->output)`，创建并探针写入失败时直接向 `stderr` 输出展开路径与原因并退出码 1，避免日志目录不可写时静默无信息。
- **预设加载与保存**：保存始终写入可写目录 `output/presets/<file>`；加载优先查 `output/presets/<file>`，不存在时回退只读 `paths.presets/<file>`。
- **便携基准规格**：`BenchmarkRun::loadBenchmarkConfig` 支持 `"output_dir": "$USER_OUTPUT/..."` 展开到 `paths.output`，且内部诊断目录采用带时间戳的唯一 `runId`，支持在同一输出目录下重复运行基准而不冲突。
