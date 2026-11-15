# 验证计划

| CTest | 检查 |
| --- | --- |
| `render-culling-reference` | CPU 固定输入，覆盖内外、相交与容量边界 |
| `rhi-gpu-culling-reference` | 真机读回 GPU 对象标识和计数，与 CPU 参考比较 |
| `sample-gpu-driven-pixels` | 真机检查混合场景和空集合像素、连续帧清零及关闭 |

真机用例加 gpu 标签。按根规则跑 TDD、完整双配置门禁及独立复审。

有图形问题需要抓帧时，由用户用 RenderDoc 抓取固定场景。回放可检查 Compute dispatch、间接绘制、命令及计数缓冲和最终画面。记录工具版本、场景与事件，捕获保存到 `build/`。
