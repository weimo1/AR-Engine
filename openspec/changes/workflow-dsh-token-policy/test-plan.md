# 验证

无费测试覆盖显式无token阈值与大usage、非法或冲突选项、原budget阈值保留、时限输出限制、read角色和developer回归；续接检查覆盖未知费用、角色/任务不符、活动writer、过期新包、缺收据及重复续接引用。

真实中断任务对照原始RPC、usage和desktop多frame日志，只核对不改写；不得把SDK退出、持久化终结或全部usage可见当作供应商账单已结清。共享CI在handoff后执行；独立Review/gate与实际恢复授权另行处理。
