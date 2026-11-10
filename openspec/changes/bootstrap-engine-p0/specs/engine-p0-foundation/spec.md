<!-- Generated from proposal.md; source-sha256: 9ae854c58551b628e0c24cf9f21b988f741e0425faa752c1df27dff5049dc22a. Do not edit. -->

## Purpose

为后续独立渲染程序提供可复现的Windows C++23静态基础模块，通过正式模块公共接口与有实际断言的测试验证诊断、启动、能力查询和失败清理。当前不建设独立Probe；环境查询、合成判定和产品GPU验收分开记录，避免把SDK版本、配置成功或测试退出0当成渲染器已完成。

状态边界：静态模块及既有D1/D2事实语义沿用确认；本轮新增core接口/预算和CTest承接方式仍是待确认设计。以下SHALL表达拟议实现要求，不代表这些新增差异已获批准。

## ADDED Requirements

### Requirement: 固定工具链与真实联编
工程 SHALL 固定VS2026/MSVC、CMake、Windows SDK、x64及有效C++23preview，提供Debug和RelWithDebInfo真实构建与CTest。core/rhi/runtime静态库经公共头由测试可执行文件实际调用；第三方优先静态库，全目标动态CRT，不承诺跨工具链ABI。不建设独立ArProbe，P1再接独立渲染程序。

#### Scenario: 标准模式或测试无效
- **WHEN** 真实cl命令出现D9002、latest/冲突标准开关，或CTest没有执行规定的用例
- **THEN** 该配置验收失败；配置/编译退出0、空目标或另一配置通过均不能代替

### Requirement: 受控依赖与构建身份
工程 SHALL 使用固定vcpkg工具、registry baseline、x64-windows-static-md及显式features，记录实际解析ports/配置。Vulkan-Headers、volk、VMA来自统一受控依赖，SDK提供工具。构建身份由输入清单和工具/依赖记录生成，不允许运行参数伪造；完整规则见本契约的身份产物约定。

#### Scenario: 运行了陈旧或错误配置的测试程序
- **WHEN** 编入build_id/input_hash/configuration与本次sidecar不符，或源文件已变化而身份未更新
- **THEN** 构建/检查失败并保留差异；不能从日志文本或当前HEAD补造旧二进制身份

### Requirement: core诊断最小集（本轮候选）
core SHALL 提供独立于Vulkan的错误值、诊断会话、结构化事件与运行摘要；spdlog为薄封装后端。会话由runtime唯一持有，rhi仅借用且不留存超过会话生命周期的引用。事件和结果使用稳定类别，不靠文本日志控制流程。公开头不暴露spdlog、JSON、volk或Vulkan类型。

#### Scenario: 最早初始化失败
- **WHEN** 诊断文件打开失败，或诊断已建立而loader/查询失败
- **THEN** 前者返回output_failed且不得开始Vulkan加载；后者保留已知错误及摘要并清理已创建资源；清理和诊断次级错误不覆盖最早业务原因

### Requirement: 有界日志与可靠结束（本轮候选）
P0诊断 SHALL 在启动/查询/关闭的低频路径同步输出文本及UTF-8 JSONL，不创建异步队列或全局日志服务。候选默认预算为文本和JSONL各8MiB、单事件编码上限16KiB、摘要64KiB；详细策略见本契约日志表。必需诊断输出损坏不得把运行标为成功，析构不得抛异常或隐式结束进程。

#### Scenario: 诊断输出失败或重复结束
- **WHEN** 写入/flush失败、配额耗尽，或finish被重复调用
- **THEN** 返回可判断的输出状态，最低限度stderr诊断不递归调用日志；重复finish不重复摘要/关闭；已有主错误和次级输出错误同时可查。不能把成功写入摘要当成画面正确

##### core候选事件与结束语义（尚未实施）

