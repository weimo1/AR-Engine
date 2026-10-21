# SDD 与 Code Review 接入指南

多 Agent 的任务依据、角色包和串行交接见 [协作与上下文](多Agent协作与上下文.md)。任务级审查绑定当前 change 的冻结规划与适用 Skills；无任务通用检查不冒充需求符合性验收。agent-pack 不自动启动模型；显式 dsh-run 下发执行见 [DSH 接入](DSH直接执行接入指南.md)。新提交遵循 [提交信息规范](../standards/Git提交信息规范.md)，commit-msg 与暂存检查均不调用模型。

更新：2026-10-21。状态：本地执行链仍为试运行；既有提交的 Actions 已实跑成功，本批多 Agent/DSH/提交信息变更不继承旧批准，须核对对应输入的任务记录及提交/Actions。远程保护未启用；当前任务是否通过以当前输入验证、独立审查和 gate 为准。

## 各工具负责什么

OpenSpec 管理变更提案、方案、任务和归档；项目脚本检查文档、派生契约及本地审查记录；OCR delegate 提供文件筛选和审查规则，实际判断可由独立上下文或已配置的 DeepSeek API 执行。CMake/CTest、Shader 与 GPU 验证在引擎基础工程建立后另行接入。

流程为：提案与验收 → 方案与验证计划 → 实现和适用检查 → 冻结审查输入 → 独立审查与修复 → 有效交付后归档。用户只审阅影响需求、架构和关键契约的方案，普通实现不重复申请。

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

## 准备独立 Code Review

```powershell
python scripts/workflow.py review-prepare
```

该命令校验 OCR 二进制，执行无模型的 `delegate preview`，记录 Git 工作区/索引、内容指纹、实际选择集合与排除项，并把输入复制到忽略目录 `.ar-workflow/reviews/<run>/snapshot/`。指纹绑定源码状态、选中集合、OCR 版本与预览内容摘要；检查时同时核对冻结副本逐文件内容和预览集合，不能通过缩小报告范围掩盖缺覆盖。初始 Git 尚无提交时支持 untracked 文件；后续工作区模式会保守绑定整个可见输入，不冒充 PR 的精确 source/target 检查。

交给独立只读 Agent：读取冻结副本中的 `AGENTS.md`、适用依据、全部选中文件及 `ocr-preview.json`，核对排除原因，报告逐文件覆盖、问题、未覆盖内容与实际结论。不要把 `report.example.json` 直接改名当审查结果。

独立结果保存后检查：

```powershell
python scripts/workflow.py review-check '<packet-directory>' '<actual-review-report.json>'
```

结果格式见 packet 中的示例：`fingerprint`、实际 `reviewer`、`independent_context`、`status`、`decision`、`reviewed_files`、`findings`。问题项使用 `level: blocking|must_fix|suggestion` 与 `status: open|resolved`，附具体位置、依据和复核。脚本只能发现结构、覆盖与内容身份不一致；声明独立身份的 JSON **不是身份认证**，本地检查通过也不授予远程合并资格。

项目规则显式包括 GLSL、头文件、测试、脚本与文档；排除第三方参考、许可原文、他人 GameEngineDoc 和生成的 npm 锁文件，理由保存在工具清单。它们仍纳入快照指纹；排除不等于完成第三方供应链/许可审计。新增文件类型应在实际 OCR preview 和规则检查中验证。

`review-prepare` 的 delegate 模式不调用 OCR 外部端点。宿主 Agent 审查使用宿主额度；下节的 API review 则会将所选差异、规则及检索上下文发送至 DeepSeek，并使用对应 API 额度。两条方式分别记录实际执行者，不混称离线审查。

## DeepSeek API 与 SSH

