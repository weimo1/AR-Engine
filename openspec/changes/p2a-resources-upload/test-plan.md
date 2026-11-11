# 验证计划

| 用例 | 输入与断言 | 环境 |
| --- | --- | --- |
| `rhi-vma-buffer` | 真机创建 host-visible 与 device-local Buffer，验证已知字节、失效句柄和按序关闭 | GPU |
| `rhi-vma-image-view` | 真机创建 RGBA8 Image/View，拒绝非法描述和错误释放顺序 | GPU |
| `rhi-upload-readback` | 上传并经 GPU 读回 Buffer 与 Image，比较每个字节 | GPU |
| `rhi-resource-retirement` | 受控推进完成令牌，验证立即失效和延迟回收 | CPU |
| `rhi-upload-retirement` | 真机提交后释放资源，等待完成并安全关闭 | GPU |

真机用例在 Debug 启用 Validation，传输用例启用同步检查。按 [开发入口](../../../AGENTS.md) 先运行验收失败用例，再运行双配置完整门禁和独立评审。P1 的呈现、截图及缩放作为回归。
