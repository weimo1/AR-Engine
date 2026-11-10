# SDD 与 Code Review 接入指南

## 第一步：本地自检

文档、OpenSpec 契约或工作流配置改动后，从仓库根运行 `python scripts/workflow.py check`。它检查文档链接、派生契约和工具配置，结果直接显示在终端。日常自检无需保存日志或生成任务包。修改代码时，另跑覆盖该改动的构建和测试，交接时用一行记录实际命令与结果即可。

## 第二步：交接

日常开发由 [自动运行](自动运行.md) 驱动，交接靠 TASK.md 和 Git，不在对话间转发。需要人工交接时，说清任务编号、依据和已跑的检查即可。

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

每个开发任务按 [AGENTS.md](../../AGENTS.md) 先核对技术方案，再执行 TDD 和构建检查。开发中用 `--fast`，收尾运行 `python scripts/build_checks.py` 的完整门禁。通过后，只暂存本批差异并生成 OCR 输入：

```powershell
python scripts/prepare_review.py --change <change-id>
```

脚本用 OCR 的离线预览筛选文件，记录排除项及输入快照。把打印的目录、方案章节和实际测试结果交给未参与本批实现的 Codex 评审者。评审者按差异和调用方给出阻断意见或通过结论。修复阻断项后重跑受影响的检查，并让同一评审者复核修复差异。OCR 输入无法准备、缺少独立评审结论或额度耗尽时，保留差异，暂停勾选和提交。

提交时由 Git hook 检查暂存内容和提交信息。GitHub Actions 的共享检查在推送 main、PR 或手动触发时运行，使用 `workflow.py ci` 检查文档、Python 工具测试和 OpenSpec。共享检查保留 OCR 安装，因为其中有使用真实 OCR 二进制的离线文件筛选测试。

引擎 CPU Action 由手动触发，先核对 runner 的工具链，再使用 `build_checks.py` 编译和运行 CTest。GitHub 验证按实际提交和运行结果记录。OpenSpec 归档以适用验收完成为依据。

## Token 用量记录

本机 `.ar-workflow/update-token-usage.py` 可离线汇总 Codex/DeepSeek 用量，需要时手动运行，不再是每个任务的必做步骤。
