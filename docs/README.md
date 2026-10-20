# AR Engine 文档索引

更新：2026-10-20。当前文档、调研和历史资料分开维护；归档不改变已确认决定，也不把草案升级为规范。

## 开始阅读

| 想了解什么 | 入口 |
| --- | --- |
| 项目做什么 | [渲染引擎需求](../渲染引擎需求.txt) |
| 模块与技术路线 | [总体架构](总体架构与技术路线.md)：基础选型已定，第 13 节集中维护首版实施提案及待确认边界 |
| 已确定的技术选择 | [ADR 目录](adr/) |
| 如何开始开发、检查和审查 | [SDD 与 Code Review 指南](workflow/SDD与CodeReview接入指南.md) |
| 当前进展和缺口 | [当前决策与待办](workflow/当前决策与待办清单.md) |
| 让其他对话继续完善 AI 工作流 | [后续完善交接](workflow/AI工作流后续完善交接-2026-10-20.md)；不默认恢复付费审查 |
| 如何维护文档和确认边界 | [文档体系与确认机制](workflow/文档体系与确认机制.md) |
| 过去的讨论和旧模板 | [历史归档](archive/README.md) |

项目仓库：[weimo1/AR-Engine](https://github.com/weimo1/AR-Engine)，本地 origin 使用用户给定的 SSH 地址。

## 已确认的技术决策

选型确认与引擎集成通过是不同状态。

| ADR | 决定 |
| --- | --- |
| [0001](adr/0001-Vulkan内存分配采用VMA.md) | Vulkan 内存使用 VMA |
| [0002](adr/0002-Shader采用GLSL与SPIR-V.md) | GLSL → SPIR-V |
| [0003](adr/0003-主视图采用Reverse-Z.md) | 主视图 Reverse-Z |
| [0004](adr/0004-诊断日志采用spdlog.md) | spdlog 与薄诊断封装 |
| [0005](adr/0005-数学库采用GLM.md) | GLM 及已核实的 Flower 数学/颜色约定 |
| [0006](adr/0006-Windows构建采用VisualStudio2026.md) | VS 2026 / MSVC；实际 C++23 标准开关待探针 |
| [0007](adr/0007-基础构建与第三方依赖选型.md) | CMake/MSBuild/CTest、vcpkg 与基础依赖 |

## 工程规范

| 文档 | 确认范围 |
| --- | --- |
| [AI 任务规则](standards/AI任务执行规则.md)、[技能约束](standards/AI技能约束.md) | 有效任务协议与技能边界 |
| [C++ 目录与格式](standards/C++目录命名与格式规范.md)、[类型与生命周期](standards/C++类型接口与资源生命周期规范.md) | 修订 1 已确认，实际模块实现后续建设 |
| [数学、坐标与颜色](standards/数学坐标与颜色规范.md) | Flower 对应约定按文中确认范围执行 |
| [错误、异常与日志](standards/错误异常断言与日志规范.md) | spdlog/诊断方向已定；错误策略仍有草案 |
| [Shader 与数据契约](standards/Shader编译与数据契约规范.md) | 语言/工具已选；详细编译工作方式仍为草案 |
| [构建与依赖](standards/构建工具链与依赖管理规范.md) | 工具方向已定；版本/CRT/预设待集成 |
| [并发与 GPU 同步](standards/并发线程与GPU同步规范.md) | 草案 |
| [测试与验收](standards/测试与验收规范.md) | 方法草案；Catch2 已选 |
| [Git 交付](standards/Git提交审查与合并规范.md) | 细则草案；远程保护尚未启用 |
| [CI 资源与费用](standards/CI资源与费用约束.md) | 控制成本方向已定，具体额度未配置 |
| [架构与设计图](standards/架构与设计图规范.md) | 图示建议，按设计复杂度采用 |

## 变更、技能与参考

- `openspec/changes/`：在途变更；proposal 是行为与验收编辑源，specs 由工具派生。实际模板只维护于 `openspec/schemas/ar-engine/`。
- `openspec/specs/`：已验收归档的能力，不预填引擎愿景。
- [技能安装记录](workflow/技能安装与适配记录.md) / [来源清单](workflow/技能采用清单.json)：七项 ar-* 技能；OpenSpec 生成技能由框架维护。
- [Flower Shader 调研](research/Flower-Shader编译与宏机制调研.md)、[公开 Shader 方案](research/Shader编译与变体公开方案对照.md)。
- [公开工程实践](research/公开工程实践对照与缺口.md)、[技能候选评估](research/本地agent-skills评估.md)、[OCR 适用性评估](research/OpenCodeReview适用性评估.md)。调研不是执行指令或已部署证明。

根目录只保留 README、AGENTS 和需求。旧构想、长讨论、旧自动化方案、他人参考和普通模板统一归档；需要追溯时查看归档目录，不在日常入口重复维护。
