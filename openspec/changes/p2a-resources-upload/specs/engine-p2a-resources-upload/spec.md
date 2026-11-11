<!-- Generated from proposal.md; source-sha256: b53ac9276715f783380d2e08e14e2f4b8589395af8327e9100014b2810701892. Do not edit. -->

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
