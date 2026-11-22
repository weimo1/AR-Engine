# P5 第一批验证计划

| 批次 | 用例 | CTest 标签 | 证据与覆盖 |
| --- | --- | --- | --- |
| T37 | `rhi-descriptor-heap-layout` | CPU（无 `gpu`） | 纯 CPU 校验对齐、步长、Resource/Sampler 分区偏移、保留区边界、整数溢出与超上限拒绝、槽位注册/退役/代数复用；复用 fixture 验证缺 API 1.4、缺扩展、缺 `vkCmdPushDataEXT` 入口时的明确拒绝与零残留 |
| T38 | `rhi-descriptor-heap-readback` | `gpu` | 真机 Vulkan 1.4 + `VK_EXT_descriptor_heap`：分配对齐 Heap、验证 `heapRange.address % alignment == 0`、写入默认槽、非均匀测试纹理及非零槽 linear sampler；使用非纹素中心 UV（如相邻异色 texel 交界处）分别以槽 0（nearest）与非零槽（linear）执行 `heap_probe.comp` 读回 RGBA 并断言两者数值可区分且逐通道匹配预期，Debug Validation 零错误 |
| T39 | `rhi-descriptor-heap-retirement` | CPU + `gpu` | CPU 受控完成状态验证 fence 未完成前不覆写、不解除引用；真机验证提交后立即退役拒绝旧句柄、底层 Image 拒绝销毁与上传/读回、fence 完成后覆写默认描述符并安全复用新代数槽位、按序关闭零残留且 Debug Validation 零错误 |

说明：
- 所有新增用例登记到 `workflow/p0-checks.json` 的 `expected_cases`，真机用例另加入 `gpu_cases` 并标 CTest 标签 `gpu`。
- 既有 86 个用例（含全部 Vulkan 1.3 Descriptor Set、P3c、P4a 和 T34–T36 用例）必须保持全绿、零告警。
- 本机无 1.4 Heap 设备时，CPU/fixture 用例仍须全过；真机用例明确报告设备限制而不伪称通过。
