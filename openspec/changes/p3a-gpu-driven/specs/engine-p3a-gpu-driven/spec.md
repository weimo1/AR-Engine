<!-- Generated from proposal.md; source-sha256: cbe09803c0d75af3a5cb70da8725be0d771554d2486623701649b353fd966757. Do not edit. -->

## Purpose

让渲染路径使用 GPU 生成的可见对象和绘制参数完成一帧，并以可重复的场景、CPU 参考和像素读回分别验证剔除与绘制。

## ADDED Requirements

### Requirement: GPU 可见集合

Compute SHALL 对固定相机的对象包围体执行视锥剔除，在容量内输出对象标识及实际可写命令数。超过容量 SHALL 明确报告。

#### Scenario: 可见集合对照
- **WHEN** `rhi-gpu-culling-reference` 运行混合、空集合、全可见及容量边界场景
- **THEN** 未溢出时集合等于 CPU 参考且无重复，溢出时输出为参考集合的合法子集，计数不越界并报告溢出

### Requirement: 间接绘制

图形管线 SHALL 消费同帧 Compute 生成的绘制命令和计数。

#### Scenario: 真机间接绘制
- **WHEN** `sample-gpu-driven-pixels` 运行固定可见集合及零可见场景
- **THEN** 可见几何产生预期像素，零可见时输出清屏色，Debug 同步 Validation 无错误
