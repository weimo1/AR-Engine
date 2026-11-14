# P2b 技术方案

主责为 RHI。资源绑定归设备，呈现帧上下文提供完成状态。示例负责传入材质索引和验证画面，runtime 继续组合现有会话。长期职责见 [RHI 模块](../../../docs/modules/rhi/README.md)；着色器数据契约见 [Shader 规范](../../../docs/standards/Shader编译与数据契约规范.md)。

## 设备与表

仅在显式启用时查询并启用 `shaderSampledImageArrayNonUniformIndexing`。选择固定大小、完整初始化的 sampled image 和 sampler 两个数组。首批容量作为 C++ 与 GLSL 共用的构建常量，以设备的每阶段和每 set 上限及每阶段资源总数约束，不匹配时返回具名诊断。两类槽的 0 号保留给设备持有的默认纹理视图和 sampler，初始状态将默认资源写入每个槽。路径使用 Vulkan 1.3 Shader 目标；特性语义见 [Khronos Descriptor Indexing 指南](https://docs.vulkan.org/guide/latest/extensions/VK_EXT_descriptor_indexing.html)。

RHI 在 `src/vulkan/binding/` 组织私有绑定表，持有描述符池、布局、set、默认资源及槽位引用。`DeviceResources` 管理底层 Image/View 的存活和布局，`Presentation` 在录制前协调发布并提供图形提交完成状态。runtime 和示例使用带代数的绑定句柄。创建失败按反向顺序回收，表满或句柄无效时保留原映射。普通清屏和三角形使用原管线路径。

T14 创建设备默认纹理时，先用现有 staging 路径写入已知像素，再完成 transfer 写入到 fragment 采样读取的依赖及 `SHADER_READ_ONLY_OPTIMAL` 转换。转换完成后才初始化描述符表，保证 0 号及所有空槽首次可见时均指向有效资源。T15 用户纹理复用这一可采样就绪条件。容量按普通 descriptor set 的 sampled image、sampler 和每阶段资源限制检查。

## 像素验证

在现有示例中增加显式的绑定验证模式。上传完成并建立 transfer 写入到 fragment 采样读取的同步与图像布局转换后才发布映射。GLSL 对可能非一致的两个索引使用 `nonuniformEXT`。同一次绘制提供分区索引，用两张不同纹理和一张有相邻异色 texel 的纹理验证寻址；固定采样坐标让两个 sampler 的差异可从读回像素区分，并独立检查默认槽。

## 多帧与回收

```text
释放 CPU 槽位句柄 → 立即拒绝旧句柄 → 保留描述符及资源引用
                                    ↓ 两套帧的相关 fence 均完成
                             写回默认映射 → 释放引用 → 槽位可复用
```

槽位按 `Free → Pending → Published → Retiring → Free` 转换。注册取得 `Pending` 句柄，发布后才可解析为 Shader 索引。取消尚未发布的注册时，原描述符仍是默认值，可解除引用并回收该槽。释放已发布的句柄立即使 CPU 查找失效，原描述符与引用保留到安全写回默认值。复用产生新代数。材质保存句柄，在本帧录制时解析索引。

首版使用一个共享 set。首批默认资源在表首次使用前写入，运行时变更集中于录制前的帧边界。先确认所有引用该 set 的相关图形提交已完成，再发布就绪变更、写回退役槽并录制新命令。更新使已有引用该 set 的普通命令缓冲失效，后续提交使用重录的命令。未就绪注册保持 Pending。帧边界没有可发布变更时沿用原提交节奏。CPU 请求退役不等待 fence。同步依据见 [Vulkan 描述符更新规范](https://docs.vulkan.org/refpages/latest/refpages/source/vkUpdateDescriptorSets.html)。

注册从 Pending 起持有 view 或 sampler 引用，Published 和 Retiring 延续引用。存在槽位引用时销毁 view 或 sampler 返回 `InvalidState`。Image 继续沿用有存活 view 时拒绝销毁的规则。退役先写回默认映射，再解除引用和开放槽位。关闭按已有超时和设备丢失规则收敛在途提交，然后释放表及引用，最后释放底层资源。

T15 注册用户纹理前完成上传和采样布局转换。绑定表按 view 的底层 Image 持有引用，同一 Image 的多个 view 或槽位共同计入。任一引用存在时，`DeviceResources` 的 `uploadImage` 和 `readbackImage` 返回 `InvalidState`，在创建传输工作、转换布局和提交之前拒绝。取消 Pending 注册或完成退役后解除对应引用，最后一个引用解除后恢复既有传输条件。像素验收读回独立的渲染输出。

## 诊断与复现

通过现有 `core::DiagnosticSession` 输出，沿用运行标识、事件 ID、级别和预算。现有 `Event` 使用 `message` 承载细节，本切片以稳定的 `key=value` 项记录关联信息。RHI 绑定表负责记录映射状态，呈现层提供帧与提交关联，示例负责记录复现输入和像素坐标。

| 级别 | 记录时机与内容 |
| --- | --- |
| INFO | 表初始化成功时记录绑定路径、容量、默认槽、启用能力及关键设备上限；结束时汇总发布、退役、复用及发布等待次数。 |
| DEBUG | 显式启用后记录槽位注册、发布、退役、复用和发布等待。记录表类型、槽位、代数、资源句柄与相关帧/提交标识。发布记录包含修改前后映射。等待记录包含原因及尚未完成的提交。 |
| WARNING / ERROR | 表满、非法句柄、未发布访问和销毁被拒绝沿用项目错误分类，附阶段、请求值、当前状态和可用的 Vulkan 原因。Validation 消息继续经现有诊断会话转发。 |

示例启动配置是诊断入口。用现有 `DiagnosticSession` 配置 INFO / DEBUG 和输出，RHI 通过借用的会话发事件。启动时用 INFO 报告实际生效的级别、输出位置或输出方式、Validation 状态和本次启用的复现选项。配置在启动时生效，示例说明记录开启方式以及到哪里看输出。

默认 INFO。选择 DEBUG 只增加 CPU 侧追踪。T14 沿用现有 Validation 策略，由 Debug 构建创建图形设备时启用。缺少所需层或扩展时沿用创建设备的失败路径。T14 在同一 Debug 构建下切换 INFO／DEBUG，核对实际 Validation 状态始终为已启用。GPU-AV、抓帧和额外读回按复现需要独立启用，沿用已有接入能力。正常帧没有诊断专用提交、读回或等待，槽位和资源的安全约束始终生效。

`enabled(LogLevel::Debug)` 检查通过后才组装详细消息，追踪集中在状态变化并使用现有日志预算。复现时先查 INFO 确认配置，再用 DEBUG 对照槽位、代数、资源和提交。像素读回失败时记录坐标、预期及实际值，以及对应材质句柄、本帧提交给 Shader 的索引、资源与帧标识。验证配置与性能测量分别记录。

T16 真机验证先提交两个使用旧映射的帧，记录各自读回复制，再请求退役并统一收集结果。测试路径用各自独占的读回空间，避免现有同步截图等待提前串行化这两次提交。GPU 可能提前完成，完成前禁止写回与复用的性质由 CPU 受控完成状态测试证明。