| 对象 | 当前P0候选契约 |
| --- | --- |
| Error | {category,stage,message,origin:null或{domain,code}}，拥有自身字符串；category取固定ErrorCategory枚举，stage取固定ErrorStage枚举；Vulkan错误由rhi转换为domain=vulkan及原始有符号整数，core不引用VkResult |
| Event | {schema_version:1,run_id,sequence,level,module,event_id,stage,message,origin}；sequence从1递增，时间/线程信息由会话补充；不复制完整能力表到每条事件 |
| DiagnosticConfig | 输出根、run_id、级别、配额；run_id仅允许字母/数字/下划线/连字符，长度1—64；使用新建独占运行子目录，已有运行目录拒绝，路径无效不得逃逸配置根 |
| 日志配额 | 级别过滤先于格式化；超过单事件上限返回output_failed，不写截断JSON；文件配额到达后停止写该sink并置不完整状态，stderr只告警一次；不轮转、不静默丢弃必需证据 |
| RunSummary / EndResult | RunSummaryInput含身份字段、通用结束字段及capability_json（optional UTF-8 JSON对象文本，缺省为nullopt）；EndResult含result、primary_result、primary_error、diagnostic_errors、cleanup_errors、diagnostics_complete、resources_released、residual_resources；均拥有值数据。primary_result保留查询/业务结果；诊断输出失败使最终result=output_failed，否则在原业务成功且cleanup失败时result=initialization_failed，其余保留primary_result；cleanup_errors独立保留stage=cleanup的错误，不覆盖primary_error。未查询写明状态，不靠文件存在推断成功 |
| finish | 在所有rhi清理事件结束后，接收RunSummaryInput（通用结束字段+已编码capability_json），先flush日志，再写/关闭一次summary.json，返回完整结束结果；摘要本身失败通过返回值及stderr报告，不能声称故障已写入该摘要；重复调用返回缓存的结束结果 |
| 生命周期/线程 | create→open→finishing→closed；P0单一owner线程调用，会话不复制/不移动；禁止并发emit/finish、finish后写入返回invalid_state；RAII仅兜底释放，不把异常终止当成已正常结束 |

进程被强杀、磁盘设备长期阻塞或OOM时不承诺完整日志。以上配额可配置但必须为有限正数；测试使用小配额验证边界，实际数值与API整体仍待用户确认。

diagnostic_errors按固定sink/操作类别聚合，保留首次错误及计数，不为每次后续写入失败追加无限错误对象。会话提供enabled(level)供调用方在构造高成本message之前过滤；emit接收已构造事件时不能声称能够消除调用方已发生的格式化成本。

### Requirement: 产品目标与查询策略分离
能力查询 SHALL 记录loader/device API、instance创建事实、扩展/feature查询状态、1.4产品资格和Heap支持。C++查询配置 SHALL 明确 `QueryMode{inspect,require_product,development}` 和 `DeviceSelection{FirstEnumerated,ExplicitIndex}`：前者放在runtime，rhi只报告事实；后者放在rhi，按枚举顺序或显式索引选择，不评分、不静默换设备。`development`必须显式API1.3；`require_product`必须请求1.4；模式与requested_api不符是invalid_arguments。无设备是`initialization_failed/device_not_found`并保留`device_api=null`的部分报告；显式索引越界是invalid_arguments；两者都不自动改选另一台设备。P0只查询版本profile，不创建逻辑device，不证明Shader/绘制或完整产品可用。

#### Scenario: Loader新但device版本不足
- **WHEN** loader1.4.363、请求instance1.4成功而device1.3.289
- **THEN** 两项API/产品资格false；require-product返回unsupported，inspect在查询完整时可success；显式development1.3可成功查询但产品资格仍false

#### Scenario: 无设备或设备索引越界
- **WHEN** instance成功但枚举返回0个设备，或ExplicitIndex超出设备数
- **THEN** 前者返回initialization_failed/device_not_found并保留device_api=null的部分报告；后者返回invalid_arguments；都不静默换设备，也不把无设备伪装成版本不足

#### Scenario: variant未知或不兼容
- **WHEN** loader/device variant未查询完整或任一非0
- **THEN** 未知不得判资格true；非0原始值保留并在require-product返回api_variant_unsupported；requested variant非0是invalid_arguments且不触发加载

### Requirement: 查询事实完整及唯一报告形状
报告 SHALL 遵守本契约字段表，区分not_queried/success/failed、真实/合成来源及失败阶段。VK_INCOMPLETE有界重试，最多3次枚举循环仍未完成即query_failed，不解释为空集合。版本预检未调用vkCreateInstance时不伪造VkResult；扩展查询失败不能抹去此前已确认的API事实，但任何运行错误都使product_target_supported=false。

#### Scenario: 查询失败与成功空集合
- **WHEN** 扩展枚举失败或成功返回0个扩展
- **THEN** 对应scope分别为failed或success；前者返回initialization_failed并含query_failed原始错误，后者是合法空集合，不能混同

##### D1唯一报告形状（保留事实语义，承接接口候选）

