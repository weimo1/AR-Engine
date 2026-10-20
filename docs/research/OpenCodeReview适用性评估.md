# Open Code Review 适用性评估

> 试运行状态同步（2026-10-19）：用户已要求开始接入；固定 v1.13.1 原生 Windows 工具已下载并核对 SHA-256，使用 delegate 进行本地选择/规则准备。实际检查见 [接入指南](../workflow/SDD与CodeReview接入指南.md)。本评估仍保留先试验后设强制门槛的结论，未证明缺陷检出效果或配置远程模型。

日期：2026-10-18（Asia/Shanghai）。状态：候选工具评估与接入建议，未安装、未运行、未接入必需检查。

后续用户对“先试验，再设为强制门槛”的建议回复“可以”，记录为认可候选试用方向；不等于工具已验证、已部署或可跳过试用验收。用户另明确公开仓库，调用与 CI 预算按 [CI 资源与费用约束](../standards/CI资源与费用约束.md) 草案落实。

固定源码提交：`8eb83340a487b1150af00701b94f01af97e4b4d6`，通过 GitHub API 获取并按该提交读取相关源码/文档。仓库 [alibaba/open-code-review](https://github.com/alibaba/open-code-review)。本轮是定向静态核查，不是对该工具的完整安全/正确性审计。

用户提供的 master/README.zh-CN.md 链接本轮返回 404；现有中文说明在 main/docs/i18n/README.zh-CN.md。用户提供的阿里云社区文章属于用户贡献，规模、缺陷量和效果数据按项目自述理解，不能作为本项目 C++/Vulkan 检测效果的实测证明。

## 1. 判断

推荐将 OCR 作为独立 AI Review 执行器候选，优先在 PR 阶段试用；保留提交前确定性检查。项目 Harness 负责输入冻结、覆盖核对、问题处理状态和合并资格，OCR 负责审查组织与发现问题，Actions 负责调度，分支保护负责阻止不合格合并。

不以安装 OCR 代替构建、静态分析、GPU 同步验证或画面验收；也不因工具运行结束就认定代码通过审查。

## 2. 已核查的能力与限制

| 核查项 | 结果与项目含义 |
| --- | --- |
| C++ | 内置 cpp.md 规则与常见 C++ 扩展名筛选；主要是通用资源/风格检查，不是 Vulkan 同步分析器 |
| 规则定制 | 项目规则 JSON、路径匹配与显式 --rule；需要适配已确认工程规范，不能直接把内置风格升为强制规则 |
| 审查输入 | 支持工作区、分支范围与单提交；默认工作区包含 staged、unstaged、untracked，不能直接满足暂存树隔离要求 |
| 结构化结果 | 有 comments、severity/category、warnings、manifest 与覆盖信息，可供受信检查器解析 |
| Actions | 仓库提供 action.yml，可发 PR 评论并保存结果；不是开箱即满足本项目门槛 |
| 模型执行 | 可配置模型端点；delegate 由宿主 Agent 审查。独立性由执行上下文和权限落实，命令名称本身不能保证 |

源码：[C++ 规则](https://github.com/alibaba/open-code-review/blob/8eb83340a487b1150af00701b94f01af97e4b4d6/internal/config/rules/rule_docs/cpp.md)、[规则映射](https://github.com/alibaba/open-code-review/blob/8eb83340a487b1150af00701b94f01af97e4b4d6/internal/config/rules/system_rules.json)、[CLI](https://github.com/alibaba/open-code-review/blob/8eb83340a487b1150af00701b94f01af97e4b4d6/pages/src/content/docs/zh/cli-reference.md)、[模型结果](https://github.com/alibaba/open-code-review/blob/8eb83340a487b1150af00701b94f01af97e4b4d6/internal/model/review.go)。

## 3. 必须适配的四点

### 审查完成不等于没有问题

CLI 文档的退出 0 表示运行完成，可以有评论和非致命警告。Action 的预算说明也明确预算耗尽可保留部分结果并退出 0。因此，禁止直接用 shell 退出码或绿色 Action 作为代码质量批准。

[manifest 实现](https://github.com/alibaba/open-code-review/blob/8eb83340a487b1150af00701b94f01af97e4b4d6/internal/session/manifest.go) 区分 complete/partial/failed/skipped，以及 selected/completed/reused/failed/waived。complete 描述被选集合的执行终态，不能据此证明所有应审文件都被选入，更不等于无缺陷；显式 waived 也需要本项目核对，不能直接接受。

本项目适配器应分别核对：输入/规则/模型版本、预期审查集合、实际覆盖与复用有效性、失败/豁免原因、发现的问题及其修复复核。必要审查文件未覆盖、预算/任务失败或待解决阻塞项都不得放行。无评论只是“本次未报告问题”，不是正确性证明。

### 覆盖范围必须按我们的目录验证

固定提交的 [扩展名清单](https://github.com/alibaba/open-code-review/blob/8eb83340a487b1150af00701b94f01af97e4b4d6/internal/config/allowlist/supported_file_types.json) 有 .cpp/.h/.hpp/.cmake 等，但没有 .glsl/.vert/.frag/.comp。需要项目 include 覆盖与对应规则，并实际验证路径选择；CMakeLists.txt、共享 .h、构建脚本和测试文件也必须逐项检查规则匹配，不能凭“支持 C++”推断全部覆盖。

[规则说明](https://github.com/alibaba/open-code-review/blob/8eb83340a487b1150af00701b94f01af97e4b4d6/pages/src/content/docs/zh/review-rules.md) 明确存在默认测试/生成物排除，include 可覆盖其中部分过滤，且不是白名单；自定义匹配项默认替换系统规则，merge_system_rule 才合并系统内容。必须核对实际最终规则，而不是只放一个 JSON 文件就认为生效。

### 通用 C++ 规则不能直接当引擎规则

内置规则倾向 RAII、智能指针、STL 算法等。项目需明确：GPU 析构仍受在途使用约束、借用裸指针不等于错误所有权、循环不因存在算法替代就成为缺陷，错误/异常遵循本项目有效基线。

针对 Vulkan/VMA 增加当前已确认契约与方案的审查项，例如 CPU/GPU 所有权、退役证明、同步、布局、索引范围、绑定路径能力、Reverse-Z 与颜色链路。未确认草案作为背景而非阻塞依据；这些仍是 LLM 审查提示，不是确定性证明。

### Action 与身份需要受控集成

[固定版本 Action](https://github.com/alibaba/open-code-review/blob/8eb83340a487b1150af00701b94f01af97e4b4d6/action.yml) 的 ocr_version 默认 latest，提供模型凭据、预算、规则、PR 评论等输入。接入时固定 Action 提交与实际 CLI 版本，限定模型与预算，规则从受信配置获取；不使用浮动 latest，也不让被审 PR 改规则后自行放行。

审查可以独立于 Windows 编译任务在合适 runner 上执行；源码读取型审查不要求和 VS 构建同一台机器。调用模型端点意味着将选定代码及检索上下文发送到所配置服务；实施前明确数据范围和端点，不读取/上传本机凭据，不默认开启外部 MCP 扩大访问。

## 4. 推荐接入位置

```text
提交前：统一快速检查 + 实现者自检 → Commit
PR：构建/测试/Shader/GPU 检查 + 独立 OCR 审查
    → Harness 解析并核对覆盖、输入与问题处理
    → merge-gate → 受保护合并 → 集成验收
```

OCR 可选择独立模型端点模式或 delegate 模式，依据可用执行环境确定。delegate 文档只提供工具职责划分，不能据此假定当前客户端已支持无人值守/独立会话调度或额度复用。无论哪种方式，实现者同一上下文直接审自己都不能冒充独立 Review。

## 5. 试用后再启用阻塞

先固定版本、规则及样本，完成离线规则/筛选验证和受控缺陷评测，再成为必需 ai-review 检查。试用阶段 OCR 为候选，不取代本来必需的独立审查，不能借“观察模式”把无审查代码合并。

试验样本覆盖：已知正确代码、确定的生命周期/索引/布局缺陷及修复版本、合法借用指针/手写循环等易误报情况、GLSL/C++ 头/测试/CMake 路径、部分文件失败、预算耗尽、错误输入版本和无支持文件。记录检出、漏报、误报、覆盖、耗时与模型调用成本；不能据宣传填入通过阈值或声称已满足 Vulkan 审查。

必须证明有问题时能形成可处理发现、审查不完整时 gate 阻塞、修复后重新检查才解除，且规则变化与新提交使相关结果失效。发现严重度用于分流，不能只靠模型标为 high 就永久阻塞或仅凭 low 就忽略真实契约错误；按已有审查规则复核实际依据。

当前没有引擎 C++ diff、模型配置或 CLI 试跑结果，仅完成公开资料与固定源码静态核查。没有运行安装脚本、配置用户级技能、发送本项目源码给模型、发布评论或部署 Action。
