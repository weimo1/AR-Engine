# P0 验证计划

修订：12，2026-11-04。状态：与不做独立Probe的P0候选契约对齐，并补入QueryMode/DeviceSelection、cleanup_errors、重复close与summary归属用例；以下全为后续实现的验收设计，没有真实引擎CTest结果。行为、字段、配额、错误类别和九用例矩阵唯一源为[proposal](proposal.md)，不在此另设退出码表。

## 1. 测试分层与通过含义

| 层级 | 真实输入和调用方 | 证明什么 | 不证明什么 |
| --- | --- | --- | --- |
| CPU | 标准库/GLM/JSON；共用请求校验和能力判定 | 值式错误、版本/variant、资格及编码性质 | 实际loader/驱动支持或进程退出分类 |
| core文件集成 | 任务独占目录，调用真实诊断会话 | 文件/JSONL/配额、flush、摘要、输出失败 | 强杀/OOM后全部日志必然保留 |
| 真实OS加载失败 | runtime公共配置ExplicitPath，不存在任务路径 | 与生产共同加载函数实际失败和回滚 | fixture API或系统设备资格 |
| 受控API fixture | 测试自有Vulkan库经公开LoaderSource | 精确错误、原始结果、调用次数和清理顺序 | 本机真实设备/loader具有该能力 |
| system加载集成 | runtime公共入口System，inspect及显式development1.3 | 真实加载/查询/关闭和当前设备事实 | 已创建设备/绘制、产品1.4或Heap运行通过 |

P0测试进程正常返回0意味着测试断言成立。被测ResultKind另行核对；不验收旧ArProbe退出2/3/4/5。环境不支持真实查询时该项明确未满足；CPU子集可独立成功，但不能宣布完整P0通过。

## 2. 每配置构建与证据

依次执行proposal D3定义的toolchain-record→dependencies-record→identity→configure→identity-check→build→CTest。Debug/RelWithDebInfo各执行一次，命令参数/schema/哈希规则引用proposal，不维护第二套。

- 保存真实cl命令，检查单一c++23preview、无latest/D9002，MDd/MD和依赖库配置正确；std::expected实际编译运行。
- 真实静态目标均有代码；public-headers独立包含各公共头，测试通过runtime公共入口实际调用三个模块，不用空目标证明联编。
- CTest使用--test-dir、-C、--no-tests=error、--output-on-failure并导出JUnit。解析期望命名集合及逐用例JSON，而非只检查总数非0。任何必需用例缺失、跳过、失败、身份不符或超时都不通过。
- 每个case/config/run独占输出目录，重跑不接受旧run_id；记录argv、源/二进制身份、来源、原始结果和断言。被测返回值先断言后经正式编码器写证据；不能测试自己手工拼出的报告。
- 配置/恢复输出在任务根；采用快照时全在其外，起止核对输入哈希。当前本机临时门禁结果只作工具链/依赖准备证据，不勾选引擎任务。

## 3. core用例映射（本轮候选）

| 必需用例 | 实际刺激与独立断言 |
| --- | --- |
| core-normal-close | 建立真实文件会话，写事件/结束；解析文本、JSONL和摘要，run/identity一致，顺序递增，目录不可复用；关闭后文件可由测试打开 |
| core-open-failed | 使用任务自有文件对象作为目录根，create明确output_failed；加载调用0，无伪成功摘要；stderr最低限度诊断可捕获 |
| core-json-escaping | 换行、引号、反斜杠、中文；标准JSON解析器逐行读取，内容回读一致，不用字符串查找替代解析 |
| core-quota | 使用小配额，分别覆盖单事件超限和文件配额；已完整写入行均可解析，返回output_failed/incomplete，文件不越预算，stderr不递归/不重复刷屏 |
| core-repeat-finish | 显式finish两次，结果一致、摘要只写一次、无双重close；析构后文件仍完整，不把析构当再次finish |
| core-late-event | finish后emit返回invalid_state，摘要/文件不再追加；会话不复制不移动的性质由编译检查覆盖 |
| report-unwritable | 使用任务自有目录占据summary目标文件位置；实际finish写入失败、EndResult输出失败；已有主错误保留，不能要求读取不存在/无效摘要 |

finish时flush失败还应有可控sink故障覆盖；测试私有适配只在core本目标内部调用真实输出路径，不新增公共可插拔sink API，不让rhi/runtime包含core私有头。错误注入证明失败收敛，真实文件失败另行覆盖。进程强杀/长期阻塞/OOM不宣称确定性恢复；父进程超时保留已有输出。

## 4. rhi/runtime与能力矩阵

