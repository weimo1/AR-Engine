# 架构适度收口验证计划

| 批次 | 用例 | CTest 标签 | 证据与覆盖 |
| --- | --- | --- | --- |
| T41 | `render-frame-view` | CPU（无 `gpu`） | 验证 `buildFrameView` 构造 `view`/`clipFromWorld`/`inverseProjection`/`viewToWorld`、旋转正交性、Reverse-Z、GBuffer/非 GBuffer Y 翻转以及非法相机（NaN、零四元数、非法 near/far/fov）拒绝 |
| T41 | `sample-camera-view-update` | `gpu` | 真机开启 `VK_VALIDATION_VALIDATE_SYNC=1`，验证直接与 GPU-Driven 间接静态场景在 `openStaticScene` 后仅上传 1 次（`staticSceneUploadCount == 1`），通过 `setCamera` 偏转至零可见、拉近并复位，核对可见集合、Reverse-Z 深度/视图位置变化、复位像素一致与非法相机拒绝，Debug Validation 零错误 |
| T42a | `public-headers` + 全量回归 | CPU + `gpu` | 验证 `RhiSession.h` 与 `RhiVerification.h` 独立编译通过，原 GPU 剔除与 Descriptor Heap 全部用例通过新验证头保持全绿 |
| T42b | 全量既有渲染回归 | CPU + `gpu` | 复跑 `sample-static-scene-pixels`、`sample-gbuffer-probes`、`sample-bindless-material-probes`、`sample-static-scene-gpu-driven`、`sample-hdr-lighting-probes`、`sample-ibl-probes`、`sample-resize`、`sample-camera-view-update` 及全部 Heap 用例，维持原容差，双配置零告警且 Debug Sync Validation 零错误 |

说明：
- 新增用例登记至 `workflow/p0-checks.json` 的 `expected_cases`；真机用例标 `LABELS gpu` 并登记至 `gpu_cases`。
- 既有 89 个用例必须保持全绿、零告警。
