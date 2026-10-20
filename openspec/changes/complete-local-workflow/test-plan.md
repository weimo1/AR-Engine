# 验证计划

2026-10-20 本轮误报处置修复：新增 test_workflow_findings.py 的 12 项离线回归，完整工作区 ci 共 59 项测试通过，文档/契约与 OpenSpec 严格校验通过。覆盖非缺陷派生但原始意见不变、真实缺陷/不确定/新输入修复仍阻断、缺失/重复/未知发现 ID、引用不存在或原文不符、源与反证/原始响应变化、截断与模型不符、预算不足不读取密钥/不发请求，以及实际 task_gate 的原始证据重算和手改 pass 拒绝。夹具结果不代表真实模型已经完成项目误报复核。本轮未调用付费模型、未提交或推送；当前工作区更改尚未纳入旧暂存快照，不将本次本地检查冒充当前发布批准。

后续补齐测试：实际 OCR preview 覆盖修改加删除、纯删除、改名、暂存删除与未暂存恢复、删除材料被改/遗漏；direct-api 的截断、错误模型、重复/缺覆盖、原始响应变化、预算不足、未解决发现及输入变化均拒绝。HTTP 使用可控夹具测试，真实模型复审单独记录用量。远程测试使用 codex/ 临时验证分支和 PR，故意失败只放在该分支，不合入 main。

- 适配器：完整无发现通过；缺覆盖、重复 item、partial、未知版本/模型/规则、预算与警告、不完整原始结果、存在发现均阻止。
- 状态：真实 start/verify/review/gate；中断记录可恢复，修改源和规则后旧结果失效。
- Git：暂存坏文件后仅修复工作区，commit-check 和 hook 仍失败；正确暂存后通过。使用临时仓库，不修改主项目失败输入。
- API：已知除零缺陷及修复版本各跑一次真实 DeepSeek OCR；验证缺陷阻断和修复解除，明确该样本不是完整 Vulkan 检出率评估。
- 共享入口：项目检查、现有测试和新增测试、所有在途变更 OpenSpec 严格校验；不声明 GPU/引擎编译通过。
- CI/远程：配置存在、实际运行、保护强制生效分别记录；权限不足时保留具体缺口，不造绿色占位。

## 2026-10-20 实际执行

以下 26 项与早期样本是本轮早期输入的历史记录，不能代替新增回归后的当前输入验证；测试数量和当前结果以对应 packet 的 verification.json/原始响应为准。任务文件勾选只对应条目中指定的实现/测试，不包含未完成的独立复审或远程保护。

- 共享 ci：26 项 Python 测试、项目链接/派生检查及全部在途 OpenSpec 严格校验通过。新增覆盖空/全跳过测试集、OpenSpec 缺项、原生源摘要、未知模型/规则、规则更新前拦截、部分提交的真实备用索引、异任务 packet 与异常清理。
- 临时 Git 仓库真实 pre-commit：暂存错误但只修复工作区时被拒绝，暂存修复后提交成功；主项目未注入错误和未创建提交。本仓库 core.hooksPath=.githooks。
- 真实 DeepSeek 负样本：除零被报告 critical，退出码为 0 仍阻断，9,339 tokens；修复样本 complete/pass，2,854 tokens。样本不足以估计整体精确率或 Vulkan 检出率。
- 真实任务夹具：完整检查、单文件原生审查与 task_gate 通过；审查 22,503 tokens。随后改检查日志被拒绝，改源并恢复后旧验证/审查失效。结果在忽略目录 .ar-workflow/task-gate-e2e-results.json。
- 多文件准入试验：发现 planner 在预算检查前消耗 664 tokens，已去掉所谓 budget=1 零请求准入，使用离线规则锁；保留原始失败证据，不冒充审查。
- 新增实现独立审查：相对上一轮冻结副本选择 15 文件。调度预算 200,000，实际 239,025 tokens，12/15 项失败并返回 partial，报告 request_changes；执行器拒绝。单次调度预算不能等同账户硬费用封顶。

## 独立审查意见复核

原生报告位于忽略目录 .ar-workflow/local-change-review-r2/ 下；未手改该报告为通过。

1. OCR-001：未采纳“用户改回运行状态/复用 api 目录会放行”的推测。api 目录为随机唯一标识，中断不产生 finished run.json，原生不完整、缺覆盖和解析失败都会拒绝；恶意重写全部本地证据不在本地身份认证承诺内。没有据此增加伪安全标志。
2. OCR-002：加强 task/input、packet、scope 的显式绑定并新增异任务输入回归；原有当前工作区状态核对仍保留。
3. OCR-003：修复零测试可能退出零的问题，拒绝空/全跳过套件；核对 OpenSpec JSON 全集合/计数/有效状态；gate 核对三条预期命令及日志摘要。
4. OCR-004：恢复时提供明确 next_action，区分已有有效验证可显式重试 review 和需要先 verify；不自动调用模型。
5. OCR-005：锁清理遇到文件消失或损坏时保留原始执行异常；已有故障注入回归。
6. OCR-006：临时记录清理异常不覆盖原子替换的原始失败。
7. OCR-007：未采纳清除源仓库 GIT_INDEX_FILE 的建议。它是部分提交使用的实际索引，必须保留；仅对冻结副本清除 hook 导出变量。新增真实备用索引回归证明复制的是指定提交内容。

