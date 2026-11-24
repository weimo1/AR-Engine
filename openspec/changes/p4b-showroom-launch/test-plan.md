# 默认展示场景与启动验证计划

| 批次 | 用例 | CTest 标签 | 证据与覆盖 |
| --- | --- | --- | --- |
| T49 | `app-paths-and-showroom-cli` | CPU（无 `gpu`） | 验证 `resolveAppPaths`（基于 EXE 目录定位 `shaders/` 与 `content/`、显式 `--output` 覆盖、缺失目录诊断）；验证 `parseLaunchOptions` 覆盖 `--showroom` 默认展开、与 `--descriptor-heap` 组合、与 `--static-scene`/`--asset-root`/`--clear-only`/`--set-binding`/`--gpu-driven` 冲突拒绝；验证源码与部署目录文本一致并通过 `StaticSceneSession` + `packStaticSceneDrawData` 加载部署后的展示室 `scene.json` 核对实例数、ID 唯一性、共享网格复用及粗糙度/金属度递增顺序 |
| T50 | `sample-showroom-launch` | `gpu` | 真机开启 `VK_VALIDATION_VALIDATE_SYNC=1`，从非构建根的独立工作目录启动 `ar_triangle --showroom --hidden --frames 2`，分别运行 1.3 Set 与 1.4 `--descriptor-heap`，断言两路 PNG 与 GBuffer/HDR 探针一致、画面非空白且体现多材质差异、日志含实际场景/资产根/绑定路径、Debug Sync Validation 零错误；同时验证显式 `--static-scene` 覆盖、缺失场景/贴图报告展开路径并干净退出、冲突参数 `--showroom --static-scene` 返回退出码 2，并核对构建部署目录中的展示室资源与源码一致 |

说明：
- 新增用例按约定登记到 `workflow/p0-checks.json` 的 `expected_cases`，`sample-showroom-launch` 标 `LABELS gpu` 并登记到 `gpu_cases`。
- 既有全部基线用例（含 `shader-build` 及 `ar_triangle` 既有全部 CLI 脚本）必须保持全绿、双配置零警告。
