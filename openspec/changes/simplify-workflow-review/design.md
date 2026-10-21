# 技术方案

用户已授权实施本次精简。新策略版本写入工具锁，历史缺字段按原逻辑核验。Python AST 解析直接导入和明确脚本路径，同时收集直接反向依赖；C++ 本地 include 及配置引用采用路径匹配，动态 Python import 无法解析时拒绝，任务 review_paths 可补充明确文件，不宣称完整程序依赖分析。

预算以完整源输入 state（HEAD、索引及文件摘要）为轮次，在 reviews/paid-rounds 的忽略目录记录首次请求时间、已知用量及在途状态；同一输入更换 change/packet 仍共用账本，API 审查和裁决也复用；旧 packet-local 已付费/未知记录必须显式核对，不自动开启新预算；严格等待截止时间，失败不自动重发。小任务输出上限 32768，保留 High；截断停止而非升至百万级。

提交快检从 Git 索引读取全部实际 blob，导出任务自有临时目录执行既有文档/派生检查及 Python AST 解析，不运行 unittest。完整 verify 与 CI 不变。

普通文档仅限 docs/README.md、docs/research/ 与 docs/archive/ 内 Markdown，规则/架构/ADR/当前任务/删除审查材料保持模型目标。报告区分 checked_without_model 与 reviewed_files，冻结 manifest 和 gate 重算二者，确定性文档不宣称独立模型批准。

旧角色、DSH 和提交格式变更仍未发布，保留已有暂存内容。本轮不运行付费 Review，不通过降低规则放行当前 40 余文件的大批输入。


## 独立上下文接入（用户授权推进至第 7 步）
使用 review-context 离线导入角色包关联的原始 JSON，保存输入指纹、逐文件覆盖、具体意见、只读任务包和 completed 交接快照；导入时核对角色包摘要、真实证据路径及当前输入，gate 从保存的原始记录重算。任何意见均保留 open，修复后重审，不允许实现者自行消除。该入口不伪装 OCR 或 DeepSeek 响应；不调用付费 API，不授予远程身份。首次使用 workspace scope，与角色包逐字节输入一致；若使用 staged scope，只有角色输入完全相同才可接收，不能将近似内容视为相同。

删除审查材料按 manifest.deletions 映射原路径，将已删除路径作为空的依赖图节点保留，供当前导入/引用反查；不会把历史正文当可执行当前源。角色写入范围对 Windows 大小写及尾部点/空格归一化后拒绝 Git 与内部证据路径。
