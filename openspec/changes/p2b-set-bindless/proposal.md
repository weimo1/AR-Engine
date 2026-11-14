# P2b Descriptor Set 资源绑定

P2a 提供了上传和资源退役。本阶段按[实施路线](../../../docs/实施路线与阶段计划.md)实现 A 路径，在 Vulkan 1.3 开发基线上，用材质索引访问纹理和 sampler 数组。本阶段覆盖能力、默认资源、容量、索引和在途安全。Descriptor Heap、资产加载和通用材质系统留在既定后续阶段。

<!-- AR:CONTRACT capability=engine-p2b-set-bindless -->
## Purpose

让渲染调用方通过材质索引选择不同纹理与 sampler，并在同一绑定表上明确默认资源、容量边界和 GPU 在途时的槽位生命周期。

## ADDED Requirements

### Requirement: 绑定能力与容量

RHI SHALL 在显式请求绑定路径时核对所需 Descriptor Indexing 特性和设备限制。超出容量或缺少能力 SHALL 明确失败。每个空槽 SHALL 指向已完成上传和采样布局转换的有效默认资源。INFO SHALL 记录表容量、默认槽和启用能力。

#### Scenario: 设备与表的边界
- **WHEN** `rhi-set-binding-limits` 检查不支持的设备、非法容量和表满，`rhi-set-binding-device` 在真机创建表
- **THEN** 错误包含失败原因，表满不破坏已有映射。Debug 真机创建图形设备时按现有策略启用 Validation，切换 INFO／DEBUG 不改变该状态，表创建及关闭无 Validation 错误

### Requirement: 材质索引采样

绑定路径 SHALL 允许同一次绘制内的不同片元按各自的材质索引选择纹理和 sampler。

#### Scenario: 按索引生成可区分的像素
- **WHEN** `sample-set-binding-pixels` 绘制使用不同纹理、sampler 和默认槽的区域
- **THEN** GPU 读回的区域像素与对应输入一致，Debug 同步 Validation 无错误

从首个槽位持有纹理 view 到最后一个相关槽位解除引用，RHI SHALL 按底层 Image 拒绝 `uploadImage` 和 `readbackImage`，返回 `InvalidState` 并保持布局及提交状态。多个 view 指向同一 Image 时共享这一限制。

### Requirement: 在途绑定与槽位退役

只有已发布句柄 SHALL 可解析为 Shader 索引。释放 SHALL 立即使 CPU 槽位句柄失效。在相关提交完成前，原描述符和所引用的资源 SHALL 保持有效。存在槽位引用时 SHALL 拒绝销毁其 view 或 sampler。完成后 SHALL 恢复默认映射并允许以新代数复用槽位。显式 DEBUG SHALL 能关联槽位、代数、资源、帧及发布等待。

#### Scenario: 多帧期间请求释放
- **WHEN** `rhi-set-binding-retirement` 和 `sample-set-binding-retirement` 在旧映射被两帧引用时请求释放
- **THEN** 旧句柄立即失效，只完成一个相关提交时不得写回或复用，两帧结果保留原映射，全部完成后新句柄可复用同一槽位。Debug 同步 Validation 无错误，显式 DEBUG 可追踪退役到复用
<!-- /AR:CONTRACT -->

长期边界见 [RHI 模块](../../../docs/modules/rhi/README.md)，实施与检查见[技术方案](design.md)和[验证计划](test-plan.md)。
