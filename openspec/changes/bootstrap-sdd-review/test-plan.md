# 验证计划

| 要求 | 检查 | 通过条件与限制 |
| --- | --- | --- |
| 文档入口 | `python scripts/workflow.py check` | 自有文档本地链接有效；分类与确认范围由独立只读审查核对 |
| 单源派生 | `derive bootstrap-sdd-review` 与单元测试 | 正常派生一致；源变化、缺文件、多文件、重复/非法块失败 |
| 本地工具 | setup-workflow、doctor、OpenSpec schema validate | npm锁文件恢复；OCR SHA-256与版本匹配；Node满足实际版本要求 |
| SDD 兼容 | OpenSpec validate --strict、status、instructions | schema可解析、delta结构有效、应用上下文可生成；不代表实现验收通过 |
| Review 输入 | review-prepare、读取ocr-preview | 原生OCR可选中文档/脚本，输入快照与指纹一致；无外部模型调用 |
| Review 失败路径 | Python 单元测试与真实 pending 示例检查 | 过期内容/索引、缺覆盖、不独立声明、partial、阻塞项导致非零结果 |
| 新文件类型 | 隔离临时 Git 夹具 | .h/.cpp/.glsl/.vert/.frag/.comp、测试、CMake、文档均被选中；不在用户代码中注入缺陷 |
| 独立审查 | 冻结版本的只读上下文审查 | 核对工具实际能力与文档陈述，问题修复后复核；不宣称 OCR 检出率已达标 |

所有临时测试只写任务拥有的 TemporaryDirectory，限制子进程时长；不接触 GPU、编辑器输入、系统音量或账户。JSON 合格夹具只用于检查器测试，不作为实际项目审查。
