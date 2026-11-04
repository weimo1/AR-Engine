# 技术方案

## 授权与依据

按proposal及维护请求实施，复用现有assignment、relay、operation_lock。保留既有未提交修改与冻结证据。

## 主责与模块关联

workflow；无引擎/P0修改。完整设计准备见本机维护请求旁 implementation-plan.md，本文规定实际实现边界。

## 实现与取舍

1. 新格式包增加package_format与frozen_documents清单，basis/按原始字节冻结。task.md仅短说明及manifest/必读文件入口；manifest单独列用途/批准范围/字节/哈希和最大贡献。包检查从inspect/relay/Review共用，DSH使用经校验首页。旧格式保留。
2. Agent准备只过滤其声明写路径内的changed_names；context.build和正式Review路由不降低。公共规则与当前契约仍存在完整冻结清单，参考按任务读取；离线转交清单明确需携带哪些文件。
3. agent-transfer只生成本地说明，未分配时只显示relay下一步。agent-accept留下显式本地领取回执，验证agent、输入、包及活动状态；没有网络发送能力，不制造sent状态或完整阅读证明。
4. temp-create仅在专属temporary/<UUID>/payload创建文件并登记所属活动assignment。temp-preview枚举本工具记录，默认不移动；仅completed任务可成为候选，writer/未确认DSH运行/读取不完整或任何引用均阻断。未知旧文件不自动登记。
5. temp-archive显式提供预览计划，每次重新生成并比较；源/目标均校验全部路径组件，计划绑定根/源内容/当前workspace输入与记录；只移动普通文件至同目录archive，保存恢复journal。temp-restore核对journal、内容、目标和进程后恢复，不覆盖。只处理单文件payload，避免目录递归删除；中断记录可按实际文件位置恢复。

## 任务上下文与协作

独立context声明相关scripts/tests/docs及本change允许路径；用真实developer写包，交接后共享ci。没有正式独立Review不得声称gate。统计由统筹串行，维护不并发写token汇总。

## 验证与恢复

使用owned临时仓库，覆盖v1/v2及缺项篡改、DSH实际prompt、Review接入、原生路由、领取回执、writer/输入变化、预览无修改、引用/未知进程/路径攻击、过期计划、归档恢复。实际历史清理只允许预览，不为测试移动真实证据。
