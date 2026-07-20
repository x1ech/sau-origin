# Im2Col -> project-owned 16x16 SA Step 8 工作站包

> 顶层模块名为兼容旧工具仍含 `mikui`，实际 pipeline filelist 不再编译或实例化 Mikui
> `SA_ENGINE/SA_ROW`。冻结边界见 `src/sau_n/SAU_FREEZE_SCOPE.md`。

该包在VCS工作站上执行三层验证：

1. 原有reference Im2Col四项legacy回归；
2. 项目自有 16x16 SA 的六项独立验证；
3. 七个 Im2Col -> 单 tile buffer -> 项目自有 SA 端到端 profile。

端到端运行写出53字段canonical `trace.csv`、NCHW `output.csv`、fixture manifest和
总`result_manifest.json`。每个RTL output在工作站上先与独立Python convolution oracle
比较；返回后仍须在gem5主机执行严格逐拍比较，才能完成Step 8最终验收。

## 环境

- Linux；
- Synopsys VCS，目标版本`T-2022.06`；
- Python 3.10或兼容版本；
- bash；
- 不需要联网或第三方Python包。

## 运行

在解包后的根目录执行：

```bash
PYTHONDONTWRITEBYTECODE=1 \
util/conv_pipeline/step8/run_step8_vcs.sh \
    --outdir /absolute/path/to/sau_step8_results
```

若VCS不在`PATH`：

```bash
PYTHONDONTWRITEBYTECODE=1 \
util/conv_pipeline/step8/run_step8_vcs.sh \
    --vcs /absolute/path/to/vcs \
    --outdir /absolute/path/to/sau_step8_results
```

仅检查确定性命令：

```bash
util/conv_pipeline/step8/run_step8_vcs.sh \
    --outdir /tmp/sau-step8-dry-run \
    --dry-run
```

脚本拒绝覆盖已有输出目录。全部通过时输出：

```text
PASS Step 8 workstation matrix outdir=...
```

请保留完整结果目录，包括 compile/run log、六个 standalone trace、七组 pipeline
trace/output/manifest以及顶层`result_manifest.json`。不要只返回PASS文本，也不要修改
RTL、fixture、verifier或生成的manifest来迁就失败。

## 返回后的验证

在原gem5工作区运行：

```bash
PYTHONDONTWRITEBYTECODE=1 \
python3 util/conv_pipeline/step8/validate_step8_results.py \
    --results /absolute/path/to/sau_step8_results
```

该命令校验全部source/config/artifact hash，重新验证RTL output与oracle及canonical
trace。若同时已有七组gem5运行结果，可增加：

```text
--gem5-results /absolute/path/to/gem5_pipeline_results
```

执行七组严格逐拍比较。没有 `--gem5-results` 时只确认融合 RTL 与 oracle，不声明
gem5/RTL per-cycle validation passed。
