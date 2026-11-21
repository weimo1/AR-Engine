<!-- Generated from proposal.md; source-sha256: 67f01151d4c87101623d7882ad41a6725033d2f5d3e5da130326a10212458cbc. Do not edit. -->

## Purpose

在同一多网格场景中对照 CPU 参考与 GPU 剔除，并为 P5 保留一致的几何和材质输入。

## ADDED Requirements

### Requirement: 共享几何、实例与材质数据

整合路径 SHALL 提供共享顶点与索引缓冲、稳定的实例索引、按用途解释的材质纹理槽及明确的容量和失败诊断。

#### Scenario: 多网格静态场景布局

- **WHEN** `render-static-scene-draw-data` 构造两个网格、共享网格的实例和不同材质
- **THEN** 索引范围、顶点偏移、实例索引与材质引用符合约定，超容量和非法索引被拒绝

### Requirement: 有界 Compute 剔除与间接绘制

GPU 路径 SHALL 生成有界索引间接命令，并声明 Compute 写入到 Draw Indirect 读取的缓冲依赖。

#### Scenario: 同场景 CPU 和 GPU 对照

- **WHEN** `sample-static-scene-gpu-driven` 用相同场景、相机和材质执行 CPU 参考与 GPU 剔除
- **THEN** 可见实例集合一致，GBuffer 和最终画面符合原容差，零可见结果正确且 Debug 同步验证无错误
