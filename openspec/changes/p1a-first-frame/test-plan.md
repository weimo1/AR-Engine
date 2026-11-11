# 验证计划

| 验收要求 | 输入与命令 | 通过条件 | 失败与限制 |
| --- | --- | --- | --- |
| T01 窗口生命周期 | `python scripts/build_checks.py --fast`，收尾运行 `python scripts/build_checks.py` | `platform-window-create` 隐藏创建、轮询、读尺寸和状态、拒绝非法尺寸及重复创建、关闭后可重开；`public-headers` 通过 | 不操作用户窗口，不验证可见窗口交互 |
| T01 Vulkan 扩展 | 本机完整门禁中的 `platform-window-vulkan-extensions` | 扩展名非空，包含 `VK_KHR_surface`，销毁窗口后副本仍有效 | 需要真实 loader；登记到 `gpu_cases`，云端 CPU 不执行 |
| T02 逻辑设备和诊断 | 本机门禁中的 `rhi-device-create`，以及 fixture 用例 `rhi-device-missing-feature`、`rhi-device-selection` | 真实设备能创建和释放；Debug 已启用 Validation 且日志无错误；fixture 验证缺能力拒绝、设备选择和诊断 | 真实设备用例登记到 `gpu_cases`；本批无 surface、GPU 提交及呈现 |
| T03 清屏与有限帧退出 | `sample-clear-frames` 运行 `ar_triangle --frames 60 --hidden --clear-only` | 60 次成功呈现，退出码 0，诊断包含呈现初始化及清理完成，Debug 启用 Validation 和同步检查且无错误 | GPU 标签，真实窗口、交换链和提交，在专属构建输出目录写诊断 |
| T04 Shader 与三角形 | `shader-build` 用 spirv-val 验证两阶段构建产物；`sample-triangle-frames` 运行默认三角形示例 | SPIR-V 符合 Vulkan 1.3；成功建立管线并呈现 60 帧，Debug 零 Validation 错误 | 仅呈现用例标 GPU；像素验证在 T05 |
| T08 Vulkan 1.4 与 Heap | `rhi-device-vulkan14-heap` 真机、`rhi-device-enhanced-unavailable` 和 `rhi-device-selection` fixture | 真机支持时启用 Heap、读限制并检查入口，Debug 日志无错误；不支持时明确拒绝；相同主次版本的 1.3 loader 和设备仍兼容不同补丁号 | 真机用例登记到 `gpu_cases`；验收记录区分实际启用和不支持分支，Heap 内存绑定及 Shader 读回另验 |
| T05–T06 截图与窗口变化 | [任务队列](../../../TASK.md) 中对应 CTest 和本机完整门禁 | 截图和重建逐项满足队列断言 | 云端 CPU 不能验证实际呈现 |
| T07 人工验收 | 用户运行三角形示例 | 用户确认画面和窗口行为 | 由用户决定进入下一阶段 |
