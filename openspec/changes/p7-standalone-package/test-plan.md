# P7 验证计划

| 切片 | 用例 | CTest 标签 | 证据与覆盖 |
| --- | --- | --- | --- |
| 1.1 | `app-paths-and-showroom-cli`（更新） | CPU | 验证 `resolveDefaultUserOutputDirectory` 与 `selectDefaultOutputDirectory` 回退链（显式 `AR_ENGINE_OUTPUT_DIR` 无回退；未指定时 `<exeDir>/output` → `LOCALAPPDATA` → `USERPROFILE` → `TEMP`）、`AppPaths::presets`、`verifyWritableDirectory()` 成功与文件阻挡失败，以及包内预置 `presets/*.json` 经 `loadObservationSettings` 往返通过 |
| 1.1 | `app-benchmark-run`（更新） | CPU | 验证 `"output_dir": "$USER_OUTPUT/..."` 展开到用户可写输出根及非法前缀拒绝 |
| 1.3 | `sample-installed-package` | `gpu` | 真实调用 `cmake --install` 安装到含中文与空格的临时独立目录；核对 PE 无第三方 DLL 导入、13 个产品 SPIR-V 与展示室/预设/基准/README 完整；从隔离 CWD 启动安装版 `ar_engine.exe`（仅设 `AR_ENGINE_OUTPUT_DIR`，不传 `--output`），核对资源指向安装副本、只读安装目录下日志正常写出、Set/Heap PNG 逐字节一致、缺失 Shader/场景与不可写输出清晰报错、包内 `--benchmark` 复现导出有效且 0 条 `rhi.validation.error` |

说明：
- 新增 1 项 GPU 用例 `sample-installed-package` 登记到 `workflow/p0-checks.json` 的 `expected_cases`（111 项）与 `gpu_cases`（37 项）。
- 双配置干净重编零警告、111/111 全过，既有 P0–P6 用例零退化。
