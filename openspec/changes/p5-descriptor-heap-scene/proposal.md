# P5 第二批：同场景 Descriptor Set 与 Descriptor Heap 绑定切换及等价对照

依据 [P5 第一批方案](../p5-descriptor-heap/design.md#后续第二批演进边界) 与 [RHI 长期方案](../../../docs/modules/rhi/README.md#vulkan-14-与-descriptor-heap)，第一批（T37–T40）已在独立 Compute 探针上完成 Vulkan 1.4 `VK_EXT_descriptor_heap` 的布局、采样读回与在途退役验证；架构收口（T41–T43）已将每帧 `FrameView` 与 `render` 管线规格归位。

本批将同一 `DescriptorHeapTable` 接入真实静态场景、GPU-Driven 间接绘制、延迟直接光照/IBL 与 SDR 显示全链路，复用既有 GLSL/SPIR-V 着色器，在同一场景、同一相机与同一材质下完成 Vulkan 1.3 Descriptor Set 与 Vulkan 1.4 Descriptor Heap 两条路径的逐像素/探针对照。不删除 1.3 Set 基线，不支持 1.4/Heap 时显式拒绝、保留具体失败原因、不静默回退。

<!-- AR:CONTRACT capability=engine-rhi-descriptor-heap-scene -->
## Purpose

在静态场景 GBuffer、GPU-Driven 间接裁剪/绘制及延迟光照/IBL 全链路接入 Vulkan 1.4 `VK_EXT_descriptor_heap`，支持在 `RequireProduct`（1.4 Heap）与 `Development`（1.3 Set）之间切换并在同一场景下验证像素、探针与可见性等价。

## ADDED Requirements

### Requirement: 静态场景 Bindless GBuffer 支持 Descriptor Heap 路径

`DescriptorHeapTable` SHALL 扩展支持 `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER`、场景纹理之外的内部附件/环境槽区以及多固定 Sampler 槽；`StaticSceneDraw` 与 `Presentation` SHALL 在 `enableDescriptorHeap` 下使用 `VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT`、`layout = VK_NULL_HANDLE`、`VkShaderDescriptorSetAndBindingMappingInfoEXT` 与 `vkCmdPushDataEXT` 复用既有 Bindless GBuffer 着色器，不创建 `VkDescriptorPool`/`VkDescriptorSet`。

#### Scenario: 同场景直接 Bindless GBuffer Set 与 Heap 对照一致

- **WHEN** `sample-static-scene-heap-gbuffer` 在同一确定性多材质静态场景与 `setCamera` 更新下分别运行 1.3 Set 与 1.4 Heap 路径
- **THEN** 两条路径的 GBuffer 探针（`baseColor`、`normal`、`material`、`depth`、`viewPosition`）与最终 PNG 输出一致，`staticSceneUploadCount == 1`，Debug `VK_VALIDATION_VALIDATE_SYNC=1` 零错误

### Requirement: RequireProduct 窗口呈现与失败映射

`QueryMode::RequireProduct` SHALL 允许携带窗口呈现，并在启动失败时区分版本不足（`ApiVersionUnsupported`）与 1.4 设备缺 Heap 扩展/特性/入口的能力失败，且所有失败分支均 SHALL 释放窗口与初始化资源。

#### Scenario: Heap 场景布局与 RequireProduct 窗口失败清理

- **WHEN** `rhi-descriptor-heap-scene-layout` 校验含内部附件槽与缓冲槽的 Heap 布局规划，并以 fixture 加载器分别触发版本不足（1.3 设备）与 1.4 设备缺 Heap 能力失败
- **THEN** 布局与超限拒绝符合分区约定，两类失败分别返回 `ApiVersionUnsupported` 与具体能力错误且窗口和初始化资源均完全释放

### Requirement: GPU-Driven 间接绘制与延迟光照/IBL 全链路支持 Descriptor Heap

`StaticSceneIndirect` 与 `PresentationPasses` SHALL 在 `enableDescriptorHeap` 下按逐阶段完整映射表将实例/材质/双帧槽间接命令/计数/每帧视图 UBO 及双帧槽 GBuffer/Depth/HDR/IBL 附件映射至 Resource Heap 与 Sampler Heap（固定 sampler 须显式设置 `samplerHeapIndexStride = 0`、`samplerHeapArrayStride = 0`），复用既有 `static_indirect.*`、`deferred_direct/ibl.frag`、`display_sdr.frag` SPIR-V。内部附件槽与环境槽更新（Resize 与 `publishEnvironment`）SHALL 遵守“先就绪候选资源 → 等待所有引用旧描述符的提交完成 → 覆写并 flush 描述符 → 再释放旧资源”的安全更新协议。

#### Scenario: GPU-Driven + 延迟光照/IBL + Resize + 环境替换的 Set/Heap 等价验证

- **WHEN** `sample-static-scene-heap-gpu-driven` 在同一场景下以 GPU-Driven 间接绘制 + 延迟 HDR/IBL + `setCamera`（含零可见与复位）+ 窗口 Resize + 运行期环境替换（`Constant` → `Directional` 含回滚验证）+ 容量截断分别运行 1.3 Set 与 1.4 Heap
- **THEN** 非溢出场景的可见计数、`drawnIds`、GBuffer/HDR/IBL 探针及最终 SDR 像素一致；容量截断溢出场景两侧各自满足 `visible` 计数正确、`drawnIds.size() == capacity`、所绘 ID 均为合法可见子集且无越界；Resize 与环境替换前后旧帧使用旧环境、新帧使用新环境且无过期描述符，Debug `VK_VALIDATION_VALIDATE_SYNC=1` 零错误

### Requirement: 示例应用提供 `--descriptor-heap` 开关并显式拒绝非法组合

`ar_triangle` SHALL 提供 `--descriptor-heap` 开关以 Vulkan 1.4 `RequireProduct` 模式运行静态场景、GPU-Driven 与 HDR/IBL，且与 `--set-binding` 互斥、不支持时不静默回退。

#### Scenario: 端到端 CLI 同场景 Set 与 Heap 对照

- **WHEN** `sample-descriptor-heap-scene-parity` 通过 `ar_triangle` 在同一场景上分别运行 Set 与 `--descriptor-heap`（含 `--static-gpu-driven --hdr-lighting --ibl directional`）并测试冲突参数拒绝
- **THEN** 两组渲染输出的剔除集合、探针数值与最终 PNG 一致，冲突或不支持请求返回明确失败，Debug Sync Validation 零错误
<!-- /AR:CONTRACT -->
