# P5 第二批验证计划

| 批次 | 用例 | CTest 标签 | 证据与覆盖 |
| --- | --- | --- | --- |
| T44 | `rhi-descriptor-heap-scene-layout` | CPU（无 `gpu`） | 验证含 256 场景槽 + 13 外部附件/环境槽 + 8 Buffer 槽的 Heap 布局规划、步长对齐与超限/溢出拒绝；并用 fixture 加载器分别验证 `RequireProduct` 窗口在“版本不足（1.3 设备）”返回 `ApiVersionUnsupported` 与“1.4 设备缺 Heap 能力”保留具体能力失败错误，且两种失败均释放窗口与初始化资源 |
| T44 | `sample-static-scene-heap-gbuffer` | `gpu` | 真机开启 `VK_VALIDATION_VALIDATE_SYNC=1`，在同一多材质静态场景与动态 `setCamera` 下对照 1.3 Set 与 1.4 Heap 直接 Bindless GBuffer 路径，断言 GBuffer 探针与最终 PNG 一致、`staticSceneUploadCount == 1`、Debug Validation 零错误 |
| T45 | `sample-static-scene-heap-gpu-driven` | `gpu` | 真机开启 `VK_VALIDATION_VALIDATE_SYNC=1`，在同一场景下对照 1.3 Set 与 1.4 Heap 的 GPU-Driven 间接绘制 + 延迟 HDR/IBL + 动态相机（含 180° 零可见与复位）+ 窗口 Resize + 运行期环境替换（`Constant` 捕获旧帧 → 失败回滚保持旧环境 → `Directional` 捕获新帧确认旧/新环境切换生效）+ 容量截断（非溢出严格比集合与像素，溢出各自校验 `visible`、合法子集与无越界），Debug Validation 零错误 |
| T46 | `sample-descriptor-heap-scene-parity` | `gpu` | 通过 `ar_triangle` CLI 端到端运行 Set 与 `--descriptor-heap`（`--static-gpu-driven --hdr-lighting --ibl directional`）同场景对照并验证冲突开关拒绝，Debug Sync Validation 零错误 |

说明：
- 新增用例按要求登记至 `workflow/p0-checks.json` 的 `expected_cases`，真机用例标 `LABELS gpu` 并登记至 `gpu_cases`。
- 既有 92 个用例必须保持全绿、零告警。
