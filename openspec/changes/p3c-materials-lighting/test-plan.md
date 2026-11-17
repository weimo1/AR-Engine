# 验证计划

| CTest | 观测及通过依据 |
| --- | --- |
| `assets-opaque-material` | 自制单 primitive glTF，分开核对缺省、显式因子和贴图。用非端点 RGB 和不同 B/G 检查 sRGB/线性解释、通道顺序及因子乘积。检查无效引用、非法因子和坏贴图，复用 P3b 夹具验证兼容 |
| `sample-gbuffer-probes` | 两片重叠表面、非均匀缩放的已知法线、上下不对称的投影探针和空背景。读回各通道，验证量化、法线逆转置和位置重建；确认近表面覆盖远表面并校验 Debug Vulkan Validation |
| `sample-hdr-lighting-probes` | 固定光源与材质、CPU 参考及两档曝光。中灰验证单次编码，高亮验证 HDR 大于 1；EV 0 和 -1 的 HDR 读回逐通道相等，最终 SDR 变暗。HDR 对 CPU 参考绝对误差 ≤0.012，SDR 对参考绝对误差 ≤3 个字节级码值，背景全黑 |
| `sample-ibl-probes` | CPU 用 `render-environment-lighting` 核对六面与离中心方向、完整 mip 和 BRDF 能量。真机对照直接光基线，检查常量 RGB 的归一化能量、方向环境中 +Z 主导、粗糙度 0.08 和 1.0 的高光色差、黑色背景及同步验证。首张漫反射上传完成后故障注入，复测 HDR 与原环境逐通道相等 |

每批跑红测和双配置完整检查。GPU 用例登记为 `gpu`，本地验证真实执行；独立评审核对冻结版本。使用合成资产做确定性验收，Suzanne 用作额外展示。以上为计划，尚未执行。
