# P2b 验证计划

| CTest | 输入与观察 |
| --- | --- |
| `rhi-set-binding-limits` | CPU 控制能力和容量输入。检查缺特性、上限拒绝、表满与原槽有效 |
| `rhi-set-binding-device` | 真机显式启用绑定路径，默认纹理完成上传及采样布局转换后初始化整表。用同一 Debug 构建分别以 INFO 和 DEBUG 创建图形设备，核对两次 Validation 均按现有策略启用、创建及关闭无 Validation 错误，且只有 DEBUG 输出逐槽追踪。INFO 可核对实际诊断配置、表容量、默认槽和能力 |
| `sample-set-binding-pixels` | T15 比较不同纹理、不同 sampler 和默认槽像素，读回独立渲染输出。用同一 Image 的两个 view 占两个槽，验证 Pending 与 Published 时上传和读回均返回 `InvalidState`，无布局变化和传输提交。失败信息包含坐标、预期/实际像素、材质及索引 |
| `rhi-set-binding-retirement` | CPU 控制两个完成状态。核对 Pending 不可解析、旧句柄失效和被引用资源的销毁拒绝。只完成一个时不得写回和复用，全部完成后恢复默认映射并以新代数复用 |
| `sample-set-binding-retirement` | 真机先提交两帧，各使用独立读回空间，再请求退役并收集结果。核对旧帧像素与复用后像素、同步 Validation 和关闭。同一 Image 的两个 view 共用传输保护，Retiring 及仅解除一个引用时均拒绝传输，最后一个引用解除后恢复原有上传和读回条件。启用 DEBUG，核对同一槽位的代数、资源、提交完成与复用的先后关系；实际发生等待时核对原因及未完成的提交 |

真机用例登记为 `gpu`。先运行新增用例取得预期失败，再跑 `python scripts/build_checks.py --fast` 与完整门禁 `python scripts/build_checks.py`。两配置均回归 P1/P2a。完整门禁通过后按开发规则进行 DeepSeek 独立评审。跨设备性能和 Descriptor Heap 等价性由后续阶段验证。