九个负向ID和精确ResultKind/category/事实以proposal矩阵为准；另加device-none、device-index-invalid、cleanup-failure、cleanup-empty、quarantine-reopen、repeat-close及summary-capability七个用例。invalid-arguments现在验证C++配置、加载次数0，不存在未知CLI选项子用例；loader/device variant继续分别覆盖。错ResultKind、普通初始化失败冒充版本不足、缺字段/旧身份/错来源必须使测试失败。

| 必需用例或组 | 补充验证 |
| --- | --- |
| runtime-rollback | 覆盖库打开失败、打开后缺符号、create失败、create成功后枚举失败；实际fixture计数instance destroy/reset/unload且次序正确；无instance不得destroy |
| runtime-device-none | fixture枚举0个设备：initialization_failed/device_not_found，部分报告device_api=null，不静默换设备；不创建会话 |
| runtime-device-index-invalid | ExplicitIndex超出枚举数：invalid_arguments，不创建instance、不自动改选0号设备 |
| runtime-cleanup-failure | 仅注入“已持有instance但销毁入口不可用”或“实例已销毁且入口已清除后OS卸库失败”（不伪造vkDestroyInstance返回错误）：CloseResult保留stage=cleanup，runtime EndResult.cleanup_errors含category=cleanup_failed，primary_error不被覆盖；再次close不重复触发清理 |
| runtime-cleanup-empty | 尚无instance且无本会话资源：closed=true、resources_released=true、residual_resources=[]、cleanup_errors=[]；instance未创建不得作为cleanup故障注入条件 |
| runtime-quarantine-reopen | 专用子进程已有残留后连续两次open：精确initialization_failed/cleanup/cleanup_failed；无新会话/close，加载、volk初始化、instance创建计数均为0，本次resources_released=true/空残留/空cleanup_errors，进程隔离保持；诊断建立失败时output_failed且隔离检查0 |
| runtime-repeat-close | 连续close两/三次：返回缓存EndResult且一致，摘要只写一次，无第二次cleanup事件；runtime close后自有report值不变且可读取；会话销毁后不得访问旧引用 |
| runtime-summary-capability | 成功close摘要capability为嵌入object而非字符串，内容与runtime快照一致；core不包含rhi类型；缺字段/篡改/旧来源失败 |
| runtime-system-inspect | 真实System入口获取完整查询快照，simulated=false/id=null，报告仅解释设备事实；成功不意味着1.4资格 |
| runtime-system-development | 显式1.3查询、真实System入口，能力和身份来源清楚；无窗口/逻辑device/绘制，不称GPU渲染通过 |
| 枚举完整性 | 两scope成功空集合/失败/未查询；VK_INCOMPLETE到上限，确认query_failed而非unsupported；扩展失败不抹去已证明API事实 |
| 版本与variant | loader/device/instance请求分开；unknown判定false但不伪造版本拒绝；loader低版本预检不create；INCOMPATIBLE_DRIVER与INITIALIZATION_FAILED精确分类 |
| 资格与来源一致性 | 失败product=false、instance失败api=false、renderer_runtime_validated=false；CPU/fixture输出不能被标成system或用于产品资格 |
| 模式边界 | inspect在device1.3.289完整查询时success但product=false；require_product同设备unsupported；development显式1.3 success；mode/requested_api不符invalid_arguments |
| build-identity | 正常身份；旧EXE、错配置、改源未重建、缺/陈旧sidecar、triplet/CRT不符都拒绝；revision未知不猜clean |

2026-11-06 rhi 收口后共 18 个 rhi 级用例（`rhi-request-validation`、`rhi-loader-unavailable`、`rhi-loader-symbol-missing`、`rhi-device-none`、`rhi-device-index-out-of-range`、`rhi-api-version-unsupported`、`rhi-api-variant-unsupported`、`rhi-instance-incompatible`、`rhi-instance-init-failed`、`rhi-extension-query-failed`、`rhi-device-extension-query-failed`、`rhi-enumeration-incomplete`、`rhi-device-version-insufficient`、`rhi-descriptor-heap-present`、`rhi-cleanup-instance-retained`、`rhi-repeat-close`、`rhi-report-after-close`、`rhi-quarantine-reopen`），由 `tests/support/rhi_fixture/FixtureLoader.cpp` 编出的 stub 加载器驱动：该 DLL 只导出 `vkGetInstanceProcAddr`，按 `AR_RHI_FIXTURE_SPEC` 改变 loader/device 版本、variant、`vkCreateInstance` 结果、设备数、`VK_INCOMPLETE`、instance/device 扩展查询失败、`vkDestroyInstance` 缺失与 Heap 扩展呈现；测试只经公开 `LoaderSource` 的 `ExplicitLoaderPath` 装载，路径由可执行目录推出。`rhi-quarantine-reopen` 以 `CreateProcessW` 拉起本可执行文件的隐藏用例 `[.][rhi-quarantine-child]`，在干净进程里制造残留并按 proposal 的精确拒绝字段断言，避免进程级标志污染其它用例；`rhi-report-after-close` 验证"关闭前复制快照仍可读"，并用钩子计数记录关闭后的违规调用，不以未定义行为充当失效证据。fixture 不是产品加载器，只用于负向与边界用例；它们**不能替代**上表的 runtime-*、`cleanup-empty`、`summary-capability` 用例，后者仍须经 runtime 公共入口覆盖（`runtime-quarantine-reopen` 即属该批）。真实 system 查询没有跑到时，记录未运行/失败而非用 fixture 补通过。所有 P0 Vulkan 用例串行，不重叠使用 volk 全局状态。后续 P1 再定义 CLI、应用退出、有限帧、resize、Validation 及截图验收，不在本 P0 偷偷恢复 Probe。


