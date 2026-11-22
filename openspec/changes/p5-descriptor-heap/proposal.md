# P5 Vulkan 1.4 Descriptor Heap 独立原型

依据[实施路线](../../../docs/实施路线与阶段计划.md)与 [RHI 长期方案](../../../docs/modules/rhi/README.md#vulkan-14-与-descriptor-heap)，保留既有 Vulkan 1.3 Descriptor Set 路径与 T08 设备能力启用代码。本批只建设 P5 第一批独立原型，在支持设备上验证 Heap 布局、写入、绑定、Shader 数值读回与在途槽位退役；同场景 Set/Heap 对照留待本批验收后的第二批，Mesh Shader、硬件光追与 Capture/Replay 不在本轮范围。

<!-- AR:CONTRACT capability=engine-p5-descriptor-heap-prototype -->
## Purpose

在 Vulkan 1.4 支持设备上验证 `VK_EXT_descriptor_heap` 的内存布局、描述符写入、管线绑定、Shader 数值读回、在途退役及缺能力明确拒绝。

## ADDED Requirements

### Requirement: Descriptor Heap 布局规划与能力边界

RHI SHALL 查询完整描述符尺寸、对齐、保留区与 push data 上限，解析所需设备入口（含 `vkCmdPushDataEXT`），按对齐和保留区规则规划 Resource Heap 与 Sampler Heap 布局，并显式拒绝缺能力、零限制、整数溢出或超容量请求，不静默回退到 Descriptor Set。

#### Scenario: 布局规划、容量边界与缺能力拒绝

- **WHEN** `rhi-descriptor-heap-layout` 提交有效限制、超上限容量、溢出输入以及缺少 API 1.4、扩展或设备入口的 fixture 配置
- **THEN** 有效配置输出满足对齐和保留区约束的步长与偏移，非法或不支持配置返回明确 `core::Error` 且无残留资源

### Requirement: Descriptor Heap 写入、绑定与 Shader 数值读回

支持 `VK_EXT_descriptor_heap` 的设备 SHALL 通过 VMA 分配满足对齐的 Resource Heap 与 Sampler Heap，写入默认槽与资源/sampler 描述符，使用 `VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT` 和 set/binding 映射绑定 Heap 与 push data，并让 Shader 读取指定纹理槽与 sampler 输出可验证数值。

#### Scenario: 真机多槽位采样数值读回

- **WHEN** `rhi-descriptor-heap-readback` 在真实 Vulkan 1.4 Heap 设备上写入默认纹理、测试纹理及 nearest/linear 两个 sampler，并按不同槽位索引分派 Compute Shader 读回
- **THEN** 默认槽、不同纹理槽与不同 sampler 的读回数值与预期逐通道一致，Debug Validation 无错误

### Requirement: Descriptor Heap 在途槽位退役与有序释放

释放 Heap 槽位句柄 SHALL 立即拒绝后续 CPU 访问；关联 GPU 提交完成前 SHALL 保留槽位字节与底层资源引用并拒绝销毁或覆盖底层图像；提交完成后 SHALL 覆写默认槽描述符、解除引用并允许以新代数复用，关闭时按序收敛并释放所有 Heap 缓冲与管线。

#### Scenario: 在途退役与资源保护

- **WHEN** `rhi-descriptor-heap-retirement` 在提交完成前后检查退役句柄、底层图像销毁/传输保护及完成后槽位复用与会话关闭
- **THEN** 完成前旧映射与资源保护保持有效，完成后覆写默认值并可复用新代数槽位，关闭无残留且 Debug Validation 无错误
<!-- /AR:CONTRACT -->
