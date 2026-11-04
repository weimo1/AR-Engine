# P0 实施任务

修订：11，2026-11-04。状态：方案已按先不做Probe修订，D1—D3 已确认。2026-11-04 首批实现完成 1.1/1.2/2.2、core 相关用例与 3.2（命令表驱动的 verify）；独立审查两轮意见已处置，两配置各 12/12 重跑通过并重记身份。2026-11-05 补齐 3.3 的本地可审阅部分（手动触发的 P0 CPU 工作流、runner 镜像与固定工具链预检、证据上传），但未推送、未触发，云端结果不可观察，故 3.3 不勾选。rhi/runtime 与 3.4 交付仍未完成，不把本机临时门禁算正式P0任务完成。

## 1. 工具链与依赖

- [x] 1.1 在正式manifest固定MSVC/CMake/SDK、vcpkg工具/registry来源及baseline、x64-windows-static-md；核对实际ports/features/CRT、triplet内容哈希与Heap结构编译；承接已跑通的任务自有ASCII工具/临时/输出根和进程环境处理，不改全局工具。（已完成：固定版本与 triplet 哈希、实际 4 端口/CRT/配置、任务自有 ASCII 根均核对；Vulkan/Heap 结构编译随 2.3 rhi 批次，本批未含 Vulkan 依赖）
- [x] 1.2 接入唯一C++23preview模式、真实标准库运行、toolchain/dependencies记录及BuildInfo/identity生成和过期拒绝；Debug/RelWithDebInfo分别验证，区分顶层与端口构建工具，不复用旧临时结果代验收。

## 2. 最小模块纵切

- [ ] 2.1 实现runtime组合公共入口：真实调用core/rhi、QueryMode三模式、startup/close及部分失败回滚；close幂等，cleanup_errors保留，capability_json交core写摘要；无独立ArProbe，测试程序实际链接三静态库，P1再接独立渲染程序。
- [x] 2.2 实现core最小诊断、Error（固定ErrorCategory/ErrorStage）、事件/摘要和显式结束；按本轮确认后的候选接口/预算处理open/write/flush/finish失败、配额、sticky输出失败和重复结束，EndResult分开保存diagnostic_errors/cleanup_errors，无Jobs/反射/CPU分配器。
- [ ] 2.3 实现公开LoaderSource共同加载路径、instance/loader所有权、查询/版本/variant/来源及精确返回分类；DeviceSelection为FirstEnumerated/ExplicitIndex，无设备device_not_found、索引越界invalid_arguments，CloseResult幂等；不建CLI，不创建逻辑device/窗口/帧循环。

## 3. 适用验证与交付

