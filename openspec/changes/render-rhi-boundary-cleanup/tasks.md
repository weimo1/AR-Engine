# 实施顺序

- [x] 1.1 T41 实现每帧相机视图 `FrameView` 与静态场景上传解耦（`render-frame-view`、`sample-camera-view-update`），移动/旋转相机不重传静态网格、贴图与 SSBO。
- [x] 1.2 T42a 将测试专用 `verify*` 入口及探针结果类型从 `rhi/RhiSession.h` 剥离至 `rhi/RhiVerification.h`，保持全量回归通过。
- [x] 1.3 T42b 将 GBuffer/延迟光照/IBL 管线规格、探针解码与 `RenderGraph` Pass 编排归位 `render`，将 `rhi` 收窄为设备、资源、绑定与执行原语。
- [x] 1.4 T43 [人工] 架构收口里程碑验收，确认后进入 P5 第二批（同场景 Set/Heap 对照）与 P4b（方向光阴影与相机漫游）。
