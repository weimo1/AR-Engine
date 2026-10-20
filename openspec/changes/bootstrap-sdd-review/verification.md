# 验证记录：本地流程接入

日期：2026-10-19。范围：本次文档与工作流工具；尚无引擎源码，不包含 C++、Shader 或 GPU 功能验收。

历史输入声明：下表只对应 2026-10-19 当时的输入，计数不随仓库更新；不能作为当前整仓检查或审查通过依据。当前输入须由 complete-local-workflow 的验证日志及完整复审另行证明。

当时已实际执行：

| 检查 | 结果与证据 |
| --- | --- |
| setup-workflow.ps1 | npm ci 恢复 57 个本地工具依赖；Windows PowerShell 实际运行成功；OCR SHA-256 与锁定值相同，版本 1.13.1 |
| doctor | 项目文档/派生检查、OpenSpec 1.14.3、OCR 版本及本地 Git 根可读取 |
| workflow.py check | 51 份自有根/docs 文档、281 个本地链接及工具/派生配置通过；计数为记录时的检查范围 |
| Python unittest | 10 项测试通过，含原生 OCR 临时 Git 夹具、冻结副本与选择集合变化的负向测试；合法记录夹具只是检查器测试，不是项目 Review |
| OpenSpec schema validate ar-engine | valid:true，无问题 |
| OpenSpec validate bootstrap-sdd-review --strict | valid:true，无问题 |
| OpenSpec instructions apply | 实际加载 ar-engine 与项目指令；未回退默认配置 |
| OCR 项目规则解析 | Device.cpp 路径命中 Custom (--rule) 与项目规则；不是 Vulkan 正确性证明 |

修正过程中发现：既有链接带 `:行号`，检查器需分离文件定位；父目录旧依赖稿已不在原位置，更新为项目 ADR；OpenSpec 初始化原有 context 与追加配置出现重复键，已合并并增加唯一键检查。没有将这些失败写为第一次即通过。

首轮独立审查指出并已修正：旧根资料缺少历史定位；生成技能裸命令未映射锁定本地版本；冻结快照和 OCR 选择集合未纳入充分核验。修复后增加负向测试，并对变化文件提交复核。安装脚本另增加临时下载/校验后发布与 Python 版本说明；最终审查结论以对应 packet 为准。

最终冻结、OCR 选择结果、逐文件独立审查和本地核验记录存放在忽略目录 `.ar-workflow/reviews/<run>/`，使用 manifest 的内容指纹定位。此文不预填最终独立审查结论；检查器只核对本地记录一致性，不认证模型身份或授予远程合并资格。

未执行：引擎编译、真实 CPU/Shader/GPU 验收、OCR 缺陷检出率评测、外部模型端点、hooks、CI、远程保护与合并。不归档未完成的引擎能力，也没有创建或推送远程仓库。
