# Mikui SA_ENGINE Step 0 工作站验证

该验证包用于冻结 Im2Col -> Mikui SAU 周期模型的 RTL 基线。它同时编译：

- upstream commit `2ca8252ef1cac43ef843998e9e08023259ac17ee` 的原始
  `SA_ENGINE`；
- 只修复 `FINISH_ROW/FINISH_COL` 常量位宽的 integration variant。

它不是完整 NPU、SA_CORE、feeder 或 SRAM 系统验证。最终 golden 对象明确是
“`2ca8252` with the documented finish-dimension patch”。

## 环境要求

- Linux；
- Synopsys VCS，目标版本为 `T-2022.06`；
- Python 3.10 或兼容版本；
- bash。

不需要 gem5 binary，也不需要联网或安装 Python 第三方包。

## 运行

在解包后的工程根目录执行：

```bash
PYTHONDONTWRITEBYTECODE=1 \
util/conv_pipeline/step0/run_step0_vcs.sh \
    --outdir /absolute/path/to/sau_step0_results
```

输出目录必须不存在；脚本不会删除或覆盖已有结果。若 VCS 命令不叫 `vcs`：

```bash
PYTHONDONTWRITEBYTECODE=1 \
util/conv_pipeline/step0/run_step0_vcs.sh \
    --vcs /absolute/path/to/vcs \
    --outdir /absolute/path/to/sau_step0_results
```

仅查看确定性命令而不编译：

```bash
util/conv_pipeline/step0/run_step0_vcs.sh \
    --outdir /tmp/sau-step0-dry-run \
    --dry-run
```

## 覆盖矩阵

original 和 integration 两个 DUT 均运行：

- `tail_r1_c1`、`tail_r15_c15`、`tail_r16_c16`；
- `mapping_k9`：非对称 row/column 数据验证物理 bit/lane 映射；
- `control_k9_bp`：配置、K 拍连续输入、输出请求和周期性反压；
- `sat_pos_k567`：第 512 次 MAC 首次正饱和；
- `sat_neg_k567`：第 517 次 MAC 首次负饱和。

trace 记录控制输入、内部状态、FINISH 尺寸、output grant、注册输出、256 个 PE 的
MAC/add commit mask 和 256×24-bit accumulator。验证器检查输出数据/顺序、tail
行为、反压覆盖以及 PE[0][0] 的每次饱和累加。

## 成功标志和返回内容

全部通过时最后输出：

```text
PASS Step 0 VCS matrix outdir=...
```

结果目录中应有：

```text
result_manifest.json
original/compile.log
original/*.log
original/*.csv
integration/compile.log
integration/*.log
integration/*.csv
```

`result_manifest.json` 记录 simulator 版本、host、源码/脚本 SHA256、每项 cycle/output
摘要及全部返回文件 SHA256。请保留整个结果目录并打包返回，不要只返回 PASS 文本。

若任一编译、仿真或验证失败，请连同已有输出目录和终端完整日志一起返回；不要修改
RTL、testbench 或 verifier 来迁就结果。
