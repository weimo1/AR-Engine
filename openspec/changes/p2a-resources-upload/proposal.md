# P2a 资源与上传

按 [阶段路线](../../../docs/实施路线与阶段计划.md) 和 [VMA ADR](../../../docs/adr/0001-Vulkan内存分配采用VMA.md) 接入 Buffer、2D Image/ImageView、上传、完成查询及资源退役。Vulkan 1.3 为开发基线。Bindless、Descriptor Heap 写入和资产加载由后续阶段处理。

<!-- AR:CONTRACT capability=engine-p2a-resources-upload -->
## Purpose

让最小 RHI 调用方创建、上传和释放 GPU 资源，并以数值读回验证结果。

## ADDED Requirements

### Requirement: 资源分配

RHI SHALL 使用设备级 VMA allocator 分配 Buffer 和 Image。公开接口 SHALL 使用类型化句柄，并拒绝非法描述或失效句柄。ImageView SHALL 在关联 Image 之前释放。

#### Scenario: 资源创建与关闭
- **WHEN** 运行 `rhi-vma-buffer` 和 `rhi-vma-image-view`
- **THEN** 真机资源创建和释放通过，host-visible Buffer 写读一致，非法输入和错误释放顺序得到明确拒绝

### Requirement: 上传与数值读回

RHI SHALL 提供 Buffer 和 2D Image 的 staging 上传，并返回可查询的完成令牌。调用方 SHALL 在 GPU 操作完成且满足 CPU 可见性要求后读取数据。

#### Scenario: 上传内容一致
- **WHEN** `rhi-upload-readback` 上传固定字节和像素并完成 GPU 读回
- **THEN** 结果逐字节一致，Debug 同步 Validation 无错误

### Requirement: 在途资源退役

释放请求 SHALL 立即使 CPU 句柄失效。实际销毁与槽位复用 SHALL 等待最后一次相关提交完成。

#### Scenario: 在途释放
- **WHEN** `rhi-resource-retirement` 和 `rhi-upload-retirement` 在资源仍被上传引用时请求释放
- **THEN** 旧句柄立即失效，完成前资源及槽位保留，完成后安全回收并可关闭设备
<!-- /AR:CONTRACT -->

长期职责见 [RHI 资源与上传](../../../docs/modules/rhi/README.md#p2a-资源与上传)。验收命令见 [验证计划](test-plan.md)。
