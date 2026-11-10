# platform

platform 管理窗口和平台事件，向 runtime 提供窗口状态与呈现所需的接入信息。Vulkan 对象与同步由 rhi 管理。构建目标 `ar_platform` 为静态库，公开接口使用 C++ 值类型与 core 错误类型，GLFW 通过项目固定的 vcpkg baseline 私有链接。选型见 [ADR-0007](../../adr/0007-基础构建与第三方依赖选型.md)。

## 窗口接口

| 接口 | 职责 |
| --- | --- |
| `Window::open(config)` | 按尺寸、标题及可见性创建窗口，返回唯一所有者或明确的错误。GLFW 使用 `GLFW_NO_API`。 |
| `pollEvents()` | 在主线程轮询系统事件。 |
| `requestClose()`、`shouldClose()` | 设置和读取关闭请求；由调用方决定何时结束事件循环。 |
| `framebufferSize()`、`isMinimized()` | 分别查询帧缓冲的像素尺寸和窗口的最小化状态。 |
| `requiredInstanceExtensions()` | 显式查询时才访问 Vulkan loader，返回独立持有的扩展名副本或查询错误。窗口销毁后副本仍有效。 |

## 所有权与失败

P1a 支持一个活动窗口。`Window` 负责 GLFW 初始化、窗口销毁和终止 GLFW，整个生命周期均在主线程。无效尺寸和重复打开在调用 GLFW 前返回错误。初始化成功后若创建失败，本次打开操作释放 GLFW 会话。关闭请求只设置标志，实际销毁由所有者析构触发。GLFW 失败会保留描述和原始错误码。

输入映射与 surface 接入分别随输入、呈现切片定义。当前阶段的范围和验收见 [P1a](../../../openspec/changes/p1a-first-frame/proposal.md)。
