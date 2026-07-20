# Im2Col + 16x16 SA gem5 迁移交付说明

## 应用基线

```text
repository: https://github.com/x1ech/sau-origin.git
branch: feature/sau-command-types
base commit: b9fbc18c20f387bd537d5318d7f7b5ecfe63aa8f
```

交付压缩包是相对于上述基线的源码覆盖包。将压缩包解压到同一 gem5 仓库根目录即可；
包内不包含 `.git`、`build/`、测试临时输出或用户环境配置。

## 实际冻结对象

融合 pipeline 为：

```text
gemmini_im2col_chw_gather_readable
  -> single-tile adapter
  -> project-owned sau_array_16x16
  -> NCHW output collector
```

部分顶层文件、模块和工具路径继续保留 `mikui` 字样，仅用于兼容原 Step 8 工作流。
实际 pipeline filelist 不编译或实例化 Mikui `SA_ENGINE`、`SA_ROW` 或 delay 层次；
这些历史源码仅作为来源证据保留。详细边界见 `src/sau_n/SAU_FREEZE_SCOPE.md`。

## 周期契约

- 阵列状态：`IDLE/STREAM/DRAIN/BIAS/OUTPUT = 0/1/2/3/4`；
- PE MAC commit：输入后 `2 + row + column` 拍；
- 末输入到全阵列 bias 和首输出：33 拍；
- 输出在 `valid && grant` 同拍交付；
- 末行握手后一拍产生 `cal_finish`；
- `pe_finish` 在第 0 输出行有效期间保持；
- `row_ready_mask` 对当前 `os_valid_mask` 晚一拍更新。

## 验收结果

- Im2Col legacy：通过；
- 项目自有阵列六项 standalone VCS/oracle 验收：通过；
- 七项融合 RTL 功能和独立卷积 oracle 验收：通过；
- gem5 官方 testlib：7 个运行、7 个退出检查、7 个 strict verifier，共 21 项通过；
- 七份 53 字段 gem5/RTL trace：合计 7,772 个 cycle，无字段差异；
- 七项 NCHW output：全部逐字节一致；
- Python：42 项通过；
- `git diff --check`：通过。

七项融合 profile 的 drained cycle 为：

```text
01_c1_w1_oc1_ones:       85
02_c2_w5_oc3_pack:       276
03_c3_w16_oc16_full:     574
04_c3_w17_oc7_dil2:      1754
05_n2_c4_w20_oc15_s2:    1551
06_c63_w1_oc16_maxk:     3061
07_c2_w5_oc3_outbp:      464
```

当前可以声明：

```text
Im2Col-to-project-owned-16x16-SA gem5/RTL per-cycle validation passed.
```

## 复现

构建 `build/RISCV/gem5.opt` 后，从仓库根目录执行：

```bash
cd tests
./main.py run --skip-build gem5/conv_pipeline
```

预期结果为：

```text
Results: 21 Passed
```
