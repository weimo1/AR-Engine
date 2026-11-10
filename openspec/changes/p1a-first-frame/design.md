# 技术方案

## 授权与依据

沿用 [T01–T07](../../../TASK.md)、[总体架构](../../../docs/总体架构与技术路线.md) 的模块职责及 [ADR-0007](../../../docs/adr/0007-基础构建与第三方依赖选型.md) 的 GLFW 选型。

## 主责与模块关联

[platform](../../../docs/modules/platform/README.md) 管理窗口。[rhi](../../../docs/modules/rhi/README.md) 管理 Vulkan 对象和同步，[runtime](../../../docs/modules/runtime/README.md) 负责组合。T01 不接入逻辑设备、surface 或帧循环。

## 实现与取舍

T01 按 [platform 模块技术方案](../../../docs/modules/platform/README.md) 接入 GLFW 窗口。T02 按 [RHI 模块技术方案](../../../docs/modules/rhi/README.md#p1a-逻辑设备与-validation) 接入逻辑设备和 Validation。T08 按 [RHI 的 1.4 与 Heap 方案](../../../docs/modules/rhi/README.md#vulkan-14-与-descriptor-heap) 验证显式启用路径。后续任务按阶段依赖接入 surface、呈现及 Shader。

## 任务上下文与协作

依据、技能和可写范围见 [context.json](context.json)。进度只在 [TASK.md](../../../TASK.md) 维护。

## 验证与恢复

本机按 [验证计划](test-plan.md) 运行双配置门禁。窗口测试仅操作本进程的隐藏窗口。CPU runner 仅筛除显式登记的真实 Vulkan 用例。