2026-11-06 2.1 实施结果：上述十六个 runtime 用例已全部落地并通过（`tests/runtime/test_runtime_session.cpp`，两配置各 49/49）。实现时明确两点：`runtime-device-index-invalid` 断言的是没有 runtime 会话、设备索引返回 invalid_arguments、rollback 释放用于枚举的 instance（Vulkan 枚举需要 instance，故不是"未创建 instance"）；`runtime-cleanup-failure` 的两个 SECTION 之间需调用 `clearProcessQuarantine()`，否则第二个 SECTION 会被隔离拒绝。

2026-11-06 评审修复补记（F1–F9）：新增 `runtime-require-product`（查询完整但 1.4 资格不足 → `unsupported`/`api_version_unsupported`/`capability`，并保留部分报告）、`runtime-mode-mismatch`（inspect+1.2、require_product+1.3、development+1.4 三组都在加载前 `invalid_arguments`，且 `quarantineChecks==0`）、`runtime-diagnostic-create-failure`（诊断 create 失败 → `output_failed`，rhi 侧 load/volk/instance 与隔离检查均 0），runtime 用例增至 13 个。断言层面：`runtime-rollback` 增 `processShutdownOrder()=={1,2,3}` 次序断言（对应 test-plan:47 的 destroy→volkFinalize→unload）；`runtime-cleanup-failure` 的 "destroy 不可用" SECTION 用 close 前后计数差断言 `destroyInstanceCalls+1` 且 `volkFinalizeCalls`/`unloadCalls` 不变（直接抓"实例残留仍卸库"的违规，对应 test-plan:80）；`runtime-summary-capability` 改断言 `capability.descriptor_heap` 为对象（`{status}`，对应 proposal:124）。为让 SECTION 间行为独立，stub 加载器改为每次调用重读 `AR_RHI_FIXTURE_SPEC`，不再缓存首次快照。

## 5. Harness与云端限制

命令表与解析器已实现（`workflow/p0-checks.json`、`scripts/workflow_p0.py`、`scripts/build_checks.py`），当前通过 `python scripts/build_checks.py` 执行该表。工作流自检、单元测试和 OpenSpec 检查仍由工作流 CI 运行。拒绝路径由 `scripts/tests/test_workflow_p0.py` 覆盖：缺命令/未声明用例/缺用例/失败或跳过用例/错退出码/错身份（sidecar 与依赖记录不符）/超时/崩溃/非法占位符/未知 parser/前向依赖；harness 自身另断言运行不改变 snapshot 文件清单。

云端先核对实际VS2026及工具链，不假设windows-latest满足；缺环境明确未满足，不用空job/跳过取得通过，也不否定单独已完成的本地证据。CI不代替本机设备或GPU运行。云端入口已随 `4cbfe87` 推送，该提交尚未手动触发：`.github/workflows/p0-cpu-build.yml` 仅手动触发、`runs-on: windows-2025-vs2026`、45 分钟上限，先由 `scripts/p0_ci_preflight.py` 比对 `workflow/p0-toolchain-expectations.json` 再复用命令表，失败也上传预检与构建证据；本机已验证预检成功以及 mismatch 和 missing 两类失败路径。`4cbfe87` 的云端工具链版本、恢复耗时与两配置结果仍待验证，本地证据与云端结论分开记录。

超时与负载按design建议固定，清理只结束任务拥有进程树，不按名称全杀。文档检查、OpenSpec validate、真实构建/CTest、独立审查分别记录，不相互替代；提交/推送按用户授权，不要求强制gate。

## 三项复核回归断言（2026-11-04）