用户已授权配置 DeepSeek API。按 [官方接口说明](https://api-docs.deepseek.com/quick_start/agent_integrations/)，端点为 `https://api.deepseek.com`，采用 OpenAI Chat Completions 兼容协议。独立复审、裁决、执行与探测的新请求统一使用用户指定的 DeepSeek-V4.1-Flash（界面徽标 1017）、High；本机 SDK/API 模型 ID 为 `deepseek-flash`。Chat Completions 显式使用 `thinking={"type":"enabled"}`、`reasoning_effort="high"`。不回退 Pro 或降低等级。旧 Pro 证据仅保留当时事实；模型与推理参数变化使旧单元缓存失效。原生 OCR runner 没有已核实的 High 映射，付费运行被阻断；仍可无模型准备输入，正式审查使用 direct-api。

OCR 的用户级配置保存在 `%USERPROFILE%/.opencodereview/config.json`，只保存 provider、model 与凭据命令。密钥通过 Windows DPAPI 加密，密文与读取助手均在该用户目录下；文件权限限制为当前用户和 SYSTEM。项目不包含密钥、密文或私钥，换机器时需重新配置凭据。

```powershell
# 不运行原生 OCR 付费连接测试；该入口尚不能确保指定 High 配置

# 无模型的选择预览
.\.tools\ocr\1.13.1\ocr.exe review --preview --rule .opencodereview/rule.json --format json

# 有限预算的 API 审查；先使用下节的任务 verify，再执行 review
python scripts/workflow.py review <change-id> --mode direct-api --budget 100000 --seconds 600
```

示例预算不是每个任务的固定额度；先用 review-plan 核对当前输入估算后显式指定，CLI 不提供隐式默认预算。上述预算是单次运行的调度上限，不是账户费用封顶；正在运行的一组请求可能超出估计。大变更可能得到 partial；退出 0 不证明覆盖完整。当前 direct-api 自动核对覆盖、版本、冻结输入和问题，保留 direct-* 目录中的原始响应、run.json 与派生 report.json，不自动增加预算重试。native.json 仅属于历史 OCR 原生路径，不是当前 direct-api 的产物。

SSH 复用已有用户密钥；`ssh -T git@github.com` 已认证为 `weimo1`。GitHub 主机公钥与 [官方指纹](https://docs.github.com/en/authentication/keeping-your-account-and-data-secure/githubs-ssh-key-fingerprints) 对照，保持严格主机校验。仅在本仓库配置 `core.sshCommand` 使用 Windows 系统 OpenSSH，解决 Git 默认 SSH 未正确使用现有配置的问题，不改全局 Git 或关闭校验。

早期接入时（首次发布前），`git ls-remote origin` 已通过 SSH 认证，当时尚无分支引用。此段保留初次连接事实；当前已有 main 与正式发布，实际提交和 Actions 见当前状态，不能用此历史记录判断现在的远程状态。模型连接与工具调用测试同样不代表 OCR 的缺陷检出效果已经评估通过。

## 归档与后续接入

当前 `complete-local-workflow` 记录执行链补齐，`bootstrap-sdd-review` 保留早期接入记录；两者不是同一批验收输入。实现、验证和独立审查完成前保持在 `changes/`，`openspec/specs/` 不预填引擎愿景。归档要先核对实际任务、有效验证、用户确认范围与独立审查；OpenSpec 本身不会替项目完成这些检查，不应提前执行 sync-specs。

后续顺序：

1. 收敛总体模块、RHI、生命周期方案，创建首个引擎基础工程变更，接入真实 CMake/CTest/Shader 命令。
2. 用户仓库 `git@github.com:weimo1/AR-Engine.git` 已通过 SSH 认证与读取验证；既有基线已发布；本次新增内容仍须完成对应检查、独立审查和 gate 后再提交推送。
3. 先接共享确定性检查与 CI，再配置受信结果来源、主分支保护和独立审查发布身份；本地脚本不能替代它们。
4. 用受控样本评估 OCR 的缺陷检出、误报、漏报和成本后，再决定它能否承担必需审查执行器；当前仅试运行。
5. 渲染程序建好后接真实 GPU 检查；没有引擎源码、支持设备或工具时记录未就绪。

本地 hook 和任务执行器已实现；`.github/workflows/workflow-checks.yml` 已为历史提交实跑成功，新增输入需另核对其提交 SHA 的结果。分支保护、自动合并、长期驻留调度器与真实引擎验收仍未启用。

## 任务执行、恢复与提交

```powershell
python scripts/workflow.py install-hooks
python scripts/workflow.py start <change-id>
python scripts/workflow.py verify <change-id>
python scripts/workflow.py review <change-id> --mode direct-api --budget 100000 --seconds 600
python scripts/workflow.py gate <change-id>
python scripts/workflow.py resume <change-id>
```

start 要求 proposal、design、test-plan、tasks 与派生契约已准备。verify 在冻结副本执行文档/契约检查、Python 测试及 OpenSpec 严格校验；review 默认 direct-api，OCR 无模型准备固定输入，DeepSeek 执行独立判断。gate 重新核对原始模型结果、日志、完整覆盖和当前输入，只表示本地交付证据一致。实现者不能自填 resolved：需要修改后新审查，或按下节的独立误报复核证据派生处置，没有自动消除问题的按钮。

状态保存在忽略目录 `.ar-workflow/tasks/`，报告和日志在 `.ar-workflow/reviews/`。记录原子写入，操作使用单工作区锁。resume 会使输入变化前的证据失效，能识别中断审查和取回已完成结果，**不会自动再次调用 API**；仍活着的执行进程不能被另一个会话抢占。status 可查看精简状态。

审查运行前，执行器离线核对规则的规范化 JSON 摘要与工具清单中的原生规则身份；原生摘要来自固定 OCR 的已记录受控运行，不是 rule.json 的字节摘要。规则或工具变化后须复核并更新清单，不能直接接受未知摘要。结果同时核对冻结任务指纹、HEAD、逐项覆盖及原生源摘要的内部一致性；本地记录仍不提供身份认证。

实际试验发现，多文件的分组 planner 可能在预算检查前调用模型；所以 budget=1 也不是零请求预览，已去掉该做法。只有 delegate preview 不调用模型。正式 review 的规划消耗也计入原始结果，不自动增额重试。

新策略 pre-commit 使用 commit-check 导出 **Git 暂存树**，执行文档/派生/配置和 Python 语法检查，不重跑完整 unittest，不读取未暂存修复来替代提交内容。verify 与 CI 保留完整测试，快速提交检查不产生 gate 资格。提交快检不自动花 API 额度，也不替代独立审查。可以用 `review-prepare --staged` 准备暂存内容；需要 staged 任务时从 `start <change-id> --scope staged` 开始。手动共享检查入口是 `python scripts/workflow.py ci`，Actions 运行同一命令，无需模型凭据。

删除和纯删除会从 HEAD 导出完整旧内容及差异到冻结副本的 `.ar-review-context/deletions/`，生成材料必须被 OCR 选中并纳入指纹。审查需检查当前调用方、配置及文档引用；实际验证使用删除后的源码，不把旧代码恢复成当前实现。改名同时覆盖旧路径删除材料和新文件。二进制、非 UTF-8、超出大小约束的删除及未知排除仍明确阻断，需要专项方案。

### 独立 API 上下文复审

原生 OCR 因工具往返消耗较大而不适合当前整仓复审时，可以使用同一固定 OCR 输入的独立上下文：

```powershell
python scripts/workflow.py review <change-id> --mode direct-api --budget 100000 --seconds 600
python scripts/workflow.py gate <change-id>
```

此模式只向已授权的 DeepSeek 发送冻结正文、差异和规则，没有模型执行工具。代码、现行文档与参考资料分别组织；代码/删除单元提供当前调用方和规范，避免每份归档资料重复携带全部执行器代码。每个选中文件均须完整返回覆盖。原始响应、请求摘要、实际用量和模型身份保存在 packet 的 `direct-*` 目录。截断、未知模型、缺覆盖、预算不足和未处理问题都会阻断，gate 从原始响应重新核验。它不是 OCR 原生运行，不生成伪 native.json，也不是受信远程身份。预算预估不精确，仍需检查实际使用量；不会自动增加额度或重试。

先用 `python scripts/workflow.py review-plan <packet-directory>` 查看审查单元、预计用量和缓存命中，不调用模型或凭据助手。只有原始响应完整通过、单元正文及相关上下文/基线/规则/模型完全一致时，才复用 `.ar-workflow/review-cache/` 中的证据；修复导致关联单元失效，无关单元不重复付费。失败与截断从不缓存为通过。首次基线需要覆盖当前全部选中文件，后续普通变更由 OCR 按实际差异筛选；不为每次提交重新搭建基础设施、审查整个历史仓库或运行无关 GPU 测试。

direct-api 使用上述 `deepseek-flash`、thinking enabled、High。当前工具锁将审查与裁决的单请求输出上限设为 32,768 tokens（含推理），根据用户同意的日常有界方案限制，历史大请求已停止；大任务必须先拆分；这只是上限，不保证无截断，截断仍阻断，不自动降低等级或追加预算。旧快照没有 api_max_output 时保持原 8,192/16,384 请求构造和摘要，不改写历史证据。输出上限计入离线预算预估。finding 的 `path` 允许指向本单元的 assigned 文件或随请求提供的 frozen 上下文文件，但 `reviewed_files` 仍须与 assigned 完全一致；缺少 `level`/`path`/`line` 的样本会在同一批次内重采样一次，第二次仍不合法则整轮 fail-closed。每次采样尝试都单独保留为 `attempt-<批次>-<序号>.json`，`run.json` 的 `batches[].attempts` 记录每次尝试的摘要、实际用量、是否被接受及拒绝原因；`actual_tokens` 与报告合计都计入被拒样本，gate 重算时逐条核对文件摘要并要求记录值与证据一致，避免重采样的费用从证据中消失；accepted 尝试的 `actual_tokens` 必须为正整数且等于该批次解析出的用量。重采样本身也要再过一次预算门（`spent + 估计 > budget` 即停止且不再发请求），否则被拒样本可能把单次运行推过调用方给的上限。裁决阶段（finding-run）的证据校验按内容：引用正文必须在冻结文件中逐字出现，且与标注行区间相距不超过 8 行；实现者自己提交的 rebuttals 证据仍要求区间逐字相符，不享受该容差。按依赖关系提供生产代码及适用任务契约上下文，不将无关渲染 ADR 送入 Python 工作流审查；测试套件逐个分配审查，测试单元仍带生产代码。每单元正文最多约 120 KB，调用方单文件最多 100 KB、总计最多 1 MB，超限在付费前拒绝。完整统计实际 usage；模式不代表质量已获证明，正式结论仍要求全部单元完整覆盖且无未处理问题。参数依据 [DeepSeek 思考模式文档](https://api-docs.deepseek.com/guides/thinking_mode/)；模式、提示、规则及输出上限变化都会改变请求身份，使旧单元缓存失效。用户级 OCR 配置保持原样；不能把连接兼容测试时的非思考配置当作正式复审质量已经验证。

已有任务需要改为暂存模式时，显式 `start <change-id> --scope staged` 会记录范围变化并清空旧验证/审查资格。提交前不再编辑已冻结的暂存内容；修改后重新验证和审查。

本地 hook 可以被绕过；Actions 文件存在不代表服务器跑过，远程 required check 和独立审查身份仍需部署与实测。当前没有宣称自动合并就绪。

远程独立模型审查配置为 `.github/workflows/independent-review.yml`：`pull_request_target` 仅运行默认分支脚本，PR 内容从固定 head/base 导出为数据，绝不执行其脚本、测试或安装步骤。`ai-review` Environment 只允许默认分支，模型密钥仅放该环境，不放仓库级 secrets。自动付费审查仅面向仓库所有者创建的同仓库 PR；每轮最多 100,000 tokens、单请求 480 秒、工作流 10 分钟，不自动增额。超预算/不支持输入会使审查失败，需要明确处理。

模型结论通过 GitHub Actions 身份发布到确切 PR head 的 `ai-independent-review` 检查，主分支应要求此检查和 `workflow-checks`。这提供服务端结果及默认分支执行隔离，尚不是单独 GitHub App 身份：同样拥有仓库写权限并故意授予 token 写检查权限的工作流可能冒用同一 Actions 身份；仓库管理员也可改变保护。因此不得把它宣传为抵御恶意管理员或所有写入者的防篡改系统。更强身份隔离需后续安装独立 Review App。CI/保护实际部署结果继续记录在当前状态中。

正式远程策略取自可信默认分支。候选 PR 的 OCR 工具清单与规则必须完全匹配该策略，不能通过扩大排除项自我批准；规则/工具升级需要单独的可信准入，不走普通自动批准。跨运行缓存由默认分支工作流恢复/保存，每次仍核对完整请求和原始响应，PR 源码不能提供通过缓存。

代码单元最多分配四个文件，技能辅助代码单独组织。只对分配文件报告最终缺陷，其他源文件用于交互核对，不作为额外审查目标；所有 OCR 选中文件仍需分别完成覆盖。模型收到当前冻结输入的实际验证摘要；摘要不是远程身份，也不能代替独立 CI。重新执行 verify 或 review 时先撤销旧资格，新的失败不能被旧 pass 掩盖。

## 日常开发的固定路线

### 完整审查中的误报复核

仅对完整 direct-api 结果使用；失败、截断、缺覆盖的原审查仍需完成，不能借复核放行。证据链已实现，离线回归和小样本真实 API 联调已通过，具体范围见 [当前状态](当前决策与待办清单.md#真实-api-联调2026-10-20)。完整项目复审仍未通过，联调结束后没有后台付费调用。

```powershell
# 离线导出稳定 ID 和模板；复制模板填写反证，保留原文和 ID
python scripts/workflow.py finding-prepare '<complete-direct-report.json>'
# 反证 items 每项含 id、argument、evidence；引用包含 path/start_line/end_line/quote
python scripts/workflow.py finding-prepare '<complete-direct-report.json>' --rebuttals '<counter-evidence.json>'
# 以下是付费步骤，须明确恢复调用后才执行，不自动重试
python scripts/workflow.py finding-run '<finding-directory>' --budget 100000 --seconds 600
# 离线核验、派生结果并关联当前任务，然后单独执行原有 gate
python scripts/workflow.py finding-apply <change-id> '<finding-directory>'
python scripts/workflow.py gate <change-id>
```

原始响应和问题不改写。独立结论必须精确覆盖全部待复核 ID；只有有冻结源码精确引用的非缺陷结论可解除对应阻塞，真实缺陷、证据不足、新输入修复及过期证据继续阻断。gate 从原始审查、反证、请求及原始复核响应重算，不信任手填的派生 pass。预算是单次调度约束，不是账户硬费用封顶，模型也不提供形式化正确性保证。失败或中断后须显式重新 prepare 生成新尝试，旧证据保留，不后台重试。当前未将该本地路径部署成远程误报裁决服务。

首次基线建好后复用工具、hook、CI 与保护，不重新建设。普通小变更引用现有方案；新模块或关键契约变化才补方案和对应真实验证。本地适用检查与暂存 hook 不调用模型。推送 codex/ 功能分支后，PR 侧执行正式独立 Review 与受信 CI；本地付费预审按需要使用，避免默认在本地和服务器重复花两份模型额度。修复后只重审本次差异及关联单元，缓存不变的通过结果。没有执行本地 API 时，不填写本地 gate 已通过；远程合并资格由真实服务器检查与保护决定。

旧流程设计、普通模板和讨论沿革见 [历史归档](../archive/README.md)，当前操作只维护于本指南；文档归档与 OpenSpec 能力归档是两回事。

2026-10-20 继续完善现有工作流时，先读 [后续完善交接](AI工作流后续完善交接-2026-10-20.md)。此前付费审查已停止；先离线核对当前输入与剩余事项，不因恢复任务或剩余历史预算自动再次调用模型。

服务器审查缓存采用 `cache-mode: read` 和 `actions/cache/restore`，不在 pull_request_target 中发布 PR 证据缓存。GitHub 按 ref 隔离缓存，普通 PR 的 merge-ref 缓存不能由 main 恢复，见 [官方缓存边界](https://docs.github.com/en/actions/reference/workflows-and-actions/dependency-caching#restrictions-for-accessing-a-cache)。当前没有部署可信服务器缓存写入器，因此服务器缺缓存时正常审查，不宣称跨 PR 复用已验证；本地单元复用仍由实际请求身份严格核验。

预算估算经 15 个精确请求身份和原始 usage 校准：观测每个输入 token 对应约 3.803～4.354 个 UTF-8 请求字节，当前按 3.5 字节估算输入并加完整输出上限；仍逐次核对实际 usage，不把估算当供应商硬账单上限。

2026-10-20：ai-review 环境、main 分支访问限制及加密环境密钥已配置；实际 runner 访问、Actions、required checks 和 PR 阻断仍未验证。完整复审因结构错误、误报及思考截断未通过。随后已实现 [独立误报复核](独立复审意见处理-2026-10-20.md) 并完成小样本真实 API 联调；该结果不批准整个项目，也不表示远程复核入口已经部署。重新付费前先预览当前输入和剩余预算，不自动增额重试。

## 2026-10-21 日常精简策略

用户已同意保留测试并精简流程；review_policy_version=2 仅改变新包，历史包按冻结策略核验。本轮不恢复此前百万级模型调用，不宣布当前建设已通过独立复审。

日常路线：开发时按影响检查 → 最终输入 verify 一次 → review-plan 预览 → 显式不超过 100000 tokens/600 秒的 review → 必要的同轮 finding 裁决 → gate → 快速提交检查 → 推送后 CI 全量检查。不默认在本地先手动 ci 再重复 verify。当前新回归用于验证精简边界，不删除既有测试。

上下文保留全部模型目标文件及任务依据；Python 仅带直接本地导入、直接调用方和明确路径引用，context.json 可用 review_paths 指定额外源文件。未知动态导入或超限先拒绝，不能截去内容冒充覆盖；此分析不等于完整程序调用图，原生头文件和其他动态关系需要方案声明补充。

仅 docs/README.md、docs/research/、docs/archive/ 内 Markdown 可走确定性文档检查；AGENTS、规范、ADR、架构、工作流规则、当前变更提案和删除材料继续独立审查。报告的 checked_without_model 与 reviewed_files 分开，前者不宣称模型审阅；缺少当前完整验证或覆盖被改写仍阻断。

预算从首次真实付费请求开始按完整源输入 state（HEAD、索引及文件摘要）共享；换 change 或 packet 不能刷新同一输入的预算、截止时间和 pending 阻断，审查和裁决也不能分别重置。旧包已付费/未知的 packet-local 账本须先核对，不能静默重开。截断/错误不自动重采样，未知在途费用阻断后续付费；原始响应与实际 usage 保留。账本与哈希仍只是本地证据，不能提供远程身份或供应商账单硬封顶。


## 独立 Codex 上下文的收尾审查

大批历史累计输入不适合重复付费重跑时，可以由统筹实际启动全新只读审查上下文，使用 agent-pack 分配具体目标。先 verify 当前 workspace 输入；审查者读取 packet 的冻结源码、差异和当前任务依据，并写 ar.independent-context/v1 原始 JSON：fingerprint、assignment、reviewer、independent_context=true、status=complete、reviewed_files、findings。每条意见包含 path/line/level/content；不得提交自行 resolved 的意见。审查者返回后，统筹核实上下文、覆盖和原始结果，agent-handoff 记录真实结果及证据路径。

```powershell
python scripts/workflow.py review-context simplify-workflow-review --coordinator /root --evidence .ar-workflow/agents/<id>/review.json
python scripts/workflow.py gate simplify-workflow-review
```

多个证据路径接在 --evidence 后；合并覆盖必须完整，任一意见阻断本轮通过。导入不会启动模型、不会补造遗漏的审查、不会消除旧意见，不能把实现者自己写的 JSON 当独立审查。gate 从保留的原始审查和角色交接快照重算；本机记录仍不是远程身份认证。当前角色包绑定 workspace 字节与索引，导入使用匹配的 workspace 任务；不要在审查期间改源码或暂存选择。修改后必须准备新输入并重审。日常小任务仍可使用预算内 Flash/High API。
