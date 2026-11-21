# 验证计划

| 用例 | 证据 |
| --- | --- |
| `render-graph-plan` | CPU 固定图验证颜色/深度、读写依赖、外部导入导出、按 mip/layer 隔离。错误输入覆盖未初始化读、冲突访问、越界与缺少间接访问声明。写入 → Fragment 与 Compute 两次兼容读取 → 再写入，断言最后的写依赖包含两次读取。丢弃覆盖仍依赖此前的访问。 |
| `sample-gbuffer-probes`、`sample-hdr-lighting-probes`、`sample-ibl-probes` | 沿用 P3c 的固定场景、容差与同步验证，比对迁移前后的探针输出。 |
| Graph 路径 resize | 在至少两帧槽轮换后触发 resize，确认新尺寸输出与读回有效，旧附件收敛后释放。 |

每批按开发流程先见红测，再跑双配置完整门禁和独立评审。
