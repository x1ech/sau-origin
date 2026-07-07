# SAU RTL 双命令基线 → 交接

## 矩阵
M=64, K=256, N=256 (int8 GEMM, sau_testdata.h)

## 双命令
| cmd_id | cycle | 时长 |
|--------|-------|------|
| 1 | 0 → 2772 | 2772 |
| 2 | 3125 → 5897 | 2772 |

## 每命令 key params
- arrayFillLatency: 343
- drain: 80
- writeback: 256×1
- flowBoundaryGap: 235×7 (flow_loop_times=8)
- 状态: IDLE→REGISTER_LOAD(256)→TRANSPOSE_LOAD(32)→8×(REUSE_LOAD+CLIP/FIRST+D_OUT)→REGISTER_UNLOAD(265)→IDLE

## 包内文件
- architecture.csv / diagnostic.csv / manifest.json / summary.json / analysis.md
- sim_run1.log / sim_run2.log / SHA256SUMS
- validate_sau_trace.py / test_validate_sau_trace.py / gen_manifest.py
- testcase_INT8_SAU_MATMUL_TEST_ID_0/*.hex
- source_diff/* (TB instrumentation, Makefile, verilog.f, README)

## 验证
```bash
cd /home/xch/work/npu_lpnpu
sha256sum -c sim/vcs/build/sau_regress/baseline/int8_gemm_32x32/SHA256SUMS
python3 -m unittest sim.vcs.script.case_sau_regress.test_validate_sau_trace -v
```