2—6 的修复尚待新的完整独立复审，不能以实现者复核代替通过。服务器 Actions、required checks、独立批准、分支保护和首次发布均未执行；删除覆盖仍明确阻断。变更保持在途，不归档。

## 后续补齐与当前边界

上段是早期输入的结论，文本删除覆盖现已实现并有原生 OCR 预览回归。新增单元缓存、零模型 review-plan、受保护的远程策略身份与内核锁；完整独立复审仍待通过，不允许用本地回归代替。已将“本轮预算耗尽/截断”与“测试通过”分开记录，未提前提交或归档。

思考模式样本证据：.ar-workflow/thinking-probe-53c024fdd1/result.json，实际 900 tokens，正确检出 bad_ratio.py 且未误报 good_ratio.py；样本不是全仓审查。用户追加的本轮总预算为 3,000,000 tokens，预算台账 .ar-workflow/review-budget-2026-10-20.json。

## 2026-10-21 当前输入复审

模型切换：连续多轮显示 deepseek-flash 的行号锚点漂移（如把 run() 尾部报成 196 行、把 197 行的文件报到 224 行），裁决时 45 条证据里 15 条引用无法与冻结源码逐字匹配，导致 finding-run 一律失败。改用 deepseek-v4-pro 后先对最小单元做单发探测（8,359 tokens，覆盖完整、2 条意见锚点均在文件行号内），再执行整仓复审。切换记录见 AGENTS.md、docs/workflow/SDD与CodeReview接入指南.md。

当前输入复审（packet staged-d4a65de4e2732ea9bc8b861c，目录 direct-502fad32ecaf）：15/15 单元完成，实耗 305,097 tokens，报告 request_changes，12 条 findings（1 blocking / 9 must_fix / 2 suggestion）。逐条核对结论全部为误报或推测，关键反证：

- test_workflow_deletions.py:58：validate_packet 返回错误列表，assertTrue 断言的是“检出篡改”，对应 prepare() 中的 assertEqual([], ...)。
- test_workflow_findings.py 的导入与 batches 假设：setUp 调用 fixtures.ApiReviewTests.setUp(self)，模块由 discover 从同目录导入。
- workflow_review_api.py:63-68 与 :119-127：上下文文件先经 is_file() 过滤，native_code 分支剔除渲染上下文。
- workflow_review_github.py:51-53：mkdir(parents=True, exist_ok=False) 是刻意的 fail-closed 行为。
- workflow_review_api.py:320-323、workflow_runtime.py:319-327、mcpcall.py:69-70：分别用传入 root 校验、在写日志前抛错、裸 raise 传播。

裁决：finding-433c3b87496e46b99214b404，10 条非 suggestion 意见全部 not_defect，实耗 109,428 tokens（前一次 finding-1baca533119f4ffa8249189e 花 109,660 后因模型给出的引用范围与源码不符而失败，保留为失败证据）。finding-apply 输出 Finding disposition: pass；gate 输出 LOCAL DELIVERY GATE PASSED。本轮 2026-10-21 侧累计约 1,015k tokens（490,800 + 305,097 + 109,660 + 109,428）。

发布与 Actions：首次提交 f68da07（384 文件）经 pre-commit commit-check 通过并推送 origin/main。服务器实跑 workflow-checks（run 39456293164）在 python scripts/workflow.py ci 步骤失败，两个裁决门禁测试抛 Gate blocked: packet does not belong to current task input；根因是 CI 运行器把临时目录暴露为 8.3 短名，task_gate 用 str() 比较 packet 路径。改为两侧 resolve() 比较后，0568387 的实跑为 success（run 16:22:08Z）。该缺陷只在 Windows CI 路径表示下出现，本地 62 项测试不能替代这次 Server 实跑。

未执行且不宣称完成：required checks、分支保护、PR 失败阻断。用户 2026-10-21 明确个人开发暂不需要 PR 流程，故不做 PR 实测，3.6 保持未勾。

## 2026-10-21 补做 0568387 复审与重采样证据修正

复核意见指出两处缺口：已提交的 0568387 没有独立复审证据，重采样时被拒样本的响应与费用会被覆盖。本节记录两者的实际处置。

