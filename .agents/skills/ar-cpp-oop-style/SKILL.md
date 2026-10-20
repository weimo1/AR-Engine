---
name: ar-cpp-oop-style
description: >-
  Develop and review AR Engine C++23 code, interfaces, ownership and resource
  lifetimes, and evaluate C++ build/dependency integration. Apply to C++ design
  and code changes; use the project's confirmed architecture rather than forcing
  a universal OOP pattern.
license: CC-BY-NC-SA-4.0
---

# C++ 工程指导：AR Engine 采用版

先读 [项目 AI 技能约束](../../../docs/standards/AI技能约束.md)，再读当前任务关联的已确认需求、架构、技术方案与工程规范。原作者的全部风格不是项目默认规则；本入口保留名称来源，但不强制 OOP 架构。

## 设计与实现

- 先确认模块职责、输入输出、错误行为、资源所有权和线程约束。接口、结构体、自由函数、枚举、variant、模板或虚分发按实际需求选择。
- 对外边界、实现隐藏或开放行为集合可以评估虚接口；不为普通内部函数或每个类型创建接口/Impl。闭合集合和数据热点允许值类型、批处理和静态分发。
- 优先明确的单一所有者与 RAII，借用表达必须具有有效期约定；共享所有权需要实际理由。GPU 生命周期另行分析，不能把通用 C 句柄析构示例原样当作 Vulkan 销毁方案。
- 类型、单位、索引和资源标识表达清楚；校验外部输入、数值边界和资源失败。断言保护内部不变量，不替代 release 模式输入校验。
- 使用经固定工具链验证的 C++23 能力。命名、括号、const 位置、class/struct、异常/RTTI 等遵循项目已确认规范；未确定且影响技术方案时准备具体建议供用户审阅。
- 构建系统尚未选定时先写方案。若使用 CMake，按 target 表达模块和依赖要求，验证具体库版本与运行库/ABI 配置；不直接套用 GCC 参数或 libstdc++ 宏到 Windows 工具链。
- 验证与改动相称，修复缺陷优先保留有效回归。性能问题配合 `$ar-cpp-hpc-optimization`，不能为遵守风格牺牲已确认的性能契约。

## 按影响读取参考

参考文档是候选经验与示例。遇到其绝对风格要求、默认依赖或平台命令与项目不一致时，以已确认项目规范为依据，记录重要冲突。不要一次加载全部参考。

| 任务 | 参考 |
| --- | --- |
| 模块拆分、纯计算与真实交互边界 | `references/decoupled-modules.md` |
| CMake target、依赖可见性与预设 | `references/cmake-first-projects.md` |
| 第三方集成、依赖来源或 ABI | `references/dependencies/router.md` |
| 所有权、借用和资源释放 | `references/ownership-lifetime.md` |
| 错误与结果类型 | `references/error-handling.md` |
| C 句柄封装 | `references/wrapping-c-resources.md`，示例须核查，GPU 延迟释放另设计 |
| UB、边界、算术或共享可变状态 | `references/undefined-behavior.md` |
| 回调、模板与类型擦除 | `references/functors-callbacks.md`、`references/generics-compile-time.md`、`references/type-erasure.md` |
| 诊断探针与可控运行入口 | `references/debug-instrumentation.md`、`references/debug-harnesses.md` |
| Unicode 与路径 | `references/text-encoding.md` |
| 安装/打包已被纳入任务 | `references/deployment/router.md` |

独立 harness 必须调用正式实现，不能复制逻辑制造第二个看起来正确的版本。视口与编辑器交互的正确性还需要真实运行证据。

## 来源

原版入口在 `references/upstream-skill.md`，仅用于比较来源与取舍，不是默认编码规范。详细参考与 LICENSE 保留；不得因参考推荐某库就直接增加项目依赖。
