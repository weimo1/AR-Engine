# 实施顺序

- [x] 1.1 改造 `AppPaths`、`main.cpp` 与 `BenchmarkRun`：移除 `AR_ENGINE_OUTPUT_ROOT` 编译宏，新增 `defaultOutputDirectoryFor()`（便携优先回退链）、`verifyWritableDirectory()`、Windows 控制台 UTF-8、`AppPaths::presets` 回退加载及 `$USER_OUTPUT/` 基准输出展开与唯一诊断 `runId`，补齐 stdout `build_id` / `output_dir` 输出（`app-paths-and-showroom-cli`、`app-benchmark-run`）。
- [x] 1.2 新增随包交付文件（`examples/triangle/content/package/{presets,benchmark,README.txt}`）与 `examples/triangle/CMakeLists.txt` `install(...)` 组包规则，更新 `docs/workflow/VisualStudio启动与应用入口.md`。
- [x] 1.3 新增 `tests/triangle/check_installed_app.py` 并登记 CTest `sample-installed-package`（`gpu`）及 `workflow/p0-checks.json`，在含中文与空格的独立只读安装前缀验证双路径出图、只读输出隔离、缺失资产/不可写报错及基准复现。
- [x] 1.4 跑通 P7 双配置完整门禁（Debug + RelWithDebInfo 111/111 零警告），产出 `build/p7/package/AR-Engine` 正式优化版便携交付包，经 DSH 独立评审 `e997caab` PASS 后更新 `TASK.md` 并本地提交。
