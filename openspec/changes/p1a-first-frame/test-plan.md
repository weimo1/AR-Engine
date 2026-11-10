# 验证计划

| 验收要求 | 输入与命令 | 通过条件 | 失败与限制 |
| --- | --- | --- | --- |
| T01 窗口生命周期 | `python scripts/build_checks.py --fast`，收尾运行 `python scripts/build_checks.py` | `platform-window-create` 隐藏创建、轮询、读尺寸和状态、拒绝非法尺寸及重复创建、关闭后可重开；`public-headers` 通过 | 不操作用户窗口，不验证可见窗口交互 |
| T01 Vulkan 扩展 | 本机完整门禁中的 `platform-window-vulkan-extensions` | 扩展名非空，包含 `VK_KHR_surface`，销毁窗口后副本仍有效 | 需要真实 loader；登记到 `gpu_cases`，云端 CPU 不执行 |
| T02 逻辑设备和诊断 | 本机门禁中的 `rhi-device-create`，以及 fixture 用例 `rhi-device-missing-feature`、`rhi-device-selection` | 真实设备能创建和释放；Debug 已启用 Validation 且日志无错误；fixture 验证缺能力拒绝、设备选择和诊断 | 真实设备用例登记到 `gpu_cases`；本批无 surface、GPU 提交及呈现 |
| T03–T06 渲染与窗口变化 | [任务队列](../../../TASK.md) 中对应 CTest 和本机完整门禁 | 窗口呈现、截图和重建逐项满足队列断言 | 云端 CPU 不能验证实际呈现 |
| T07 人工验收 | 用户运行三角形示例 | 用户确认画面和窗口行为 | 由用户决定进入下一阶段 |
