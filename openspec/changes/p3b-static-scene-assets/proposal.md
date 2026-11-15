# P3b 静态场景与资产

按[实施路线](../../../docs/实施路线与阶段计划.md)从 P3a 固定几何推进到可保存的静态场景。沿用 [A2/A4](../../../docs/总体架构与技术路线.md#2026-10-23-架构确认记录) 与 [ADR-0007](../../../docs/adr/0007-基础构建与第三方依赖选型.md)。首批使用仓库自制 glTF、外部 PNG 和两实例夹具。一般材质、动画、资源流送和编辑器留待后续。

<!-- AR:CONTRACT capability=engine-p3b-static-scene-assets -->
## Purpose

从可移植的场景文件加载静态资产，提取可渲染输入并验证保存重开一致性。

## ADDED Requirements

### Requirement: 静态资产加载

同一场景内同一规范化资产引用 SHALL 共享同一份 CPU 资产。加载失败 SHALL 标识资产及失败阶段。

#### Scenario: 静态 glTF 与纹理
- **WHEN** `assets-static-gltf` 加载含静态索引三角形和 PNG 的夹具，并模拟缺失与错误输入
- **THEN** 有效输入提供顶点、索引和 RGBA 数据；无效输入有来源和失败阶段，修正后可重试

### Requirement: 场景保存与同步提取

场景 SHALL 保存稳定对象标识、实例变换、相机和可移植资产引用。runtime SHALL 提取可独立存活的值数据供 render 使用。

#### Scenario: 保存重开
- **WHEN** `scene-static-roundtrip` 保存含两个共享资产实例的场景并重新打开
- **THEN** 资产仍共享，实例变换独立，场景及提取结果等价；失败重开保持当前场景

#### Scenario: 真机静态场景
- **WHEN** `sample-static-scene-pixels` 打开夹具并呈现
- **THEN** 两个实例各命中至少两个不对称的预期纹素及独立背景探针，保存重开后探针一致，Debug Vulkan Validation 无错误

#### Scenario: 候选上传失败
- **WHEN** `sample-static-scene-pixels` 在候选场景取得部分上传资源后注入失败
- **THEN** 活动场景继续呈现，释放候选独占资源并保留活动场景共享资源，随后可以重试
<!-- /AR:CONTRACT -->