- [x] 3.1 随每层加入Catch2/CTest命名测试与真实公共入口集成：按proposal九个负向ID、新增用例（device-none/device-index-invalid/cleanup-failure/cleanup-empty/quarantine-reopen/repeat-close/summary-capability）及必需正向/core测试核对返回、事实、清理、身份/来源；两配置分别运行；不宣称验证了暂缓的Probe进程退出码。（本批完成 core 层 12 个用例：core-normal-close、core-open-failed、core-json-escaping、core-quota、core-quota-cumulative、core-flush-failure、core-summary-truncated、core-repeat-finish、core-late-event、report-unwritable、public-headers、build-identity，Debug/RelWithDebInfo 各 12/12；device-*/cleanup-*/repeat-close/summary-capability 随 2.3/2.1 批次）
- [x] 3.2 如接Harness，按proposal实现直接命令表、depends_on、记录/身份和逐case解析，再替换原生源码占位保护；保留旧三项工作流检查，验证错码/错类别/缺用例/过期/超时/崩溃均拒绝，输出不污染输入。（已完成：`workflow/p0-checks.json` 增加 `change=bootstrap-engine-p0` 与两条 ctest 的 `expected_cases`（12 个用例名）；新增 `scripts/workflow_p0.py` 做表校验（字段/占位符/parser 白名单、依赖顺序、timeout、禁止 shell 字符串）与七个解析器（json-toolchain/json-dependencies/json-identity/cmake-configure/msbuild-log/msbuild-cl/ctest-junit，逐项判定而非只看退出码）；`scripts/run_p0_checks.py` 改为同一实现的 CLI；`workflow_runtime.checks` 在 change 匹配时用表执行，其他 change 保留原"原生源码占位保护"，旧三项检查不变；`scripts/tests/test_workflow_p0.py` 53 个用例覆盖错码/错类别/缺用例/未声明用例/过期身份/超时/崩溃/非法占位符/未知 parser/前向依赖/输出不污染输入）
- [ ] 3.3 核对云端VS2026及固定工具链后接入真实CPU构建/CTest；环境不足明确未满足，本地与云端、查询与GPU运行分开记录。（本地已就位、云端未验证：新增 `.github/workflows/p0-cpu-build.yml`，仅 `workflow_dispatch`、`runs-on: windows-2025-vs2026`、`timeout-minutes: 45`，顺序为 checkout→setup-python→预检→`python scripts/run_p0_checks.py`→摘要→always 上传 `build/logs/p0-checks`、`build/metadata`、`build/identity`；`scripts/p0_ci_preflight.py` 先记录 runner 镜像再按 `workflow/p0-toolchain-expectations.json` 核对 VS 18、MSVC 14.51.36231、cl 19.51.36260、CMake 4.2.1、SDK 10.0.26100.0、generator 与 triplet，缺项或版本不符即失败并上传预检日志；依赖用仓库固定的 vcpkg 工具/baseline 在线冷恢复、不启用跨运行二进制缓存。本机已实测预检 ok 与 mismatch/missing 两条失败路径，但**未推送、未触发、未获云端结果**，镜像实际工具链可能不符而首跑失败，故不勾选、不声称云端通过；本地三配置证据与云端结果分开记录）
- [ ] 3.4 对实际输入完成适用验证、独立审查及获授权的交付；更新长期模块事实及已知限制，未满足1.4/Heap和应用验收不标通过；不以强制gate或writer包作为人工工作前置。

## 实施顺序及当前交接

实际顺序为1.1/1.2准备正式工程→2.2 core（同步3.1）→2.3 rhi（同步3.1）→2.1 runtime纵切（同步3.1）→适用3.2/3.3/3.4。保留任务编号以便追溯，不按编号误解为先做runtime。

当前交接给独立审查：首批实现、build/ 目录迁移与第二轮复审的 R1–R7 处置已完成自检，等待按同一冻结输入复审；实现者不以自检替代审查。R1/R2 由实现者按用户 2026-11-04 授权裁定：R1 采用"项目内 build/ + configure 前与 ctest 后各核对输入哈希"，并同步 `proposal.md` 第 191/202/212/213 项；R2 新增 `ErrorCategory::InvalidState`（生命周期违规与参数非法分离），同步 `docs/modules/core/README.md` 与用例断言。具体core候选见[模块方案](../../../docs/modules/core/README.md)，后续rhi/runtime确切签名随P0b/P0c细化，不拿类型示意当公开接口已批准。proposal是行为/验收编辑源，spec只能derive，不提前归档/sync。

2026-11-04三项复核映射：2.1/2.2实现能力JSON对象嵌入/语法失败和结束结果，2.3落实terminal关闭、释放状态/残留和不重试，3.1分别验证rhi借用与runtime快照生命周期、非双重编码及隔离子进程清理失败。当前 2.2 已实现四种JSON组合与输入一致性冲突映射（含 `capability:null` 失败摘要），但只有 core-normal-close 覆盖合法对象嵌入，其余组合尚无独立用例；2.1/2.3 及对应 3.1 断言未实施。不新增任务。

MF-01/MF-02映射：2.2/2.1实现显式编码错误输入/摘要映射；2.3实现隔离检查精确拒绝；3.1加入runtime-cleanup-empty、runtime-quarantine-reopen及四种JSON组合断言。2.2 部分已实现（同上，测试未覆盖），其余未实施。

