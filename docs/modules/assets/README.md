# assets 模块

## 状态与依据

依赖选型见 [ADR-0007](../../adr/0007-基础构建与第三方依赖选型.md)。以下为 [P3b 技术方案](../../../openspec/changes/p3b-static-scene-assets/design.md)中的首批落点，尚未实现。

## 职责与模块关系

assets 解析、解码并缓存 CPU 资产。runtime 通过资产引用请求加载，scene 保存可持久化引用，render 管理 GPU 映射。解析器和解码器类型留在 assets 实现内部。

## 稳定契约与资源生命周期

资产引用在受控资产根下解析。解码并校验成功后发布不可变数据；实例可以共享数据。释放缓存后，仍由调用方持有的数据保持有效。失败返回资产引用和阶段，可修正后重试。

## 验证与实现边界

首批覆盖静态 glTF、外部 PNG 和 CPU 缓存，具体验收见 [P3b 验证计划](../../../openspec/changes/p3b-static-scene-assets/test-plan.md)。异步加载、热替换及资产数据库延期。
