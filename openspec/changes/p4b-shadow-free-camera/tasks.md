# 实施顺序

- [ ] 1.1 T51 落地 P4b 纯 CPU 契约与数学：`scene::StaticScene` 可选 `directional_light`、`FrameView::viewToWorld` 4×4、`render::DirectionalShadow` 完整光相机与 `orthoRH_ZO` 近远面公式及四象限光源优先级、`PresentationGraph` 阴影 Pass 同步、`platform::WindowInputTracker` 纯 CPU 输入状态机与隐藏窗口接线、`render::FreeCameraController` 零滚转收敛与往返（`render-directional-shadow`、`render-free-camera`、`platform-input-state`、`platform-window-input`）。
- [ ] 1.2 T52 落地 RHI 方向光阴影深度 Pass（Direct + 按 `maxDrawIndirectCount` 分批的独立全场景 Indirect）、`LightingFrame` 双帧槽 240B UBO 延迟光照与 PCF 比较采样，同步打通 1.3 Set 与 1.4 Descriptor Heap（271 图像/10 缓冲/4 采样器槽）并支持 `RuntimeSession::setDirectionalLight` 与 Resize 稳定（更新 `rhi-descriptor-heap-scene-layout`，新增 `sample-directional-shadow-parity`）。
- [ ] 1.3 T53 落地 `RuntimeSession` 输入/相机桥接与 `ar_triangle` `--shadows` / `--shadow-resolution` / `--free-camera` 入口，在默认展示室上完成确定性多步自由相机漫游与阴影 Set/Heap 双路径端到端验证（`sample-free-camera-shadows`）。
- [ ] 1.4 T54 [人工] P4b（展示室 + 方向光阴影 + 自由相机）里程碑验收：用户在本地 RTX 4060 可见窗口下按 F5 进入展示室并分别以 1.3 Set 与 1.4 `--descriptor-heap` 实测实时阴影与自由漫游，确认后进入 P6–P7（ImGui 调试面板、四配置性能基准与独立运行包）。