审查修复（2026-11-04，针对独立审查的 10 条意见）：阻断项"单事件上限误用 sink 累计字节"已改为只比较单行长度，并新增 core-quota-cumulative（40×200B 事件）；`/EHsc` 经 `ar_cxx_flags` INTERFACE 目标进入全部 4 个 TU，C4530 归零；构建清单重新覆盖 `workflow/p0-checks.json`（25 个文件），命令表改为静态声明（退出码写 `build/logs/p0-checks/results.json`）以消除自引用抖动；新增 core-flush-failure（故障注入 + 首错告警一次）；摘要截断回退保留 primary_error/cleanup/diagnostic 错误并在仍超预算时改记计数；子进程输出改为严格 UTF-8 优先解码；`clcmd-<cfg>` 与 `verify-inputs-<cfg>` 进入命令表，语言/CRT 结论改由命令表产物复核；`tasks.md` 两处"全部未实施"改为按实际实现程度描述。未覆盖项：摘要截断回退当时无独立用例、`invalid_state` 三处不一致——两项已在第二轮复审处置中补齐（见下）。

审查修复后证据：16 条命令全部 exit 0；两配置 CTest 各 11/11（0 失败 0 跳过）；`/std:c++23preview`×4 且 latest=0；Debug `/MDd`×4、RelWithDebInfo `/MD`×4、`/MT`=0；`C4530`=0、编译 warning=0；身份 build_id Debug `4cb46a78…` / RelWithDebInfo `f040c61f…`、input_hash `50ff37ed…`。
第二轮复审残留处置（2026-11-04，用户授权实现者裁定 R1/R2）：R1 改契约为"项目内 build/ 例外 + configure 前/ctest 后哈希复核"，`verify-inputs-<cfg>` 命令已入表；R2 新增 `ErrorCategory::InvalidState`（13 值），`proposal.md` 的 invalid_state 措辞与实现、模块文档一致；R3 故障注入钩子改为 `AR_ENABLE_TEST_HOOKS` 编译开关（默认 OFF，检查命令传 ON；测试启用时若为 OFF 则 FATAL_ERROR，防止用例集静默缩水；不带测试的生产树用 `-DAR_BUILD_TESTS=OFF`）；R4 新增 core-summary-truncated（summaryBytes=600 + 20 条 cleanup 错误，断言 errors_omitted/计数保留与 primary_error 不丢）；R5 core-flush-failure 增加"两 sink 失败、告警只记一次"的计数断言；R6 保留 build-* 与 clcmd-* 两级（前者覆盖普通增量构建、后者留存命令行证据），并删除测试目标重复的 `/W4`；R7 `decode_output` 改为逐行解码。

3.2 交付（2026-11-04，用户"继续"后实施）：命令表由 `scripts/workflow_p0.py` 校验并执行，`workflow.py verify bootstrap-engine-p0` 现在跑真实 configure/build/CTest 而不是占位保护；表结果与旧三项工作流检查分开记录（`checks.json` 的 `checks` 仍是三项，引擎侧在 `engine_checks`，`gate` 的既有校验口径不变）。本地开发入口 `python scripts/run_p0_checks.py` 与 verify 共用同一实现。单条命令的观察退出码/耗时写 `<output>/logs/p0-checks/results.json`，表内 `wrapper_exit`/`child_exit` 仍是声明字段（null）。尚未完成：2.3 rhi、2.1 runtime、3.4 交付审查。

3.3 本地就位（2026-11-05，用户指定镜像/预算后实施）：新增手动触发的 `.github/workflows/p0-cpu-build.yml`（`workflow_dispatch` only、`windows-2025-vs2026`、45 分钟），在依赖恢复前运行 `scripts/p0_ci_preflight.py` 记录 runner 镜像并按 `workflow/p0-toolchain-expectations.json` 核对 VS/MSVC/CMake/SDK/generator/triplet，缺项或版本不符即失败并上传预检日志；通过后由 `python scripts/run_p0_checks.py` 复用 3.2 的命令表完成在线冷恢复与两配置构建/CTest，并把 `build/logs/p0-checks`、`build/metadata`、`build/identity` 作为证据上传（含 results.json 与两份 JUnit）。首轮不开跨运行二进制缓存。已验证：本机预检 `result=ok`（exit 0）；把期望 CMake 改成 9.9.9 → `mismatch` + `result=failed` + exit 1；把 `ProgramFiles(x86)` 指向不存在目录 → 多项 `missing` + exit 1；两条失败路径都写出 log/json。**未验证**：云端实际运行（未推送、未触发），因此镜像上的实际工具链版本、恢复耗时与两配置构建结果仍是未知；若版本不符，首跑会失败并留下差异日志，需按预检流程另行审阅工具链变更，不得据此宣称云端通过。3.3 保持未勾选。