- rhi报告：在close前复制；关闭后不再调用report或使用原借用。通过调用计数/代码审查验证前置条件，不解引用失效引用做测试。runtime-repeat-close单独验证自有快照close后仍有效且值不变；会话销毁后旧引用不使用。
- runtime-summary-capability：断言capability为object，不是含JSON的string；nullopt为null；非法JSON、数组/标量、空串及真实编码边界注入失败均为summary/output_failed，诊断不完整、主错误保留、capability:null，不出现成功资格。另测失败摘要也不可写，不要求错误必已落盘。
- runtime-cleanup-failure：对每个前置条件/OS失败分支断言closed=true、resources_released=false、明确残留种类及状态；实例仍残留时卸库调用为0。正常/空清理断言closed=true、resources_released=true及空残留。vkDestroyInstance正常返回按void流程完成，不伪造VK_ERROR。
- 清理失败后重复close、会话析构的清理调用计数均不增加；新会话被拒绝。此组测试独占子进程退出，不污染同一CTest进程的其他volk用例。正常cleanup结果和输出失败各组合，核对EndResult优先级及原始错误保留。

## MF-01/MF-02精确回归（候选）

断言唯一源为proposal“摘要输入与隔离拒绝精确承接”，不另定义错误码。runtime-summary-capability覆盖四种输入组合、非法错误字段、JSON语法/顶层object、身份不匹配、resources_released=true与残留并用以及摘要不可写；对每个Error逐字段比较，核对无重复合并、主错误未覆盖、summary资源事实存在，禁止仅判断可解析。

runtime-quarantine-reopen在独占子进程中覆盖：连续两次隔离拒绝精确initialization_failed/cleanup/cleanup_failed；本次resources_released=true/空残留/空cleanup_errors，无会话无close，加载/volk/创建计数0，进程隔离未解除。正常诊断finish；finish失败保留隔离主错误但最终output_failed。诊断create失败则隔离检查0，不能断言已经拒绝过重开。

runtime-cleanup-empty为独立必需用例，尚无instance且无资源时清理成功。runtime-cleanup-failure仅注入持有instance但销毁入口不可用、或依赖已释放后的OS卸库失败；逐项核对残留和禁止后续不安全步骤，不混入空清理。

## 2026-11-06 审查修复回归映射

- F1：`rhi-request-validation` 的非零 variant 非法请求及 `rhi-quarantine-reopen` 子进程，逐字段断言 instanceCreate.requested、apiVariants.requested、capabilitySource 的 kind/id/simulated；拒绝后仍保持 NotAttempted 和隔离路径三项加载计数为 0。
- F3：`rhi-repeat-close` 在诊断会话结束前销毁已显式 close 两次的 RHI owner，读取落盘 events.jsonl，断言 rhi.close.start 恰一条，覆盖析构不重复清理。
- F4：子进程等待必须返回 WAIT_OBJECT_0；异常等待记录返回值与超时标识，终止并等待本测试持有的子进程后关闭句柄。正常路径核对 GetExitCodeProcess 成功。超时分支本批未故障注入，不声称实测通过。
- 复现入口：按 CMakeUserPresets.json 的本机覆盖设置进程级 VCPKG_ROOT/AR_P0_DEPS 后运行 `python scripts/build_checks.py --phase test --test-file tests/rhi/test_rhi_session.cpp`；日志、两配置 JUnit 和身份位于 build/logs/p0-checks 与 build/identity。隐藏 rhi-system 不在本批 CPU 验收内。

2026-11-06 验收缺口补齐：core 侧新增 `core-capability-combinations`（合法对象、非法 JSON、数组、标量、空串、仅 encoding_error、双有值冲突，逐项核对 result/诊断错误/capability 是否为 null 与主错误保留）与 `core-summary-conflicts`（resources_released 与残留冲突、build_id 不一致、run_id 不一致、encoding_error 字段不符 → summary/internal_error 且保留业务错误），并新增 `core-event-encoding-failure` 固定“非 UTF-8 事件消息必须 output_failed 而非抛出”。runtime 侧新增 `runtime-summary-output-failure`、`runtime-destructor`、`runtime-capability-encode-failure`（fixture `device-name-hex=C328` 注入非法 UTF-8 设备名）。清理失败与隔离场景改由 `[.][runtime-cleanup-destroy-child]`、`[.][runtime-cleanup-unload-child]`、`[.][runtime-quarantine-child]` 独占子进程执行，父用例经 `runChildCase` 检查 WaitForSingleObject 返回值并在超时后终止任务自有子进程。两配置各 49/49，证据与身份见 tasks.md 同日记录。