| 字段 | 类型及约束 |
| --- | --- |
| schema_version/run_id/build_id | integer 1 / 非空string / SHA256十六进制string |
| source/input_hash/configuration | source={revision:string或null,dirty:boolean或unknown,input_hash:SHA256}；顶层input_hash回显同值；configuration为编入的Debug或RelWithDebInfo |
| mode/product_target | mode为C++配置inspect/require-product/development；product_target={variant:0,major:1,minor:4,patch:0}；不是当前CLI承诺 |
| requirement_profile | {id:p0-version-query,required_extensions:[],required_features:[],scope:query_only}；将来renderer profile另定 |
| loader_api/device_api/api_variants | 原始uint32或null；api_variants={loader,requested,device}，每项整数或null，尚未查询用null；不能把未知当0 |
| instance_create | 唯一{requested:uint32,status:not_attempted/success/failed,vk_result:null或{code:integer,name:string}}；未调用结果null；成功VK_SUCCESS；无created/instance_api别名 |
| extensions_query_status | {instance:{status:not_queried/success/failed,vk_result:null或VkResult对象},device:{status:not_queried/success/failed,vk_result:null或VkResult对象}}；device作用域仅选中设备 |
| extensions/features | 扩展array<{scope:instance/device,name,spec_version}>；features按规范名索引{value:boolean或null,query_status:not_queried/success/failed} |
| capability_source | {kind:system_loader/fixture_loader/fixture_vulkan_api,id:string或null,simulated:boolean}；真实system为false/null，fixture必须true及非空固定id |
| variant_supported/effective_api | variant_supported=true/false/unknown；effective_api为查询阶段可证明的兼容API版本或null，不表示实际绘制环境 |
| judgments | {api_version_supported:boolean,required_capabilities_supported:boolean}；API需loader、device、instance请求满足1.4、创建成功且variant兼容；必需能力仅对声明profile全部查询满足才true |
| product_target_supported | 仅表示P0 profile两项judgments皆true且无运行错误，不代表完整renderer资格；未查询完整或运行失败false |
| device_name/driver_version | 选中设备的字符串/原始整数或null；未选中时null |
| descriptor_heap | {status:not_queried/query_failed/unsupported/supported}；扩展与适用feature均查询且满足才supported，头文件存在不证明支持 |
| renderer_runtime_validated | 当前恒false；未创建设备或绘制 |
| result/errors | result为下表ResultKind；errors=array<{stage,category,vk_result:null或VkResult对象}>；无错误[]；非Vulkan阶段不伪造vk_result |

纯CPU判定输入/输出必须标fixture_vulkan_api；真实查询必须system_loader，不能只信被测报告自行声称的来源，测试composition root绑定实际来源与固定fixture id。报告数据拥有自身字符串，不借用已销毁instance。运行摘要的输出失败可能发生在能力报告生成后：编码最终产物时将产品资格置false并附加输出错误；已损坏/未成功落盘的文件不得作为最终报告使用。

### Requirement: 会话所有权与共同加载路径
runtime SHALL 唯一拥有诊断与rhi会话；rhi独占LoaderModule及VulkanInstance，instance先销毁、volk状态后清除、OS库最后卸载，诊断最后关闭。公开engine/rhi/include/rhi/LoaderSource.h提供System/ExplicitPath，runtime配置携带该值。两种来源都走真实OS加载→vkGetInstanceProcAddr解析→volkInitializeCustom→global入口检查→instance创建/入口加载。测试不包含私有头。P0会话串行，不重叠使用volk全局状态。

#### Scenario: 部分初始化失败
- **WHEN** 库打开后缺符号、instance创建失败或instance成功后枚举失败
- **THEN** 仅清理实际取得的资源且各一次；成功instance销毁早于volk清除/卸库；无效instance不销毁，清理事件在诊断结束之前

### Requirement: 关闭幂等与清理错误保留
rhi SHALL 返回`CloseResult{closed,resources_released,residual_resources,errors}`并在首次close执行instance销毁、volk清理和卸库；重复close返回缓存结果，不重试、不重复清理，rhi报告在close后失效，runtime自有快照不受其影响。清理错误使用`stage=cleanup`、`category=cleanup_failed`，不得覆盖此前的查询/启动主错误。runtime SHALL 在EndResult.cleanup_errors中保留这些错误；若此前成功而清理失败，最终result转失败但primary_result保留原业务结果；诊断finish在全部清理事件之后执行。

#### Scenario: 清理失败
- **WHEN** 成功查询后清理安全前置条件不成立，或OS卸库返回失败（不伪造void销毁的错误返回）
- **THEN** CloseResult保留首个清理错误及后续计数，runtime返回result失败且cleanup_errors含stage=cleanup；重复close不再次调用清理函数；诊断摘要仍按清理后状态写入


### Requirement: 本轮三项复核修订（2026-11-04，具体选择仍为候选）

实现 SHALL 遵守下列报告生命周期、JSON对象边界及terminal关闭/残留规则；这些新增具体选择仍待用户确认。

#### Scenario: 清理后读取快照及摘要失败
- **WHEN** runtime关闭rhi，或摘要能力文本非法，或清理留下资源
- **THEN** runtime自有快照仍有效；非法JSON导致output_failed与capability:null；残留如实报告，不因closed=true声称全部资源释放

