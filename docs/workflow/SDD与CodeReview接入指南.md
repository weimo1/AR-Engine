# SDD 与 Code Review 接入指南

## 第一步：本地自检

文档、OpenSpec 契约或工作流配置改动后，从仓库根运行 `python scripts/workflow.py check`。它检查文档链接、派生契约和工具配置，结果直接显示在终端。日常自检无需保存日志或生成任务包。修改代码时，另跑覆盖该改动的构建和测试，交接时用一行记录实际命令与结果即可。

## 第二步：多 Agent 交接

在对话里交代任务、方案依据、输入版本和验收情况。接手者核对同一版差异后开始工作。直接复制 [四行交接格式](多Agent协作与上下文.md#从方案到交付怎么交接)。

统筹维护 [项目总任务](../../TASK.md)。用户安排本批执行者，按现有方案和实际差异交接。

## 本地准备

从 AR Engine 根目录执行，路径有空格时使用 PowerShell 的 `Set-Location -LiteralPath`。

```powershell
Set-Location -LiteralPath 'F:\毕业\AR Engine'
powershell -ExecutionPolicy Bypass -File scripts/setup-workflow.ps1
python scripts/workflow.py doctor
```

工具固定：OpenSpec 1.14.3，由 `package-lock.json` 锁定本地 npm 安装；OCR 1.13.1 Windows AMD64，通过 `workflow/tool-lock.json` 中的 URL/SHA-256 固定，放 `.tools/`。Node 本机 22.14.0 满足该 OpenSpec 版本要求；Python 要求 3.10+，本机为 3.13。Python 脚本只用标准库，未将 Python 变成渲染引擎运行依赖。OCR 下载先写任务临时文件，校验后发布，损坏的项目工具可重新下载恢复。

初始化已生成六个 `.agents/skills/openspec-*` 工作流入口，不改已有七个 `ar-*` 技能。新会话可发现项目技能；当前会话是否提供技能选择取决于客户端实际加载，CLI 命令可直接使用。不修改用户级配置、凭据或技能。

生成技能内的 `openspec ...` 示例统一在项目根替换为 `npm.cmd run sdd -- ...`，不依赖全局 PATH。明确只做规划时使用 propose/explore；已有实现授权时按项目方案使用 CLI 和 apply，既有授权内的材料同步不逐项重复确认。归档遵守项目证据核对后调用固定 CLI，不直接沿用技能里的语义 sync 或 mkdir/mv 示例。上游生成文件保留原样，以便固定版本升级。

来源：[OpenSpec 固定发布](https://github.com/Fission-AI/OpenSpec/releases/tag/v1.14.3)、[OCR 固定发布](https://github.com/alibaba/open-code-review/releases/tag/v1.13.1)。

## 开始一个变更

```powershell
npm.cmd run sdd -- new change add-render-bootstrap --schema ar-engine
npm.cmd run sdd -- instructions proposal --change add-render-bootstrap --json
```

CLI 只建立目录并输出指导，不会自行完成设计。按 schema 编写 `proposal.md`、`design.md`、`test-plan.md`、`tasks.md`。已有有效方案覆盖的小修复可以直接引用，不机械补一整套新材料。

### 行为与验收只编辑一处

在 `proposal.md` 的契约块写标准 OpenSpec delta。capability 名使用单段 kebab-case；未来如需要分层命名再扩展工具。

```markdown
<!-- AR:CONTRACT capability=example-capability -->
## Purpose

这里说明真实能力的用途、调用方以及可观察边界；新能力的 Purpose 至少五十个字符。不为通过结构检查编造产品功能。

## ADDED Requirements

### Requirement: 明确失败结果
程序 SHALL 在指定输入无效时返回非零退出码并保留原因。

#### Scenario: 输入无效
- **WHEN** 输入不满足约定
- **THEN** 返回非零退出码并输出可定位诊断
<!-- /AR:CONTRACT -->
```

```powershell
python scripts/workflow.py derive add-render-bootstrap
python scripts/workflow.py check
npm.cmd run sdd -- validate add-render-bootstrap --strict --no-interactive
npm.cmd run sdd -- status --change add-render-bootstrap --json
```

`derive` 写 `specs/<capability>/spec.md`，并记录来源摘要。`check` 验证完整集合及内容一致性；存在未知派生文件时报告错误，不自动删除。更改源契约后重新派生。OpenSpec 验证结构和 delta；项目检查验证派生一致性；两者均不执行产品验收或证明方案已确认。没有行为变化的纯文档小修可不创建新变更；确需 OpenSpec 无规格流程时，应另建并验证适用 schema，不能随意 `skip_specs` 规避需求。

## 本地检查

```powershell
python scripts/workflow.py check
python -m unittest discover -s scripts/tests -v
```

前者检查项目自有文档链接、入口、工具版本与派生契约。后者验证过期契约、缺失/多余规格、非法路径、过期输入、缺覆盖、不完整审查与未修复阻塞项等失败路径。第三方技能参考不是项目自有 C++ 测试；本轮不宣称引擎编译或 GPU 检查通过。

## 评审与交付

按本批任务运行 `python scripts/build_checks.py` 的适用模式。暂存本批差异后运行：

```powershell
python scripts/prepare_review.py --change <change-id>
```

脚本用 OCR 的离线预览筛选文件，记录排除项及输入快照。把打印的目录、方案章节、实际测试结果交给现有评审对话。审查根据实际差异和调用方给出意见；修改后重新验证受影响的检查，并用新差异复核。旧自动交接和模型运行记录保留用于追溯。

提交时由 Git hook 检查暂存内容和提交信息。GitHub Actions 的共享检查在推送 main、PR 或手动触发时运行，使用 `workflow.py ci` 检查文档、Python 工具测试和 OpenSpec。共享检查保留 OCR 安装，因为其中有使用真实 OCR 二进制的离线文件筛选测试。

引擎 CPU Action 由手动触发，先核对 runner 的工具链，再使用 `build_checks.py` 编译和运行 CTest。GitHub 验证按实际提交和运行结果记录。OpenSpec 归档以适用验收完成为依据。

## Token 用量记录

- 每个对话在任务完成、交接或主动停止前，由主 Agent 从项目根目录串行执行 `python .ar-workflow/update-token-usage.py`，更新 `.ar-workflow/AI-token用量统计.txt` 和对应 JSON 明细。子 Agent 不并发写统计文件；有可用的会话或请求身份时随交接提供，由主 Agent 统一汇总。
- 用量只依据实际会话计数、原始响应 usage 或可追溯的原生会话汇总。相同会话的累计计数只取最新值，DeepSeek 按响应 ID 去重；缓存输入、推理输出与继承的会话历史不得重复相加。无法区分父子 Agent 独立消耗时明确标为未拆分，不伪造明细。
- 失败、截断及重试中已返回的实际 usage 也应计入；中断无响应、记录丢失、口径冲突或来源不完整时标记未知/缺口，不按零处理，不用估算冒充实际消耗。不同来源不能确认无重叠时单列，不强行合并。
- 统计只离线读取用量记录，不为统计调用模型，不读取或导出凭据，不将会话正文写入报表。Codex token 计数不等于订阅费用或五小时额度，DeepSeek 本地统计不替代供应商账单。
- 脚本及报表目前是本机 `.ar-workflow/` 中的忽略文件，不自动提交或推送。新环境缺少脚本、记录不可访问或统计失败时，交接中说明具体缺口；不因此安装全局工具、修改原始证据或阻塞其他已授权工作。

## 临时过程文件

普通检查日志、试验截图、临时脚本及重复导出统一使用 `build/temp/<task>/`，任务结束后确认无进程使用及无待审引用即可清理对应子目录。共享工作流 CI 的本地过程输出固定使用 `build/temp/workflow-ci/`，后续运行覆盖该位置。

长期方案和必要的复现结论写回 `docs/` 与对应 change。正式构建、依赖缓存及待审 OCR 快照分别沿用现有目录；命令表验收输出仍在 `build/logs/`、`build/metadata/` 等约定位置。清理 `build/temp/` 前结束使用这些文件的检查任务。
