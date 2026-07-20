# Step 8 Im2Col -> 16x16 SA 融合冻结范围

## 当前黄金对象

```text
gemmini_im2col_chw_gather_readable.sv
  + im2col_mikui_sau_pipeline.sv 中的 single-tile adapter/collector
  + project-owned sau_array_16x16.sv
```

顶层文件和模块为了兼容原 Step 8 工具继续保留 `mikui` 名称，但综合层次中不再实例化
Mikui `SA_ENGINE`、`SA_ROW` 或 `active_delay`。这些文件仅作为只读来源证据保留。

## 已完成验收

- Im2Col legacy 四项回归；
- 项目自有阵列六项独立 VCS/oracle 回归；
- 七项 Im2Col -> tile buffer -> 16x16 SA -> NCHW 端到端 RTL 回归；
- 每个融合 tile 的有效 PE 恰好 K 次 MAC 和一次 bias commit；
- 全 16 列、tail、K=9、K=567、packing、width splitting、dilation、stride 和
  output backpressure；
- 所有 RTL 输出逐元素匹配独立 Python convolution oracle。

## 尚未完成

工作站包不包含 `src/sau_n/conv_pipeline_io.hh` 及完整 gem5 C++ 模型。新阵列的状态机、
drain 和输出 ready/valid 周期与旧 Mikui `SA_ENGINE` 不同，因此旧 gem5 trace 不能直接
沿用。必须在完整本地树中更新模型后，使用本包生成的新 RTL trace 做七项严格逐拍比较，
才能声明 gem5/RTL per-cycle validation passed。

当前允许的声明是：

```text
Im2Col-to-project-owned-16x16-SA fused RTL functional validation passed;
gem5 strict per-cycle comparison pending.
```
