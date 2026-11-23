# 架构适度收口：每帧视图解耦与 render/rhi 职责边界归位

依据 [实施路线](../../../docs/实施路线与阶段计划.md#2026-11-22-架构收口与毕设交付路线确认) 与 [模块文档入口](../../../docs/modules/README.md)，在 P5 第一批（T37–T40）完成之后、进入 P5 第二批（同场景 Set/Heap 对照）与 P4b（阴影、多视图、相机漫游）之前，消除两处结构错位：

1. 当前静态场景上传路径把相机 `view`/`projection` 预烘焙进了每个实例的 `clipFromModel` 与视图空间法线矩阵，移动相机必须重传整套几何与贴图，无法支撑自由漫游与多视图/阴影。
2. 当前 [RhiSession.h](../../../engine/rhi/include/rhi/RhiSession.h) 混入测试专用 `verify*` 入口，且 `Presentation.cpp` 内嵌了 GBuffer、延迟光照、IBL 与探针坐标等具体渲染算法编排。

本变更保持单 Vulkan 后端，不引入通用多后端抽象层，也不修改资产格式与既有验证容差。

<!-- AR:CONTRACT capability=engine-render-rhi-boundary-cleanup -->
## Purpose

解耦静态场景数据与每帧相机视图，分离测试专用 RHI 验证入口，并将具体渲染管线规格与 RenderGraph Pass 编排归位到 `render` 模块。

## ADDED Requirements

### Requirement: 每帧相机视图与静态场景解耦

静态场景上传 SHALL 只上传世界空间模型变换、世界空间法线矩阵、局部包围盒、几何与材质数据；每帧相机变换（`view`、`clipFromWorld`、`inverseProjection`、`viewToWorld`）SHALL 通过独立的每帧视图接口更新并按帧槽隔离上传，更新相机不得触发静态网格、贴图或实例 SSBO 重传。

#### Scenario: 动态切换相机不重传静态场景

- **WHEN** `render-frame-view` 验证相机矩阵构造与非法参数拒绝，且 `sample-camera-view-update` 在 `openStaticScene` 后不重传静态场景而多次调用 `setCamera`（包含偏转至全剔除、拉近与复位）
- **THEN** 静态场景上传计数保持为 1，GPU 剔除可见集合、Reverse-Z 深度、视图位置与最终像素随相机动态变化并在复位后与首帧一致，非法相机被显式拒绝，Debug Sync Validation 零错误

### Requirement: 产品主头与测试验证入口分离

测试专用的同步验证入口（`verifyGpuCull`、`verifyDescriptorHeap`、`verifyDescriptorHeapRetirement` 及相关探针结果类型）SHALL 从产品主公开头 `rhi/RhiSession.h` 剥离至独立的验证头 `rhi/RhiVerification.h`，主公开头不得暴露测试专用探针类型。

#### Scenario: 公开头分离与全量回归

- **WHEN** `public-headers` 独立编译 `rhi/RhiSession.h` 与 `rhi/RhiVerification.h`，且全部既有 CPU/GPU 验证用例运行
- **THEN** 主头与验证头均可独立通过编译检查，全部验证测试与既有能力行为保持一致

### Requirement: 渲染管线规格与 Pass 编排归位 render

GBuffer/HDR 附件格式、延迟光照与显示参数、探针采样布局及 `RenderGraph` Pass 声明 SHALL 由 `render` 模块提供；`rhi` SHALL 仅作为设备、资源、绑定（Set/Heap）、Swapchain 帧循环与按声明执行 Dynamic Rendering/Sync2 Pass 的底层执行原语，保持单向依赖 `core ← rhi ← render`。

#### Scenario: 管线归位后的零回归验证

- **WHEN** 既有静态场景、GBuffer、Bindless 材质、GPU 剔除、HDR/SDR、IBL 与 resize 全部真机用例在收口后的 `render → rhi` 边界上执行
- **THEN** 全部像素、探针数值与可见集合维持既有容差，双配置零警告，Debug Sync Validation 零错误
<!-- /AR:CONTRACT -->
