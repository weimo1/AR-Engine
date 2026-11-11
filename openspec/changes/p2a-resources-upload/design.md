# 技术方案

[RHI 资源与上传](../../../docs/modules/rhi/README.md#p2a-资源与上传) 定义所有权、同步和关闭顺序。本切片沿用已有 graphics 队列，依次完成 allocator 与 Buffer、Image/View、上传与读回、在途退役。当前 vcpkg baseline 提供 [VMA 3.3.0](https://raw.githubusercontent.com/microsoft/vcpkg/cb2981c4e03d421fa03b9bb5044cd1986180e7e4/ports/vulkan-memory-allocator/vcpkg.json)，以实际依赖恢复和联编验证版本。

最小图像路径使用单 mip、单层 RGBA8。读回用确定的字节和像素数据验证，资源退役用受控完成信号补足快速设备可能提前完成的覆盖。见 [验证计划](test-plan.md)。
