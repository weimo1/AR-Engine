# ADR-0001：Vulkan 内存分配采用 VMA

状态：选型已确认，尚未集成。日期：2026-10-18（Asia/Shanghai）。

## 背景与确认来源

项目采用 C++23 与 Vulkan 1.4，需管理 Vulkan buffer/image 等资源的设备内存。用户明确指示“vulkan 使用vma”。本 ADR 记录该选型，不伪造用户确认前不存在的候选实验或性能比较。

## 已确认决策

采用 Vulkan Memory Allocator（VMA）作为 Vulkan 内存分配基础库。引擎资源管理在此基础上明确资源所有权、访问和回收责任，不默认另写一套通用 Vulkan 子分配器。

该选择针对 Vulkan 设备内存，包括适用的 host-visible 分配；不等同于确定 CPU 普通对象分配器，也不自动确定 Bindless、Descriptor Heap、上传系统或 Render Graph 实现。

## 官方能力边界

以下用于澄清选型的含义，实施时仍需对照所固定版本验证：

- VMA 提供分配管理及相关 buffer/image 创建辅助；数据上传/下载、命令记录与相关同步由使用方承担。
- 默认的部分 allocator 调用内部同步不代表任意 allocation 上的操作可以并发；具体线程安全边界依 VMA 文档和配置处理。
- allocator 依赖 VkDevice，上下文关闭时必须满足其资源与设备的有效生命周期。
- VMA 不自动证明 GPU 对象已不再使用；调用回收接口前仍需满足 [资源生命周期规范](../standards/C++类型接口与资源生命周期规范.md)。

参考：[官方 General considerations](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/general_considerations.html)、[官方 Quick start](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/quick_start.html)。这些资料是参考，未声称本项目已执行验证。

## 待技术方案确定

1. 固定 VMA 发布版本或提交，核对 Vulkan 1.4、SDK、函数加载方式及实际扩展。
2. 获取与构建集成方式、实现编译单元、宏配置、许可与依赖记录。
3. allocator 的所有者、封装范围、allocation 与资源的归属、创建失败清理。
4. 映射、上传、flush/invalidate、CPU/GPU 访问与同步契约。
5. 延迟销毁、场景切换和退出的处理及适用验证。
6. 池、预算、统计或碎片整理等能力按实际需求选用，不因库提供就全部加入第一版。

## 后果与重审

引入一项需要固定版本、维护配置和跟踪许可的外部依赖，避免把内存分配算法研究变成第一版额外目标。没有本项目测量数据，不宣称特定性能收益。

若实际兼容性或功能约束要求改变选型，提交新 ADR 与证据供用户审阅；不改写本次选型历史。本次未下载库、未配置构建，也未批准具体集成方案。
