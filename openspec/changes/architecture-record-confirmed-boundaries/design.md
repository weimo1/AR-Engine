# 文档整理与修复方式

状态：A/B/C记录已写入；2026-10-27修复只读预审发现的材料不一致。范围和验收唯一源为proposal，不重复批准架构。

## 文档契约与单源

本变更采用现有ar-engine契约块和derive机制，契约描述文档确认追溯、范围、职责和证据，不虚构引擎运行时capability。派生spec位于本change内，不提前进入openspec/specs；不修改workflow脚本或增加文档豁免。

总体架构维护A/B/C确认原文、跨模块关系、主图与数值；模块README简述职责并引用对应锚点，避免复制另一套数值契约。proposal/context/tasks/test-plan覆盖2026-10-23/24/26全部记录及本次修复；B4原为线索的记录通过C1解释后续确认，不重写历史。无可靠消息链接和精确时间如实说明。

## 依据与职责

context显式列出总体架构、scene/render及适用入口。总体架构原已由native policy隐式加载；显式声明采用同一reference/basis以避免重复冲突，不把“未显式列出”等同于历史包绝对缺失。以实际context输出和包内容为证据。

R1—R4均为architect建议，待用户确认；具体建议只维护于总体架构建议表，proposal登记来源与确认边界。本契约要求区分已确认与建议，不强制采纳四项技术/维护建议。A/B/C原确认保留：render内编排、rhi执行；scene权威、runtime提取、render派生；GPU Driven基础闭环前移；B节原数值和候选状态不变。相机字段、提交协调及完成职责不从这些粗粒度边界自动推出批准。

## 状态与协作

三次历史architect交接已完成，tasks 1.4依据实际handoff收尾；本轮修复单列，不将历史局部自检继承为新输入的共享检查或独立Review。

此前b516修复的操作与证据保留为历史，本轮由统筹分配bbd7ec0ded624ece9b2327ae唯一writer，执行前核对实际任务输入并保存起止哈希及原文。不复用旧交接批准新输入，不修改旧角色包，也不回滚其他任务。本包仅修文档及派生spec，不改P0、代码、工具或Git。

写入期间运行derive、context、check、OpenSpec严格校验及文档/范围检查作为自检。完成先handoff释放writer；随后由统筹冻结输入、记录哈希并安排check/ci/OpenSpec验证和未参与写作的独立reviewer。本轮执行者不建立正式审查或gate结论，旧CI不放行新输入。

## 验证与限制

检查A/B/C与变更材料、实际context主依据、历史handoff、全部新增/未跟踪文件、相对链接及本次允许路径。P0起止哈希保持不变。check/ci包含派生契约与适用回归，不代表实现/GPU验收；独立Review与gate待统筹安排。关键确认、完整文档或实现未获批的部分继续保持原状态。