报告生命周期：保留const CapabilityReport&接口，rhi::report()仅在open且未close时允许调用，close后禁止调用或使用此前借用；跨close需要先复制。runtime::report()返回runtime自有快照，close后仍可读取且值不变，RuntimeSession销毁后引用失效。暂不新增optional/reference_wrapper接口；它不能让已经借出的引用自动失效。禁止用访问已失效引用的UB测试“是否失效”，通过接口前置条件审查、调用计数和关闭前复制结果验证。

capability边界：RunSummaryInput.capability_json为std::optional<std::string>，有值时必须是UTF-8且恰好一个JSON对象；无查询产物为nullopt，空串不是“没有能力”的合法表示。runtime负责能力语义及编码，core只做语法和顶层object校验，不解释Vulkan字段。summary.json中固定字段capability为嵌入对象或null，不写JSON文本字符串字段capability_json，不双重编码。编码/解析/非object错误均记stage=summary、category=output_failed，最终result=output_failed；保留primary_error，追加diagnostic_errors，diagnostics_complete=false，尽力写capability:null的失败摘要。禁止空对象占位或成功资格；已有能力快照不回写篡改，摘要capability为null即不包含资格true。若摘要写入也失败，只由返回值及stderr报告，不承诺故障已落盘。

关闭边界：CloseResult={closed,resources_released,residual_resources,errors}。首次close完成清理决策后closed=true，表示API已终止，不表示全部资源释放；resources_released仅在所有已取得资源均确认释放时true，residual_resources为空。其余情况false且逐项记录资源种类(instance/dispatch_state/os_module)及retained或unknown状态，不公开裸句柄。无资源需要清理也为closed=true/resources_released=true。runtime的会话进入terminal closed，不因清理失败重新open；EndResult保留相同资源释放事实和cleanup_errors，不覆盖primary_result/primary_error。没有输出失败时，原业务成功但cleanup失败使result=initialization_failed；已有业务失败保留其result；有输出失败则最终output_failed。

清理顺序仍为instance→入口状态→OS库。vkDestroyInstance为void，无可检查的失败返回；入口状态清除也不伪造VkResult。如果安全前置条件不成立而未执行某步骤，记录cleanup_failed及残留，停止任何可能破坏父资源/函数入口的后续步骤。仅在实例释放和入口清除已完成后卸库；Windows FreeLibrary返回失败时保留os原始错误，模块引用作为残留。进程崩溃/访问违规不转换为普通cleanup结果。resources_released表示本会话拥有的引用已释放，不保证OS中该DLL被其他使用者引用为零。

重复close和显式close之后的析构均不再次尝试失败步骤；残留句柄隔离，不由成员RAII偷偷重试，交由进程退出收尾。未显式close的析构只允许做一次同样的尽力清理。存在残留/未知全局状态时禁止本进程再次打开rhi会话，避免重用污染的volk状态；测试在专用子进程验证该状态，不接管其他进程。该terminal/quarantine选择代价是无法在同进程恢复，替代方案是完整可重试清理状态机，P0暂不增加。

