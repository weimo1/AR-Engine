# runtime 模块

## 窗口与呈现

窗口会话由 runtime 持有 platform Window、诊断会话及 RHI。创建窗口后将原生句柄、instance 扩展名和帧缓冲尺寸传给 RHI。帧入口先轮询窗口事件，收到关闭请求就停止出帧。最小化、零尺寸或交换链需重试时等待窗口事件，并返回暂停状态。调用方只计数已呈现的帧。其余呈现失败记录为本次运行的主错误并停止提交。关闭时先关闭 RHI，再销毁窗口，最后结束诊断。如果 RHI 报告仍持有呈现资源，则窗口随呈现资源保留到进程退出。纯查询会话沿用无窗口路径。

P0技术方案修订：4，2026-11-06。组合接口按 2026-11-04 授权收口并已实施（见文末实现状态）；不增加游戏框架或P0帧循环。

状态：模块组合职责与目录已确认；2.1 runtime 组合已实现（2026-11-06），两配置各 49/49 用例通过。这里是长期设计入口。

- 目录：engine/runtime/include/runtime、engine/runtime/src；公开头不增加 ar 路径层。
- 职责：组合当前阶段需要的基础和图形能力；具体初始化、关闭、接口及所有权按已确认方案实施，不默认增加游戏玩法、编辑器或完整应用框架。
- 依据与方案：[总体架构](../../总体架构与技术路线.md)、[P0 proposal](../../../openspec/changes/bootstrap-engine-p0/proposal.md)、[P0 design](../../../openspec/changes/bootstrap-engine-p0/design.md)。模块边界不自动决定 DLL、namespace 或构建 target。
- 在途变更及执行入口：[模块与变更索引](../../workflow/模块与变更索引.md)。

跨模块生命周期和必要时序图在具体方案明确后按 [模块模板](../模块设计模板.md) 补充；图和实现不得假定其他未确认模块已经存在。

## 场景与渲染交接

