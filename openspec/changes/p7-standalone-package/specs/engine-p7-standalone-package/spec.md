<!-- Generated from proposal.md; source-sha256: 9f46c0fce74ee10302522f3f730edf5005a2044fc4968d07bbd843e763dfa49d. Do not edit. -->

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
