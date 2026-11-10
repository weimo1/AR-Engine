# 变更：P1a 首个画面

## 问题与范围

把 P0 能力查询推进到独立三角形示例。按 [任务队列](../../../TASK.md) 实施窗口、设备、呈现、Shader、截图与窗口变化。沿用 [阶段路线](../../../docs/实施路线与阶段计划.md) 和 [构建选型](../../../docs/adr/0007-基础构建与第三方依赖选型.md)。开发基线 Vulkan 1.3，产品 1.4/Descriptor Heap 独立验收。

## Capabilities

- 新增 `engine-p1-first-frame`。

## 行为与验收

<!-- AR:CONTRACT capability=engine-p1-first-frame -->
## Purpose

让开发者在 Windows 开发机上通过独立程序验证从窗口建立到 Vulkan 三角形输出的最小路径，并用可复现的测试区分设备、呈现及画面问题。

## ADDED Requirements

### Requirement: 窗口生命周期

platform SHALL 在主线程管理 GLFW 窗口，提供事件轮询、关闭请求、帧缓冲尺寸、最小化状态和 Vulkan instance 所需扩展名。创建失败 SHALL 返回明确错误。

#### Scenario: 隐藏窗口
- **WHEN** 调用者创建隐藏窗口、轮询事件并请求关闭
- **THEN** 可读取有效帧缓冲尺寸和关闭状态，释放后可重建窗口，由 `platform-window-create` 和公开头检查覆盖

#### Scenario: Vulkan 扩展查询
- **WHEN** 调用者查询 Vulkan 所需 instance 扩展
- **THEN** 可用时返回独立持有的扩展名，不可用时返回错误，由本机 `platform-window-vulkan-extensions` 覆盖可用路径

### Requirement: 逻辑设备与诊断

RHI SHALL 在显式请求时创建支持 Vulkan 1.3 Dynamic Rendering、Synchronization2 和 graphics 队列的设备。能力不足 SHALL 明确拒绝，Debug SHALL 启用 Validation 并把诊断转入 core。

#### Scenario: 设备创建与拒绝
- **WHEN** 执行 `rhi-device-create`、`rhi-device-missing-feature` 与 `rhi-device-selection`
- **THEN** 本机设备可创建和释放，Debug 零 Validation 错误；fixture 验证缺特性时拒绝、可用设备选择和 Debug 诊断转发

### Requirement: Vulkan 1.4 与 Descriptor Heap 设备验收

RHI SHALL 保留 Vulkan 1.3 路径，并支持显式请求 Vulkan 1.4 图形设备。1.4 路径 SHALL 报告 Maintenance 5/6、Dynamic Rendering Local Read 和 Host Image Copy 的查询结果。显式请求 Heap 时 SHALL 实际启用特性并核对限制及关键设备入口，缺能力时返回错误。

#### Scenario: 显式启用 Heap
- **WHEN** 执行 `rhi-device-vulkan14-heap` 和 `rhi-device-enhanced-unavailable`
- **THEN** 支持设备可创建和关闭，Debug 零 Validation 错误；fixture 确认缺 API 或扩展时明确拒绝。Heap 资源写入、绑定和读回待 P2 资源完成后另验

### Requirement: 最小 Vulkan 绘制

示例 SHALL 使用 Vulkan 1.3 Dynamic Rendering 与 Synchronization2 绘制三角形，并支持有限帧退出。能力不足 SHALL 明确失败。

#### Scenario: 设备及画面验证
- **WHEN** 执行清屏及三角形用例
- **THEN** `sample-clear-frames`、`shader-build`、`sample-triangle-frames` 满足对应任务的判定

### Requirement: 截图与窗口变化

示例 SHALL 支持末帧 PNG 截图，窗口变化时恢复出帧，最小化时暂停渲染，关闭时有序释放资源。

#### Scenario: 像素与重建验证
- **WHEN** 执行 `sample-triangle-screenshot` 与 `sample-resize`
- **THEN** 中心像素与清屏四角可区分，窗口变化后继续出帧且没有 Validation 错误
<!-- /AR:CONTRACT -->

## 影响与验收依据

platform、rhi、runtime、构建和示例按队列分批接入。输入映射、编辑器和资产管线留给后续阶段。T07 由用户查看画面并验收。
