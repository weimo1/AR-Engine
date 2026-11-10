# AR Engine 文档索引

更新：2026-11-10。当前文档、调研和历史资料分开维护；归档不改变已确认决定，也不把草案升级为规范。

## 开始阅读

| 想了解什么 | 入口 |
| --- | --- |
| Agent 怎么干活 | 根目录 [AGENTS.md](../AGENTS.md)：一页规则；无人值守见 [自动运行](workflow/自动运行.md) |
| 现在做到哪、下一步做什么 | [项目总任务](../TASK.md)：唯一的进度页和任务队列 |
| 项目做什么 | [渲染引擎需求](../渲染引擎需求.txt) |
| 模块与技术路线 | [总体架构](总体架构与技术路线.md)、[实施路线](实施路线与阶段计划.md)（P0—P7 阶段） |
| 已确定的技术选择 | [ADR 目录](adr/) |
| 某个模块的长期设计 | [模块文档](modules/README.md)，模块到 change 的对应见 [模块与变更索引](workflow/模块与变更索引.md) |
| 某个里程碑的范围与验收 | `openspec/changes/<id>/proposal.md`，如 [P0](../openspec/changes/bootstrap-engine-p0/proposal.md) |
| OpenSpec 与检查命令 | [SDD 指南](workflow/SDD与CodeReview接入指南.md)、[文档体系](workflow/文档体系与确认机制.md) |
| 过去的讨论、旧的手工多对话模式 | [历史归档](archive/README.md) |

### 文档分层

| 层 | 位置 | 写什么 | 谁改 |
| --- | --- | --- | --- |
| 规则 | AGENTS.md、standards/ | 怎么干活、代码和提交规范 | 用户确认后改 |
| 产品与选型 | 需求、ADR、总体架构 | 做什么、用什么、模块边界 | 用户决定，架构讨论时改 |
| 模块设计 | modules/<模块>/README.md | 职责、接口、所有权、失败处理 | 实现改变长期契约时同步改 |
| 里程碑 | openspec/changes/<id>/ | 本阶段范围与验收用例名 | 开始一个阶段时写 |
| 进度 | TASK.md | 任务队列、一行结果、待办池 | 每个任务完成时改 |
| 实现事实 | 代码、测试、提交信息 | 实际行为 | 每次提交 |

同一件事只写在一层。进度不写进模块文档或 change；实现细节不写进需求。

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
| [C++ 目录与格式](standards/C++目录命名与格式规范.md)、[类型与生命周期](standards/C++类型接口与资源生命周期规范.md) | 目录规范修订2已确认，取消公开头ar路径层；类型规范按原确认范围执行 |
| [数学、坐标与颜色](standards/数学坐标与颜色规范.md) | Flower 对应约定按文中确认范围执行 |
| [错误、异常与日志](standards/错误异常断言与日志规范.md) | spdlog/诊断方向已定；错误策略仍有草案 |
| [Shader 与数据契约](standards/Shader编译与数据契约规范.md) | 语言/工具已选；详细编译工作方式仍为草案 |
| [构建与依赖](standards/构建工具链与依赖管理规范.md) | 工具方向已定；版本/CRT/预设待集成 |
| [并发与 GPU 同步](standards/并发线程与GPU同步规范.md) | 草案 |
| [测试与验收](standards/测试与验收规范.md) | 方法草案；Catch2 已选 |
| [Git 交付](standards/Git提交审查与合并规范.md) | 细则草案；远程保护尚未启用 |
| [Git 提交信息](standards/Git提交信息规范.md) | 新提交的类型、模块、正文与 commit-msg 格式门槛 |
| [CI 资源与费用](standards/CI资源与费用约束.md) | 控制成本方向已定，具体额度未配置 |
| [架构与设计图](standards/架构与设计图规范.md) | 图示建议，按设计复杂度采用 |

## 变更、技能与参考

- `openspec/changes/`：在途变更；proposal 是行为与验收编辑源，specs 由工具派生。实际 SDD 模板只维护于 `openspec/schemas/ar-engine/`；长期模块设计单独使用 [模块模板](modules/模块设计模板.md)。
- `openspec/specs/`：已验收归档的能力，不预填引擎愿景。
- [AI 技能约束](standards/AI技能约束.md) / [来源清单](workflow/技能采用清单.json)：七项 ar-* 技能；OpenSpec 生成技能由框架维护。
- [Flower Shader 调研](research/Flower-Shader编译与宏机制调研.md)、[公开 Shader 方案](research/Shader编译与变体公开方案对照.md)。
- [公开工程实践](research/公开工程实践对照与缺口.md)、[技能候选评估](research/本地agent-skills评估.md)、[OCR 适用性评估](research/OpenCodeReview适用性评估.md)。调研不是执行指令或已部署证明。

根目录只保留 README、AGENTS 和需求。旧构想、长讨论、旧自动化方案、他人参考和普通模板统一归档；需要追溯时查看归档目录，不在日常入口重复维护。
