# P7：独立运行包与毕业交付复现

依据 [独立运行包与复现](../../../docs/毕业交付/独立运行包与复现.md) 与 [实施路线与阶段计划](../../../docs/实施路线与阶段计划.md#毕业交付的五项技术方案)，本变更将 `ar_engine` 收口为可脱离源码树与构建树独立运行的 Windows x64 绿色便携包，支持双击启动展示室、从只读程序目录安全写入用户数据目录、保存/重载观察预设及复现四配置性能基准：

1. **移除构建树硬编码，建立便携输出与只读预设路径**：移除 `AR_ENGINE_OUTPUT_ROOT` 编译宏；`AppPaths` 新增 `presets`（EXE 相邻只读预置预设目录）、`defaultOutputDirectoryFor()`（优先顺序：`--output` > `AR_ENGINE_OUTPUT_DIR` > 可写同目录 `<exeDir>/output` > `%LOCALAPPDATA%\AR Engine` > `%USERPROFILE%\AppData\Local\AR Engine` > `%TEMP%\AR Engine`，在 Windows 控制台启用 UTF-8 输出避免中文路径乱码）与 `verifyWritableDirectory()`（不可写时向 stderr 报出具体路径并非零退出）；`DebugPanel` 保存预设写可写输出目录，加载预设优先读用户目录并回退包内预置预设；`BenchmarkRun` 支持 `$USER_OUTPUT/` 输出前缀与唯一诊断 `runId`，允许在只读安装目录内重复执行基准。
2. **CMake 标准 `install(...)` 组包规则与交付随包资产**：在 `examples/triangle/CMakeLists.txt` 增加 `install(...)` 规则，经 `cmake --install <build> --config RelWithDebInfo --prefix <dir>` 输出完整独立运行目录：`ar_engine.exe`（零第三方 DLL 依赖）、13 个产品 SPIR-V 着色器、`content/showroom/`（含 `LICENSE.txt`）、`presets/`（默认与特写观察预设）、`benchmark/`（Set/Heap 便携基准规格）与 `README.txt`（前置要求、操作与论文复现指南）。
3. **独立安装目录端到端验收**：新增 GPU 验收 `sample-installed-package`（`tests/triangle/check_installed_app.py`），真实调用 `cmake --install` 安装到含中文与空格的独立目录，从隔离工作目录校验只读安装目录写出用户数据、Set/Heap 像素等价、缺失 Shader/场景诊断、不可写输出目录报错与包内基准复现导出；CPU 单测 `app-paths-and-showroom-cli` 覆盖用户目录解析、可写校验与包内预置预设往返。

非目标：不引入 MSI/NSIS 安装器、不向引擎核心模块新增“安装模式”分支、不改动既有 110 项回归契约。

<!-- AR:CONTRACT capability=engine-p7-standalone-package -->
## Purpose

建立可脱离源码树与构建树运行的 Windows x64 独立便携目录包，验证只读程序目录、中文/空格路径、预设往返及四配置基准复现闭环。

## ADDED Requirements

### Requirement: 便携路径解析与可写用户输出隔离

`ar_engine` SHALL 从当前可执行文件目录定位只读资源（`shaders/`、`content/`、`presets/`），并按 `--output` > `AR_ENGINE_OUTPUT_DIR` > 便携优先自动回退链（`<exeDir>/output` > `%LOCALAPPDATA%\AR Engine` > `%USERPROFILE%\AppData\Local\AR Engine` > `%TEMP%\AR Engine`）解析可写输出，不得依赖硬编码源码或构建树路径；输出目录不可写或只读输入缺失时 SHALL 向 stderr 报告具体路径并以非零退出。

#### Scenario: 用户目录解析、可写校验与包内预置预设校验
- **WHEN** `app-paths-and-showroom-cli` 验证环境变量优先级、可写与不可写目录判定及包内 `presets/*.json` 经 `loadObservationSettings` 校验
- **THEN** 路径解析脱离构建树、不可写目标返回明确错误、全部预置预设通过版本与场景校验

### Requirement: CMake 安装独立运行包与脱源码端到端复现

`cmake --install` SHALL 组装包含 `ar_engine.exe`、13 个产品 SPIR-V、`content/showroom/`、`presets/`、`benchmark/` 与 `README.txt` 的自包含便携目录；安装目录设为只读且位于含中文和空格路径时，从任意工作目录启动仍可正常渲染、对比 Set/Heap、识别缺失资源并复现基准导出。

#### Scenario: 独立中文空格只读安装目录双路径与基准复现
- **WHEN** `sample-installed-package` 执行 `cmake --install` 到含中文与空格的临时前缀并从独立工作目录调用安装版 `ar_engine.exe`
- **THEN** 资源加载严格指向安装副本、只读状态下默认日志与基准输出写入用户目录、Set/Heap PNG 逐字节一致、缺失资源与不可写输出清晰报错且 Debug Sync Validation 零错误
<!-- /AR:CONTRACT -->