参考API事实：[vkDestroyInstance](https://docs.vulkan.org/spec/latest/chapters/initialization.html)返回void；[FreeLibrary](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-freelibrary)有失败返回及GetLastError。前置条件故障注入是项目控制层测试，不伪称Vulkan提供销毁失败返回码。

### Requirement: 精确返回结果和分层测试（替代Probe的候选）
P0 SHALL 以测试程序经正式公开接口验证查询/启动/关闭；ResultKind与错误事实共同决定断言，不再以旧ArProbe退出码作为当前验收。候选ResultKind为success/invalid_arguments/initialization_failed/unsupported/output_failed；详细Error.category保留原分类。CPU用例、真实加载集成及受控API fixture分开；测试自身正常退出0仅表示断言通过，崩溃/超时/缺结果均失败。

#### Scenario: 原因或结果枚举不匹配
- **WHEN** 用例期待unsupported但得到initialization_failed，或错误类别/原始结果/身份/来源不匹配
- **THEN** 命名测试失败，即使两者都属于失败；不接受“任意失败即通过”，不声称验证了旧退出4

##### 负向用例矩阵（本轮承接方案，九个ID保留但不再是进程命令）

共同断言：当前schema/run_id/build_id/input_hash/configuration匹配；任何运行错误product=false；API未确认完整/instance未成功/版本不兼容时api=false。主错误与输出次级错误分开。下面ResultKind是被测C++结果，不是测试进程退出码；旧2/3/4/5退出契约暂缓。

| CTest用例ID | 预期ResultKind/主要category | 层级与必需事实 |
| --- | --- | --- |
| invalid-arguments | invalid_arguments / invalid_arguments | CPU，共用请求校验；requested variant非0及development缺显式API；加载调用次数0；不再测试不存在的未知CLI选项 |
| loader-unavailable | initialization_failed / loader_unavailable | 真实OS集成，runtime ExplicitPath指向任务自有不存在文件；source=fixture_loader；instance not_attempted；未继续初始化 |
| extensions-query-failed | initialization_failed / query_failed | 受控API fixture，逐scope status=failed、保留原始VkResult；instance已成功时仍验证清理；API事实若已查明可保留true |
| loader-version-insufficient | unsupported / api_version_unsupported | 受控API fixture，require-product，loader低于请求版本、instance not_attempted |
| instance-api-incompatible | unsupported / requested_api_unsupported | 受控API fixture，已调用create，failed及VK_ERROR_INCOMPATIBLE_DRIVER |
| instance-initialization-failed | initialization_failed / initialization_failed | 受控API fixture，failed及VK_ERROR_INITIALIZATION_FAILED；不冒充版本拒绝 |
| device-version-insufficient | unsupported / api_version_unsupported | CPU判定与受控API组合，loader1.4.363、instance成功、device1.3.289，require-product，两项资格false |
| api-variant-unsupported | unsupported / api_variant_unsupported | CPU和受控API分别覆盖loader/device非0，记录原始variant和unknown路径；source=fixture_vulkan_api |
| report-unwritable | output_failed / output_failed | 真实文件输出集成，用任务目录对象作为文件目标使写入失败；保留primary_result/primary_error和诊断错误，不要求读取不存在摘要 |

此外必需命名用例为core-normal-close、core-open-failed、core-json-escaping、core-quota、core-repeat-finish、core-late-event、public-headers、build-identity、runtime-rollback、runtime-system-inspect、runtime-system-development、runtime-device-none、runtime-device-index-invalid、runtime-cleanup-failure、runtime-repeat-close、runtime-summary-capability、runtime-cleanup-empty、runtime-quarantine-reopen。真实system两项经公共runtime入口调用，simulated=false；明确缺环境时对应集成未满足，不能改用fixture取得完整P0通过。CPU子集可独立报告成功，不代表真实加载集成通过。

### Requirement: 构建验证与输出隔离
实施 SHALL 先验证正式工程工具链及依赖来源，再运行两配置非空CPU/适用集成测试。Harness可按下述命令表调用，不要求人工操作先创建快照或等待gate；若采用snapshot，日常生成物默认放项目内 `<repo>/build/`（BUILD-001 §3.1），依赖安装树这类兼容性例外放已声明原因/归属/回收的任务自有根；无论哪种布局，都必须由命令表在configure前和ctest后各核对一次snapshot输入哈希，且清单排除输出目录。云端缺VS2026明确未满足，不换编译器或空job；云端CPU不证明本机GPU资格。

#### Scenario: 超时或输出过期
- **WHEN** 命令超时、产物身份过期、输入哈希改变或必需用例缺失
- **THEN** 验证失败，保留实际命令/部分输出，仅清理任务拥有的进程资源，不把旧证据或跳过当通过

##### D3命令和身份产物约定（本轮已实现1.1/1.2/3.2切片）


以下路径已在本轮落地：workflow/p0-checks.json（命令表）、scripts/p0_build_identity.py（记录/身份）、scripts/workflow_p0.py（表校验、依赖顺序、逐命令解析器）、scripts/build_checks.py（同一实现的命令行入口）。

1. 在版本控制中建立workflow/p0-checks.json，schema_version=1、change=bootstrap-engine-p0、commands数组，每项{id,argv,cwd,timeout_seconds,wrapper_exit,child_exit,parser_id,required,output_dir,expected_capability_source,depends_on,expected_cases}。expected_capability_source取system_loader/fixture_loader/fixture_vulkan_api/null；depends_on列出必须先成功的命令ID；expected_cases只用于parser_id=ctest-junit，声明该配置必须全部出现并通过的用例名。parser_id取json-toolchain/json-dependencies/json-identity/cmake-configure/msbuild-log/msbuild-cl/ctest-junit，每个parser把输出转成逐项事实而不是只看退出码。表中wrapper_exit/child_exit是声明字段，保持null；实际观察的退出码与耗时由runner写入<output>/logs/p0-checks/results.json，不写回被哈希的清单。python scripts/build_checks.py 选择该表并校验必需ID、依赖顺序和来源断言，再把白名单占位符snapshot/output/configuration展开为绝对路径，禁止自由shell字符串执行；没有表/解析器即阻断。旧工作流三项检查保留，其他change不自动使用此表。
2. 先运行记录命令，再运行各配置identity，顺序固定为toolchain-record→dependencies-record→identity-<configuration>→configure→identity-check-<configuration>→build→clcmd-<configuration>→ctest→verify-inputs-<configuration>：其中clcmd-<configuration>是带 --clean-first -- /v:n 的全量重建，用于留存真实编译命令行证据；verify-inputs在CTest之后重算输入哈希，证明整轮未改动被哈希的输入。
   - toolchain-record：python scripts/p0_build_identity.py record-toolchain --output <output>/toolchain.json --triplet x64-windows-static-md。记录VS实例/工具集/cl/CMake/SDK/generator/架构/triplet及triplet文件SHA256、/MDd与/MD；不联网。
   - dependencies-record：python scripts/p0_build_identity.py record-dependencies --snapshot <snapshot> --output <output>/dependencies.json --install-root <output>/installed --triplet x64-windows-static-md。该命令用固定完整vcpkg按manifest恢复，下载/buildtrees/packages均指向任务自有根，然后记录manifest哈希、baseline、tool commit、triplet和实际解析ports；网络或隔离失败即blocked，不写snapshot或用户共享checkout。
   - identity-debug/identity-relwithdebinfo：python scripts/p0_build_identity.py prepare --snapshot <snapshot> --output <output> --configuration <configuration> --test-hooks on|off --toolchain-record <output>/toolchain.json --dependency-record <output>/dependencies.json。
   - identity-check-debug/identity-check-relwithdebinfo：同一脚本check模式（必须传 `--test-hooks on|off`，与 prepare/configure 保持一致），重新计算清单、toolchain与dependencies摘要并比对既有sidecar；不同即停止。
   toolchain.json唯一格式{schema_version:1,vs_instance:string,toolset:string,cl_version:string,cmake_version:string,generator:string,windows_sdk:string,host_arch:"x64",target_arch:"x64",triplet:"x64-windows-static-md",triplet_file_sha256:SHA256,crt:{debug:"/MDd",optimized:"/MD"},vcpkg_tool_commit:string}。dependencies.json唯一格式{schema_version:1,manifest_sha256:SHA256,triplet:string,vcpkg_tool_commit:string,registry_baseline:string,resolved:[{name:string,version:string,port_version:integer,triplet:string,features:string[]}]}；resolved按name排序。两份记录均用与下述input_hash相同的确定UTF-8 JSON编码。
3. prepare在<output>/identity/<configuration>/生成build-input-manifest.json和build-identity.json。后者唯一格式{schema_version:1,build_id:SHA256,input_hash:SHA256,configuration:string,toolchain:object,dependency_manifest_hash:SHA256,build_options:{test_hooks:boolean},source:{revision,dirty}}；toolchain对象即toolchain.json内容，dependency_manifest_hash为dependencies.json规范编码的SHA256，build_options记录改变产物但不改变任何源文件的编译选项（当前仅AR_ENABLE_TEST_HOOKS，prepare以--test-hooks on|off显式声明且必须与configure一致）。manifest列相对路径和每文件SHA256；input_hash为该files数组的确定UTF-8 JSON编码（对象键排序、无多余空白、非ASCII不转义）的SHA256。build_id为{input_hash,configuration,toolchain,dependency_manifest_hash,build_options}同一编码的SHA256，禁止从编译日志解析身份。路径用正斜线；排除输出/BuildInfo自身；源码字节原样哈希，不对源码换行归一化。
4. CMake从指定build-identity.json生成BuildInfo，至少编入build_id、input_hash、configuration、build_options.test_hooks和source{revision,dirty}；sidecar记录与AR_ENABLE_TEST_HOOKS不一致时configure直接失败，不允许复用另一选项的sidecar。报告顶层input_hash回显source.input_hash，运行参数不能覆盖。每次build前先执行identity-check-<configuration>，不一致即停止并要求重新prepare/configure。wrapper只读该配置的build-identity.json，独立于EXE输出取得期望值；构建后运行身份一致性测试比较编入字段与sidecar。测试旧EXE、错误配置、被改源、陈旧sidecar都必须失败。此为本地一致性证据，不是抵抗可同时改写全部材料者的身份认证。
5. verify配置命令固定为cmake -S <snapshot> -B <output>/build-<configuration> -G "Visual Studio 18 2026" -A x64 -DCMAKE_TOOLCHAIN_FILE=<fixed-vcpkg>/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static-md -DVCPKG_INSTALLED_DIR=<output>/installed -DVCPKG_MANIFEST_INSTALL=OFF -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF -DCMAKE_EXPORT_NO_PACKAGE_REGISTRY=ON -DAR_BUILD_IDENTITY_FILE=<output>/identity/<configuration>/build-identity.json。工具实例、工具集、SDK由toolchain-record写入并在configure前核对；triplet命令值与toolchain.json/dependencies.json必须一致。依赖已由dependencies-record恢复，configure不得再触发manifest安装；若需重新恢复，回到dependencies-record并重新prepare。
6. build命令为cmake --build <output>/build-<configuration> --config <configuration> --parallel 2，其描述项depends_on包含configure-<configuration>和identity-check-<configuration>；CTest为ctest --test-dir <output>/build-<configuration> -C <configuration> --no-tests=error --output-on-failure --output-junit <output>/logs/p0-checks/ctest-<configuration>.xml。开发preset可继续使用，但不作为verify命令。identity、CTest及逐用例结果均记录输入/产物身份；输出要么位于snapshot之外的声明根，要么位于项目内 build/ 且被清单排除，两种布局都由configure前的identity-check和ctest后的verify-inputs-<configuration>分别核对输入哈希。
7. 必需命令为toolchain-record、dependencies-record、identity-debug/identity-relwithdebinfo、identity-check-debug/identity-check-relwithdebinfo、configure-debug/configure-relwithdebinfo、build-debug/build-relwithdebinfo、clcmd-debug/clcmd-relwithdebinfo、ctest-debug/ctest-relwithdebinfo、verify-inputs-debug/verify-inputs-relwithdebinfo。所有命令是直接执行，命令表内wrapper_exit/child_exit两个字段均为null，实际进程退出码与耗时写入<output>/logs/p0-checks/results.json；不再登记probe-inspect/probe-development或负向子进程命令。旧check/unittest/OpenSpec三项保留。
8. CTest必须执行本契约列出的命名用例；配置时静态登记期望用例集，验收解析CTest JUnit及逐用例结构化结果，要求预期集合全部出现、无跳过、无失败且身份与来源匹配。每个用例独占<output>/test/<configuration>/<case_id>/<run_id>，不能复用旧报告。命令层expected_capability_source=null，因为一轮CTest混合CPU和真实加载；来源按本契约的用例矩阵逐项核对。单纯CTest退出0或只发现一个测试不充分。
9. 单个命名测试正常结束时退出0只表示自身断言成立；崩溃/超时/缺结果均失败。受测runtime返回的ResultKind不是进程退出码；测试必须先对原始返回对象断言，再通过正式报告编码路径写证据，不能在测试层手造一份“正确”报告掩盖模块错误。


### Requirement: 摘要输入与隔离拒绝精确承接（MF-01及MF-02候选收口）

系统 SHALL 按本节区分runtime编码失败与没有查询产物，并精确返回由先前cleanup残留导致的重开拒绝。此节补全已有候选选择，不构成代码实施批准。

##### RunSummaryInput完整字段与输出映射

字段均拥有值数据，Error形状沿既有定义；不包含rhi或第三方JSON类型。

| 输入字段 | 类型/来源 | summary.json映射 |
| --- | --- | --- |
| schema_version | integer 1 | schema_version原值 |
| run_id | string，会话建立时固定 | run_id；不匹配会话身份为summary/internal_error及output_failed |
| build_id / input_hash / configuration / source | 编入BuildInfo的身份值，source沿D1形状 | 同名字段；必须与会话固定身份相同，不能运行时改写。不匹配时追加Error{stage=summary,category=internal_error}、result=output_failed、diagnostics_complete=false，不写伪造身份 |
| primary_result | ResultKind | primary_result，原业务结果不覆盖 |
| primary_error | optional<Error> | primary_error为原Error对象或null，完整保留stage/category/message/origin |
| cleanup_errors | vector<Error> | cleanup_errors原错误对象数组；core不解释其资源语义；超出下述摘要预算时按本小节“摘要预算回退”改为计数化 |
| resources_released | boolean | resources_released原值，指本次尝试拥有的资源；true时residual_resources必须为空，否则按输入冲突拒绝 |
| residual_resources | vector<{kind:instance/dispatch_state/os_module,state:retained/unknown}> | residual_resources原值数组，不含裸句柄；与resources_released=true并用属于输入冲突 |
| capability_json | optional<string>，UTF-8 JSON对象文本 | core只作语法/顶层object校验，解析后写capability对象；无值写null，不写capability_json字符串字段 |
| capability_encoding_error | optional<Error>，runtime编码失败通道 | 有值时完整复制原Error进diagnostic_errors，保留全部字段；不另输出同名顶层字段，不覆盖primary_error |

diagnostic_errors由core合并输入编码错误、本次解析/输出错误及会话已记录诊断错误产生；同一编码错误只合并一次，不在finish时重新上报重复事件。result和diagnostics_complete是core根据上述输入及实际输出结果产生的输出字段，不允许调用方预填成功。

四种输入组合固定如下：

| capability_json | capability_encoding_error | 精确结果 |
| --- | --- | --- |
| nullopt | nullopt | capability:null；表示无查询产物，不新增诊断失败；result仍按原业务/cleanup及实际输出状态计算 |
| 有值 | nullopt | 恰好一个UTF-8 JSON object才嵌入；非法JSON、空串、数组/标量均summary/output_failed，result=output_failed、diagnostics_complete=false、capability:null |
| nullopt | 有值 | runtime必须传Error{stage=summary,category=output_failed,...}；core保留原Error到diagnostic_errors，result=output_failed、diagnostics_complete=false、capability:null |
| 有值 | 有值 | 输入冲突；保留原编码Error并追加Error{stage=summary,category=internal_error}，result=output_failed、diagnostics_complete=false、capability:null；不尝试用JSON取得成功资格 |

编码错误字段中stage/category不符合约定也属于输入不一致，追加summary/internal_error并同样output_failed。所有失败保留primary_result/primary_error及清理事实；摘要自身无法写入时不保证这些事实已落盘，返回EndResult及最低限度stderr保存可观察原因。能力语义由runtime负责，core不检查Vulkan支持。

摘要预算回退：摘要编码超过摘要预算时先按单条消息上限裁剪各Error的message；仍超预算则允许把cleanup_errors/diagnostic_errors计数化，写cleanup_error_count/diagnostic_error_count并置errors_omitted=true，但必须保留primary_result/primary_error/身份块与resources事实，并置summary_truncated=true、result=output_failed、diagnostics_complete=false。计数化回退属于有界输出的合法降级，不得用来丢弃primary_error或把运行标成成功。

输入一致性：resources_released=true且residual_resources非空，或build_id/input_hash/configuration/source与会话固定身份不一致，均属于输入冲突；core追加Error{stage=summary,category=internal_error}，result=output_failed、diagnostics_complete=false、capability:null，保留原业务错误及资源事实，不自行删改残留或伪造身份。该拒绝不新增错误枚举，也不把冲突解释为Vulkan能力不足。

#### Scenario: 编码失败与无查询可区分
- **WHEN** 分别传入无产物、编码成功、编码失败及双字段冲突
- **THEN** runtime-summary-capability按上述四行逐项核对；无产物不自动失败，编码失败保留原Error，冲突精确追加summary/internal_error，原业务错误和资源事实均保持

##### 隔离拒绝的精确结果和顺序

普通请求有效时，runtime先建立诊断，再调用rhi::open；rhi在任何本轮OS加载/volk初始化/instance创建之前检查进程隔离标志。已有残留或未知全局状态则返回失败，runtime形成StartFailure：result=initialization_failed，primary_error.stage=cleanup、category=cleanup_failed；message明确因先前残留隔离，origin.domain=internal，origin.code固定为1表示quarantined_process（仅此domain的项目内部码）。stage=cleanup描述故障来源，不代表本次执行了清理动作，不新增错误枚举。

本次无RhiSession、不调用该会话close；load_calls=0、volk_initialize_calls=0、instance_create_calls=0。StartFailure.resources_released=true、residual_resources=[]、cleanup_errors=[]，仅指本次未取得资源，绝不清除或掩盖进程既有隔离状态。该状态通过primary_error的明确category/stage/origin表达，测试另核对进程标志仍置位。部分能力报告仍为not_queried，product=false。诊断成功则正常finish；若finish输出失败最终output_failed而primary_error仍为隔离原因。

诊断建立失败时结果output_failed，不进入rhi，隔离检查调用计数0；不能声称已经完成隔离检查。先前被隔离的旧会话不重试；本次StartFailure没有会话也不能调用close。再次同进程open依旧拒绝，隔离不会因一次拒绝或诊断结束被清除。

#### Scenario: 残留后重开被精确拒绝
- **WHEN** 在专用子进程中已产生残留，再以有效配置连续启动两次
- **THEN** runtime-quarantine-reopen核对上述精确错误、无新会话、加载/初始化/创建/close调用均0、空本次残留、正常诊断结束且进程隔离保持；诊断建立失败的独立子用例核对output_failed及隔离检查0

#### Scenario: 正常空清理不被判成故障
- **WHEN** instance尚未创建且没有本会话资源需要释放
- **THEN** runtime-cleanup-empty核对closed=true、resources_released=true、residual_resources=[]及cleanup_errors=[]；instance未创建不能作为cleanup故障注入条件

runtime-cleanup-failure单独使用“已持有instance但对应销毁入口不可用”的项目控制层故障，断言实例和依赖入口/模块保留、不得卸库；另测实例已销毁且入口已清除后的OS卸库返回失败，仅模块引用残留。不伪造vkDestroyInstance失败返回，不把空清理判成故障。