补做 0568387 复审的方法：该修复已在 6c60e99 之前提交，工作区没有对应差异，`verify --scope workspace` 只能得到 Empty/unknown OCR review set。于是在 f68da07（0568387 的父提交）上重建该提交的暂存差异——`git checkout --detach f68da07`、`git checkout 0568387 -- scripts/workflow_runtime.py`、`git add`——此时 `git diff --cached` 与 0568387 的提交内容一致（1 file changed, 5 insertions, 1 deletion）。该状态对应的 packet staged-33d0d7983ee3d22daf0bb22b 在 2026-10-21 00:21 已生成但从未复审，本次直接复用。复审后执行 `git checkout -f main` 恢复。

结果：1/1 单元完成，实耗 40,097 tokens，报告 request_changes，1 条 must_fix（scripts/workflow_runtime.py:445，称 `data.get("packet")` 可能为 None 导致 TypeError，并怀疑 packet 可由 report 路径越出仓库）。裁决 finding-77ed88d30b414d18b7de155f 实耗 41,254 tokens，结论 not_defect：`verify` 在第 402 行把 review 与 packet 同时置空，`task_gate` 第 442-443 行在 review 缺失时先阻断，不存在 review 有效而 packet 为 None 的输入；`.get("packet", "")` 在字段缺失时返回空串，`Path("")` 与真实 packet 不等，仍走同一 fail-closed 分支；`report_path` 第 445 行已由 `core.within(root, ...)` 约束在仓库根内。finding-apply 输出 Finding disposition: pass，gate 输出 LOCAL DELIVERY GATE PASSED。

重采样证据修正（scripts/workflow_review_api.py）：每次采样尝试写入 `attempt-<批次>-<序号>.json`，`batches[].attempts` 记录每次尝试的响应摘要、实际用量、是否被接受与拒绝原因；`actual_tokens` 为全部尝试之和，`report()` 新增 `batch_tokens()` 逐条核对尝试文件摘要并要求记录合计与证据一致。首次复审指出 run() 仍用只累计成功响应的 `total` 做批次预检，与计入被拒样本的 `spent` 分叉，可能低估已消耗额度，因此删去 `total`，预算预检、实际用量检查与输出统一使用 `spent`。回归 `test_malformed_sample_is_resampled_once_and_counts_one_batch` 改为断言两次尝试、24 tokens，新增 `test_attempt_evidence_and_usage_must_stay_consistent`（篡改尝试文件或合计不符即拒绝）与 `test_budget_precheck_counts_rejected_resample_tokens`（被拒样本计入后，下一批次预检即以 Remaining budget insufficient 停止且不再发请求）。本地 64 项测试通过。

## 2026-10-21 重采样证据复审与裁决引用容差

上一节的修正又经过两轮独立复审。

第一轮（packet staged-188713f1ec88e12155355c22，目录 direct-a7487988c6be，实耗 55,332 tokens，2 单元中 1 个复用缓存）返回 request_changes，4 条意见：两条 blocking 认为逐批预检用估计值、而重采样本身没有再做预算检查，单批次可能越过上限；一条 must_fix 指出 `batch_tokens()` 对标记 accepted 的尝试不校验 `actual_tokens`；一条 must_fix 指出 `cite(..., tolerant=True)` 遇到重复行时会在第一个不匹配位置 break，误拒合法引用。

对应修正：重采样前执行与首发相同的 `spent + estimates[number] > budget` 检查，超出即抛 Remaining budget insufficient 且不再发请求；`batch_tokens()` 要求 accepted 尝试的 `actual_tokens` 为正整数且等于该批次解析出的用量；容差匹配改为继续扫描后续出现位置。回归新增 `test_resample_also_needs_remaining_budget`（budget 15、估计 10、首个样本实际 12 tokens 后不再发第二次请求，urlopen 只调用 1 次）与 `test_repeated_line_citation_matches_a_later_occurrence`，并扩展 `test_attempt_evidence_and_usage_must_stay_consistent`；本地 67 项测试通过。

第二轮（packet staged-dc222683e859f5e6b2b8a5d8，目录 direct-a392602beabe）2 单元中 1 个复用缓存，实耗 56,263 tokens，decision=pass，0 条 findings；gate 输出 LOCAL DELIVERY GATE PASSED，提交 3cdbf6b。

裁决引用容差（scripts/workflow_findings.py）：v4-pro 的裁决证据文本逐字来自冻结源码，但标注区间常有几行偏移（例如实际位于 389-395 的引用被标成 384-389），严格按区间比较会让每次裁决都失败——4 次尝试（51,775 / 51,672 / 51,885 / 51,253 tokens）都因这类错位被拒，正文本身逐字正确。现在 `cite()` 的容差模式（只用于裁决者返回的证据）要求引用文本能在冻结文件中逐字找到，且出现位置与标注区间相距不超过 `CITATION_SLOP = 8` 行；找不到原文或偏移过大仍然拒绝。实现者自己的 rebuttals 证据继续走严格模式，不享受容差。

