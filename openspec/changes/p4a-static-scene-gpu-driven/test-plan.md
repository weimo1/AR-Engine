# 验证计划

| 批次 | 验证 |
| --- | --- |
| T34 | CPU 红测先覆盖异构索引、多个实例复用同一网格、材质引用、空输入及边界拒绝 |
| T35 | `sample-bindless-material-probes` 真机覆盖两个材质的色彩与数据探针、候选回滚和成功替换。扩展 `rhi-set-binding-retirement`，断言两个帧槽完成前不写入或释放，完成后只写变化槽位 |
| T36 | `sample-static-scene-gpu-driven` 运行同一双材质场景，独立计算 CPU 可见 ID，对照 GPU 命令读回、GBuffer 和最终 PNG。每路运行五帧，覆盖零可见和容量一的截断，运行 Debug 同步验证 |

每批沿用双配置完整门禁与独立评审。
