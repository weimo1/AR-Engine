# P6：主程序零参入口、6 通道调试视图、ImGui 面板与四配置性能基准

依据 [实施路线与阶段计划](../../../docs/实施路线与阶段计划.md#毕业交付的五项技术方案)、[调试视图与观察设置](../../../docs/毕业交付/调试视图与观察设置.md)、[四配置性能基准](../../../docs/毕业交付/四配置性能基准.md) 与 [默认展示场景与启动](../../../docs/毕业交付/默认展示场景与启动.md)，本变更在不破坏单向依赖边界（`core ← rhi ← render`，`runtime` 组合）和 Vulkan 1.3 Set / 1.4 Descriptor Heap 等价契约的前提下，完成以下能力：

1. **主程序零参启动与命名收口（`ar_engine`）**：将产品主程序更名为 `ar_engine` 并设为 VS 默认启动项目（清空启动参数），零参数直接进入完整展示室（GBuffer + Bindless + GPU-Driven + HDR + 定向 IBL + 2048 方向光阴影 + 自由相机漫游 + ImGui 面板）；历史 P1–P5 专项测试开关拆至 `TestSupport / ar_sample_harness`，保住既有全部回归。
2. **6 通道调试视图与运行期观察设置**：支持 `Final`、`BaseColor`、`ViewNormal`、`MetallicRoughness`、`MainDepth`（Reverse-Z 按当前相机 `near/far` 线性化）、`ShadowDepth`（非比较采样器 1:1 居中预览，无阴影时不可用）；补齐运行期 `setExposureEV`、`setDebugView` 与具名 `ObservationSettings` 预设（版本、场景引用、相机、曝光、方向光与阴影开关、视图通道）原子读写与失败回滚。
3. **纯 Dear ImGui + Set/Heap 双路径自绘 `OverlayDraw`**：`vcpkg.json` 仅接入纯 `"imgui"`（仅应用层私有链接）；`platform::Window` 收拢单一事件泵并向 UI 转交键/字符/鼠标/滚轮/焦点事件；RHI 自绘 `OverlayDraw` 在 Heap 路径复用既有 Resource Heap（新增 1 个外部字体槽，保持零 `VkDescriptorPool`/`VkDescriptorSet`/`VkPipelineLayout`），并正确处理 sRGB Swapchain 顶点色解码；按 `F1` 显隐面板，截图/隐藏自动化默认关闭 UI。
4. **GPU 时间戳查询与四配置性能基准（Set/Heap × CPU/GPU 剔除）**：RHI 按帧槽提供 `vkCmdResetQueryPool` + `vkCmdWriteTimestamp2` 非阻塞异步回收（收尾 `drainFrames` 收回尾帧，缺能力显式标缺测）；`render::StaticSceneVisibility` 复现 `static_indirect.comp` 局部 AABB 六半空间剔除，直接写入帧槽 `commands`/`counts` 复用同一 `vkCmdDrawIndexedIndirectCount` 消费路径；支持基准 `PresentMode`（解除 `FIFO` 锁帧）与低/中/高三档确定性场景、固定相机扫描轨迹及 JSON/CSV 逐帧与中位数/P95 汇总导出。

非目标：不引入级联阴影（CSM）、完整场景树/Gizmo 编辑器、官方 `imgui_impl_vulkan`/`imgui_impl_glfw` 或 P7 独立安装包脚本。

<!-- AR:CONTRACT capability=engine-p6-debug-ui-benchmark -->
## Purpose

建立零参数桌面主程序 `ar_engine`、6 通道调试视图、Set/Heap 兼容自绘 ImGui 面板、观察预设往返及 Set/Heap × CPU/GPU 剔除四配置基准评测体系。

## ADDED Requirements

### Requirement: 零参数 `ar_engine` 入口与测试驱动解耦

`ar_engine` SHALL 在零参数启动时从 EXE 同级目录加载 `shaders/` 与展示室，默认启用完整延迟 PBR + IBL + 2048 方向光阴影 + 自由相机；仅保留极简自动化/对比覆盖参数，历史 P1–P5 模式参数拆离至 `ar_sample_harness`。

#### Scenario: 零参启动、路径解析与 Set/Heap 展示室等价
- **WHEN** `app-paths-and-showroom-cli` 与 `sample-showroom-launch`、`sample-free-camera-shadows` 校验 `ar_engine` 零模式开关启动与旧开关拒绝
- **THEN** Set/Heap 输出一致、非法旧参数在设备创建前返回退出码 2，全部既有回归保持通过

### Requirement: 平台事件队列、观察预设往返与 ImGui 捕获路由

`platform` SHALL 扩展单一事件泵提供按键、字符、鼠标、滚轮与焦点事件并保持首帧/焦点恢复零跳变；`runtime::ObservationSettings` SHALL 支持原子写入、版本与场景校验、非法预设回滚（保留当前生效设置）。

#### Scenario: 事件状态机、捕获位与观察预设校验
- **WHEN** `platform-input-state` 与 `runtime-observation-settings` 验证 UI 鼠标/键盘捕获屏蔽漫游、失焦/切回视口零跳变及预设保存/重开/损坏回滚
- **THEN** 有效预设完整往返，损坏/错版本/非法字段预设报错并保持原状态不变

### Requirement: 6 通道调试视图、运行期曝光、Set/Heap `OverlayDraw` 与 GPU 时间戳

`render` 与 `rhi` SHALL 支持 6 通道调试视图按需声明 Graph 读取、`setExposureEV` 运行期生效、Set/Heap 双路径 `OverlayDraw`（Heap 零传统描述符对象）与按帧槽非阻塞 GPU 时间戳查询。

#### Scenario: 6 通道视图数值验证、Overlay 绘制与计时回收
- **WHEN** `render-graph-plan`、`rhi-descriptor-heap-scene-layout` 与 `sample-debug-views-and-overlay` 在 Set 与 Heap 路径验证 6 通道像素/方向/深度范围、Overlay 绘制、Resize 恢复与连续多帧 GPU 时间戳回收
- **THEN** 6 通道符合数值参考、关闭阴影时阴影视图不可用、Set/Heap 等价、时间戳单调有效无跨帧错位且 Debug Sync Validation 零错误

### Requirement: CPU AABB 剔除与四配置性能基准（Set/Heap × CPU/GPU 剔除）

`render::StaticSceneVisibility` SHALL 复现 `static_indirect.comp` 局部 AABB 六半空间裁剪；RHI SHALL 支持 CPU 直接写帧槽间接命令/计数并复用 `vkCmdDrawIndexedIndirectCount`、支持基准 `PresentMode` 解除 `FIFO` 锁帧；基准导出 SHALL 输出逐帧与中位数/P95 汇总及完整软硬件元数据。

#### Scenario: CPU/GPU 剔除等价与四配置基准复现
- **WHEN** `render-static-scene-visibility` 与 `app-benchmark-run` 验证旋转/缩放/贴边/零可见与分位统计/帧号联结，`sample-four-config-benchmark-parity` 与 `sample-four-config-benchmark-export` 对照四配置同帧可见 ID 与像素并验证基准 JSON/CSV 导出可解析与汇总可重算
- **THEN** 非溢出场景四配置可见 ID 集合与像素一致、零可见提交 0 绘制、溢出独立标记、导出含有效 CPU/GPU 计时或显式缺测原因且 Debug Sync Validation 零错误
<!-- /AR:CONTRACT -->
