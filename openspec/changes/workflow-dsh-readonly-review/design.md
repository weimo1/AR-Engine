# 设计

新增read/reviewer分支，writer必须空闲。保留write分支与原有防重复/源身份/冻结清单校验。reviewer使用v2完整冻结依据，目标范围由task-goal明确；不将当前change计划审查等同于整个混合工作区审查。

launch增加可选reviewer模式：sandbox-policy=read-only、tool-fs read=true/write=false/edit=false，禁用terminal-bash/terminal-pwsh/persistent-bash/persistent-pwsh。实际已安装SDK包声明提供该策略；Windows不宣称OS隔离。reviewer输出正文，由桥接器写自有ignored证据。新会话隔离开发者推理；不自动将正文变成正式review.json。

初始化无模型请求，prompt前重查writer/包/输入；完成后重查review输入。记录role/access/change、权限模式、发送状态及冻结文档摘要。保存沿用官方persistence overlay。保存不等于续聊；另查SDK恢复契约并记录后续任务，不在本次引入长期会话调度。