2026-10-23已确认：runtime内的提取适配读取scene，转换为render输入；render不直接访问EnTT容器或保存组件地址。初期同步提取与调用，不据此批准独立渲染线程、快照队列或具体帧数。确认来源及跨模块主契约见[总体架构确认记录](../../总体架构与技术路线.md#2026-10-23-架构确认记录)A2、A4。

持久渲染记录、各视图与缓存由render管理；runtime负责组合与交接，不接管全部内部实现。未来线程交接、背压、借用期与错误传播待方案。资产详细设计延期，P0范围保持不变。

## Tick与任务编排的后续讨论

2026-10-26补记：固定核心阶段自动Tick、受控应用回调、声明式渲染pass和完成通知已获用户确认（C1）；具体阶段接口、执行线程和注册/注销机制仍待设计。worker并行录制不自动意味着独立渲染线程或跨帧场景队列。

runtime驱动核心阶段，应用在约定时点修改scene；提取交接后本次输入保持稳定。worker读取稳定输入，依赖计划在render内Graph生成、rhi执行；runtime组织这些阶段，不另行规划pass资源依赖。细化提交协调和完成/回收职责见R1待确认建议。取消及关闭原则已确认（C2），任务和接口实现、失败汇合细则仍待设计。已确认GPU Driven与帧配置见[总体架构B1—B4](../../总体架构与技术路线.md#2026-10-24-gpu-driven并行录制与dynamic-rendering确认记录)，不改变P0。

## 取消、切换与关闭

来源与完整主契约见[总体架构C1/C2](../../总体架构与技术路线.md#2026-10-26-tick生命周期graphshader与绑定确认补记)。取消请求、CPU结束和GPU资源退役分开；请求身份/版本拒绝晚到结果，已开始回调有结束或存活保障。停止入口及调度后汇合CPU使用，再满足GPU/呈现完成条件，按依赖释放模块，诊断最后关闭。初期场景切换采用保守顺序，重叠加载后续设计。设备失败不等待不可能出现的正常完成；具体异常清理未定，未实现验证。

A2/A4的scene权威、runtime提取、render派生边界保留。场景相机具体参数及关联变换、派生矩阵/历史的字段归属属于R2 architect建议，待用户确认；见[待确认建议](../../总体架构与技术路线.md#2026-10-27-待确认的职责澄清建议)。

2026-11-03 P0修订：先不建设独立ArProbe；模块静态库由CTest公共入口集成测试调用，P1再接独立渲染程序。公共入口、阶段范围和验收方式见 [P0 方案](../../../openspec/changes/bootstrap-engine-p0/design.md)。

## P0 启动、结果与关闭技术方案（候选）

### 接口与数据所有权

runtime负责把core诊断和rhi查询组成可调用会话，不解释每个Vulkan函数，也不把日志文本当成状态。能力、错误及摘要字段以[P0 proposal](../../../openspec/changes/bootstrap-engine-p0/proposal.md)为唯一源；本节只说明候选C++接口如何承接。

公开头拟为RuntimeConfig.h、RuntimeSession.h，放在include/runtime；RuntimeSession只以不透明类型和工厂出现，私有Impl不进入公开头。配置可以包含rhi公开值类型，通过真实PUBLIC依赖表达，不复制一套LoaderSource。构建身份从编入BuildInfo取得，不能作为任意调用方字符串覆盖；测试的预期sidecar仍独立核对。

| 候选类型/签名 | 所有权和语义 | 失败及有效期 |
| --- | --- | --- |
| RuntimeConfig | 拥有QueryMode、诊断配置、rhi QueryConfig；没有窗口/场景/帧循环参数 | mode/API/device预校验，失败不加载Vulkan；不隐式降级API |
| StartFailure | 拥有主错误、cleanup_errors、可用的部分能力报告及结束结果；报告可能未查询 | 返回前rhi回滚、诊断结束；不能留悬空指针或日志引用 |
| RuntimeSession::start(const RuntimeConfig&) | 推荐expected<unique_ptr<RuntimeSession>,StartFailure>；成功后唯一拥有诊断与rhi会话 | 诊断创建失败不进入rhi；rhi失败须保留最早原因，诊断次级失败独立记录 |
| report() const | 候选const CapabilityReport&，runtime拥有的只读快照 | 不借用rhi内部数据；close不使该快照失效，会话销毁后引用失效 |
| close() | 返回core::EndResult（含diagnostic_errors与cleanup_errors），正常路径先关闭rhi再finish诊断 | 调用方必须处理结束状态；repeat-close只返回缓存结果，不再触发资源释放 |
| destructor | 先释放rhi，再释放诊断 | 不抛异常；未显式close不伪造完整成功摘要，不替代调用方处理EndResult |

推荐在成功start返回前复制rhi报告，避免公开接口依赖rhi关闭后的引用。代价是一次启动期值复制；替代是转移报告所有权，但会使rhi报告接口和关闭协议更复杂，P0暂不推荐。Session本体不复制/不移动，由unique_ptr保持稳定地址。

### 查询模式与设备选择

`QueryMode`固定为`inspect / require_product / development`，放在`RuntimeConfig`，不放进`rhi::QueryConfig`；rhi只报告事实。`inspect`接受requested_api为1.3或1.4，查询完整即成功，低于1.4时报告product=false；`require_product`要求requested_api=1.4，查询完整但产品资格不足时返回`StartFailure{result=unsupported, category=api_version_unsupported}`并保留部分报告；`development`要求显式requested_api=1.3，禁止隐式降级，查询完整即成功但product=false。requested_api与模式不符返回`invalid_arguments`。设备选择用rhi的`FirstEnumerated`或`ExplicitIndex`；索引越界`invalid_arguments`，无设备`initialization_failed/device_not_found`，都不静默换设备。`require_product`不自动改选另一台满足1.4的设备；需要时由调用方显式指定索引。

### 部分失败与结束结果

state为构造中→open→closing→closed。只有成功start的会话可交给调用方；失败由局部所有者完成回滚并返回StartFailure，不需要调用方猜哪些模块已经建立。单一owner线程串行执行，暂不提供回调、取消任务或并发start/close接口。

普通配置错误在诊断创建前发生时，StartFailure依然拥有Error，但不能承诺存在JSON文件。诊断已建立时才输出结构化事件；报告按not_queried表示未执行阶段，不以补造空数组掩盖失败。系统查询成功但产品资格不足时，inspect可以形成正常会话；require-product按契约返回unsupported并收尾。

关闭先收集rhi清理结果，再调用诊断finish。已有查询错误不因清理失败被覆盖；业务错误放primary_error，输出错误放diagnostic_errors，清理错误放cleanup_errors。如果此前成功而清理失败，应转为失败并在cleanup_errors中保留stage=cleanup、category=cleanup_failed的事实，不临时映射为版本不足。最终输出失败按core结束契约优先体现在result，primary_result保留原业务结果。重复close返回缓存的EndResult，不再次清理、不再次写摘要。

能力报告由runtime编码为capability_json，随RunSummaryInput交给core写摘要；core不包含rhi类型，也不解释能力内容。测试先核对实际返回对象，再读取该编码路径生成的文件；不允许测试自己生成一份预期报告冒充受测输出。文件失败时以返回结果和最低限度stderr为证据，不能坚持读取不存在的摘要。

### 关系、构建与验证

跨模块所有权与时序引用[P0 design](../../../openspec/changes/bootstrap-engine-p0/design.md)的P0-OWN-02、P0-SEQ-02，不复制第二套关系图。runtime不拥有第二个原始instance或OS库句柄；rhi借用诊断的有效期由runtime的销毁顺序保证。P0没有GPU提交，帧资源与GPU同步在P1方案另定。

ar_runtime链接rhi/core，外部调用方只包含公开头；第三方日志/JSON/volk实现不泄漏到公开签名。测试可执行文件实际调用start/report/close，P1独立渲染程序再使用同一组合入口，不恢复ArProbe产品工具。

实施顺序：配置校验→诊断建立→rhi打开及报告快照→成功会话/失败收尾→close结果→两配置CTest。验证引用[P0测试计划](../../../openspec/changes/bootstrap-engine-p0/test-plan.md)：真实加载失败、部分回滚、System查询、输出失败、cleanup失败、重复close及身份核对；重复close断言返回缓存结果、无第二次清理/摘要，不声称当前已有测试。

本轮已按用户2026-11-04授权收口候选接口：QueryMode、DeviceSelection、无设备/索引越界、cleanup_errors、CloseResult幂等及summary归属均按上文固定；仍不把方案文档当成代码实施授权。保留P0查询profile，具体渲染feature和应用CLI/退出码留P1。

## 三项复核后的组合边界（2026-11-04，候选）

runtime报告是自有快照，close后值不变、仍可读，直到RuntimeSession销毁才失效；与rhi报告借用的失效严格区分。测试不得访问销毁后引用制造UB。

runtime编码为optional UTF-8对象文本capability_json，core校验语法/顶层对象后嵌入summary.capability，而非字符串双重编码。无查询为nullopt；编码失败/非法JSON/非对象都按P0契约输出失败，尽力写capability:null并保留原错误，不空串/空对象兜底。编码失败注入应在真实编码器边界验证；OOM不承诺完整摘要。

无论清理成功与否，close都使会话terminal closed；EndResult另含resources_released及residual_resources，cleanup_errors独立保留。重复close和close后析构均不再清理或重写摘要，残留状态不允许新rhi会话。业务success但cleanup失败且没有输出失败时result=initialization_failed，primary_result仍success；输出失败优先output_failed。具体清理及隔离状态以[P0契约](../../../openspec/changes/bootstrap-engine-p0/proposal.md)“本轮三项复核修订”为唯一源。

## MF-01/MF-02启动和摘要承接（候选）

runtime编码失败显式传RunSummaryInput.capability_encoding_error，不能仅传nullopt；完整身份/主结果/清理事实和JSON组合以P0 proposal字段表为准。双字段冲突固定summary/internal_error及output_failed，保留原业务Error。

有效配置先创建诊断再rhi open；隔离拒绝返回StartFailure.initialization_failed，primary_error为cleanup/cleanup_failed，origin=internal/1；没有RhiSession，不调用close，本次资源已释放=true、残留和cleanup_errors为空。正常finish诊断不解除既有进程隔离。诊断create失败为output_failed且未执行隔离检查；finish失败保留隔离主错误但最终output_failed。

## 2026-11-06 实现状态（2.1 runtime 组合）

已落盘：`engine/runtime/include/runtime/RuntimeConfig.h`（`QueryMode{Inspect,RequireProduct,Development}`、`RuntimeConfig{mode, diagnostics, query}`）与 `RuntimeSession.h`（`StartFailure{endResult, report, diagnosticsAvailable}`、`RuntimeSession::start`、`report()`、`close()`、不可复制/不可移动、PIMPL）；`engine/runtime/src/CapabilityJson.{h,cpp}` 与 `RuntimeSession.cpp`。`ar_runtime` 静态库链接 `ar_core`、`ar_rhi`、`ar_cxx23`、`ar_cxx_flags`，nlohmann/json 仅私有。

行为与上文候选一致的部分：requested_api 与模式不符在加载 Vulkan 前拒绝（`invalid_arguments/validation`，诊断未建立）；诊断创建失败返回 `output_failed` 且不进入 rhi；`Inspect` 接受 1.3/1.4 且查询完整即成功；`RequireProduct` 要求 1.4，产品资格不足时返回 `unsupported/api_version_unsupported` 并保留报告；`Development` 要求显式 1.3、成功但 `product=false`；`report()` 是 runtime 自有快照，close 后值不变；`close()` 先 rhi 后 finish，重复调用返回缓存 EndResult；析构只释放 rhi 再释放诊断，不写摘要。

capability 编码：按 D1 字段顺序输出（`schema_version/run_id/build_id/source/input_hash/configuration/mode/product_target/requirement_profile/loader_api/device_api/api_variants/instance_create/extensions_query_status/extensions/features/capability_source/variant_supported/effective_api/judgments/product_target_supported/device_name/driver_version/descriptor_heap/renderer_runtime_validated/result/errors`）；成功会话一定给对象文本，编码异常时改传 `capability_encoding_error{stage=summary, category=output_failed}`，不产出空对象。

已知差异与限制：

- `rhi::QueryConfig.requiredDeviceExtensions` 仍是候选接口外的新增字段（自 2026-11-05 起未决），runtime 只透传，不解释。
- 报告不含 `run_id/build_id/schema_version/mode` 的运行时副本，这些字段只在编码的 capability_json 与 `summary.json` 中出现。
- `runtime-device-index-invalid` 中 instance 已为枚举而创建，rollback 时销毁；用例断言的是"没有 runtime 会话、没有换设备、设备索引为 invalid_arguments"，不是"未创建 instance"（Vulkan 枚举需要 instance）。
- 未实现：取消/超时后的强制回收、`CapabilityReport` 中的 feature 精确判定、P1 帧循环与 GPU 提交。

2026-11-06 评审修复（F1–F9）后：`runtime-require-product`、`runtime-mode-mismatch`、`runtime-diagnostic-create-failure` 补上了此前零覆盖的三条路径（产品资格不足、模式与 requested_api 不符、诊断创建失败）；capability_json 的 `descriptor_heap` 改为 `{status: …}` 对象而非字符串；rhi 回滚残留经 `QueryFailure.residualResources` 转发到 `RunSummaryInput.residualResources`，被拒启动的 `report.result` 不再默认 success。rhi 侧同批修正：实例释放失败时保留实例与模块、不 `volkFinalize()`/不卸库，并因此隔离进程；`shutdown` 记录 1→2→3 次序供测试断言。stub 加载器改为每次调用重读 `AR_RHI_FIXTURE_SPEC`，使同一进程内的 SECTION 互不继承行为。

验证（2026-11-06，`python scripts/build_checks.py --phase test --test-file tests/runtime/test_runtime_session.cpp`）：16 条命令全部 exit 0；两配置各 49/49（core 13 + rhi 18 + runtime 16）；`clcmd-*` preview=9、latest=0、ehsc=9、c4530=0、Debug `mdd=9/md=0`、RelWithDebInfo `md=9/mdd=0`、errors/warnings=0；身份 Debug `8a9ec718…`、RelWithDebInfo `a79b070b…`、input_hash `56bda280…`。
