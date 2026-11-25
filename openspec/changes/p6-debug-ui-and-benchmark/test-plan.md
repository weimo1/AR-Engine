# P6 验证计划

| 切片 | 用例 | CTest 标签 | 证据与覆盖 |
| --- | --- | --- | --- |
| 1.1 | `app-paths-and-showroom-cli`（更新） | CPU | 验证 `ar_engine` 零参数启动默认加载同级 `shaders/` 与 `content/showroom/`、合法最小自动化覆盖项与 `--benchmark SPEC_JSON` 解析、以及 P1–P5 旧模式参数拒绝（退出码 2） |
| 1.1 | `sample-showroom-launch`（更新） | `gpu` | 在独立工作目录用无模式开关启动 `ar_engine`（1.3 Set 与 1.4 `--descriptor-heap`），验证 14 实例展示室默认阴影光照渲染、无黑边、Set/Heap 像素一致与 0 验证错误 |
| 1.2 | `platform-input-state`（更新） | CPU | 验证单一事件泵下 UI 鼠标/键盘捕获屏蔽自由相机漫游、焦点丢失与从 UI 切回 3D 视口首帧零跳变 |
| 1.2 | `runtime-observation-settings` | CPU | 验证 JSON v1 观察预设（相机、曝光、方向光含 `cast_shadows`、6 通道视图）原子保存、往返加载、损坏/错版本/非法字段/不匹配场景拒绝与零副作用回滚 |
| 1.3 | `render-presentation-spec`、`render-graph-plan`（更新） | CPU | 验证 20B `DisplayPushParams` 打包、6 通道 Display 静态绑定附件转换、`ShadowDepth` 无阴影拒绝、`Overlay` Pass `discard=false` 与 `IndirectProducer`（`None/Cpu/GpuCompute`）+ `readIndirectCounts` 屏障规划 |
| 1.3 | `rhi-descriptor-heap-scene-layout`（更新） | CPU | 验证 272 图像槽（含 `kHeapOverlayFontImage = 271`）+ 10 缓冲槽 + 4 采样器槽布局规划与越界保护 |
| 1.3 | `sample-debug-views-and-overlay` | `gpu` | 真机开启 `VK_VALIDATION_VALIDATE_SYNC=1`，验证 Set 与 Heap 双路径下 6 通道调试视图数值与语义、运行期 `setExposureEV`、观察预设保存/恢复逐字节一致、`OverlayDraw` 混合绘制、运行期关阴影回退 `Final` 继续渲染、多帧 GPU 时间戳单调回收与 0 验证错误 |
| 1.4 | `render-static-scene-visibility` | CPU | 验证 `cullStaticSceneCpu` 在旋转、非均匀缩放、贴边、宽高比变化、零可见与容量截断下的六半空间裁剪精确性 |
| 1.4 | `sample-four-config-benchmark-parity` | `gpu` | 真机开启 `VK_VALIDATION_VALIDATE_SYNC=1`，在展示室对照 `Set+CPU / Set+GPU / Heap+CPU / Heap+GPU` 全可见（14）、部分剔除、零可见（0 绘制）与容量溢出（5/14）四场景，断言四配置 `drawnIds` 与 PNG 逐字节一致、帧号联结 CPU/GPU 计时精确、0 验证错误 |
| 1.5 | `app-benchmark-run` | CPU | 验证 `BenchmarkRun` 规格解析/校验、相机扫描轨迹单位四元数、中位数/P95 分位计算、按 `frameNumber` 严格联结（缺测不跨帧混用）、溢出样本隔离及 CSV/JSON 导出往返 |
| 1.5 | `sample-four-config-benchmark-export` | `gpu` | 在 128 实例确定性展示室材质网格上依次调用 `ar_engine --benchmark` 跑完四配置，校验可见性跨配置一致、独立从 `frames.csv` 重算中位数与 P95 一致、0 验证错误；同脚本 `--suite` 用于 RTX 4060 `RelWithDebInfo` 低/中/高三档（256 / 4,096 / 16,384）实测汇总 |

说明：
- 全部 110 项用例（含 36 项 `gpu` 用例）均登记至 `workflow/p0-checks.json` 的 Debug 与 RelWithDebInfo 清单。
- 双配置干净构建须零警告且 110/110 全绿，既有 P1–P5 回归经 `ar_sample_harness` 保持零退化。
