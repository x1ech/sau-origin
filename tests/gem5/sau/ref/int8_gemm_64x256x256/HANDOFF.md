# SAU RTL 周期基线 → gem5 任务 2 交接

> 这是 `2026-07-06-sau-task1-rtl-baseline-handoff.md` (Task 1) 的最终交付。
> 任务 2（gem5 SAU trace comparator / 模型标定）只需读这一份和下面列出的
> 几个稳定产物，不需要再读 RTL 源码。

## 1. 来源环境（用于追溯，不需要复制）

* Repo HEAD: `c0d588a28fefdab28b7770d90486277c56b40acd`
* 工作树整体 diff 指纹: `0424c4c0cc88c915b2283c4a556698491446afc67b95138ea9c520b6b7bb89ca`
  （`git diff --binary HEAD` 的 SHA‑256）
* Simulator / 版本: `VCS T‑2022.06_Full64`
* 平台参数: `SA_SIZE=32`, `SRAM_DATA_WIDTH=256`, `beat_bytes=32`

## 2. 维度偏离（务必先读）

Task 1 计划原文要求 `INT8_SAU_MATMUL_TEST_ID_0` = **32×32×32** int8 GEMM。
本仓库现成 `software/benchmarks/yinglong_sau_test/sau_testdata.h` 中 `TEST_ID==0`
的实测维度是 **M=64, K=256, N=256**。本机缺 `_ctypes`/`matplotlib`/`torch`，
无法重跑 `sau_matrix_yl.py` 生成纯 32×32×32 稳定 hex。经用户明确放宽，
**本基线用 64×256×256**。这意味着：

* `architecture.csv` 的七列 **schema 不变**，任务 2 可直接消费；
* 但**绝对周期数不能外推**到 32×32×32 或其它维度；
* `analysis.md` 中所有量纲都已显式标注维度， Giants 247 / 171 / 80 / 4 等
  数值仅在 64×256×256 + `flow_loop_times=8` 下成立。

要拿到真正 32×32×32 的基线，需要另一台能跑 `sau_matrix_yl.py` 的机器生成
testcase 后重新跑 `timing_trace`（见文末复跑命令）。

## 3. 任务 2 应直接消费的三个稳定产物（按优先级）

```
baseline/int8_gemm_32x32/
├── architecture.csv    ← 1. 公共 7 列 trace，任务 2 的核心输入
├── manifest.json       ← 2. 基线身份与 32‑byte beat 契约
└── analysis.md         ← 3. token 粒度与时序参数解释（15 问）
```

其它附带产物可作为佐证，但**不应**作为 gem5 公共接口：

* `diagnostic.csv` — 每周期内部信号快照，仅在跨工程 debug 时使用。
* `summary.json` — `architecture.csv` 的派生，可用 `validate_sau_trace.py`
  重建，任务 2 不应硬编码它内部的字段名（decoder 可能会变）。
* `sim_run1.log` / `sim_run2.log` — 仅证明两次仿真 `TEST PASSED`。
* `SHA256SUMS` — 用于一次性核验全部 11 个文件没被中途修改。

## 4. `architecture.csv` schema 契约（与计划 §4.2 逐字一致）

```csv
cycle,event,command_id,stream,address,beat,phase
```

* `cycle`: zero‑indexed 整数；command_accepted 周期归一化为 0；
  单调递增，非仿真绝对时间。
* `event`: 下列之一
  `phase_changed` / `command_accepted` / `read_accepted` /
  `read_response_visible` / `array_input_accepted` / `result_produced` /
  `write_accepted` / `command_complete`
* `command_id`: 单命令基线固定为 1。
* `stream`: `none` / `operand_a` / `operand_b` / `output`
* `address`: 小写十六进制 `0x%08x`；无地址事件写 `0x00000000`；
  read/write 地址均按 32 bytes 对齐。
* `beat`: 该 `(event, stream)` 维度上的零基连续编号。控制事件
  (`phase_changed`/`command_accepted`/`command_complete`) 固定 beat=0
  且不参与 beat 计数。
* `phase`: `idle`/`operand_load`/`array_active`/`array_drain`/`writeback`/`complete`

**Phase 转移顺序**（任务 2 用来划分 stage）：
`idle → operand_load → array_active → array_drain → writeback → complete`
每个转移先发 `phase_changed`，紧接着发同周期的 anchor 事件；其余同周期事件
按计划 §4.2 规定的顺序：

```
read_accepted
read_response_visible
array_input_accepted(operand_b)
array_input_accepted(operand_a)
result_produced
write_accepted
command_complete
```

## 5. 直接可读的关键数字（来自 `summary.json`，但任务 2 仍以 arch.csv 为准）

| 量 | 值 (cycle) | 备注 |
| --- | --- | --- |
| command 总长 | 2036 | command_accepted=0 → command_complete=2036 |
| first read          | 3    | `latency.command_to_first_read=3` |
| first array_input   | 77   | operand_load 进入 array_active |
| first result        | 324  | `arrayFillLatency=247` |
| last  array_input  | 1689 | 进入 array_drain |
| last  result        | 1769 | `drain latency=80` |
| first write         | 1777 | array_drain 进入 writeback |
| last  write         | 2032 | |
| command complete    | 2036 | `last_write_to_complete=4` |
| result interval    | min=1, mode=1, max=171 | 248 段×1 + 7 段×171 |
| array_a / array_b  | 1568 / 1568 | 每个 output 对应 6.125 个 array_input |
| results / writes    | 256 / 256 | **1 : 1** |

## 6. 给 gem5 的 token 粒度与时序参数建议（与 analysis.md §13/§14 一致）

### 6.1 Token 定义

* **1 token = 1 个外部 256‑bit SRAM beat（32 bytes）**
* 在 SAU 内部：
  - 每个 `read_accepted` 行 = 1 input token （A 或 B）
  - 每个 `array_input_accepted` 行 = 1 阵列 input token（A 与 B 同 cycle
    并发，按 (B, A) 顺序写两行）
  - 每个 `result_produced` / `write_accepted` = 1 output token
* 1 output token ⇐ 32 个 (A, B) input token pair + 1 个 `flow_loop` boundary
  gap（`flow_loop_times=8`）。
* **不能** 把 “1 input token → 1 output token” 当作通用假设；它只在稳态
  per‑flow 内才成立（参见 analysis §15）。

### 6.2 时序参数

| gem5 参数 | 推荐值 / 表达式 | 证据 |
| --- | --- | --- |
| `arrayFillLatency` | **247 cycles** | first_array_input → first_result |
| `arrayInitiationInterval`（per result） | **1 cycle** | result interval mode=1 |
| `arrayActiveSpan`（per flow 内） | 1612 cycles total | last − first array_input |
| `flowBoundaryGap` | **171 cycles** × 7 次 | result interval max=171 |
| `arrayDrainLatency` | **80 cycles** | last_array_input → last_result |
| `lastResultToFirstWrite` | **8 cycles** | array_drain 进 writeback |
| `writebackInitiationInterval` | **1 cycle** | 连续 256 个 write_accepted |
| `lastWriteToComplete` | **4 cycles** | writeback 进 complete |
| `sram_wr_last_ma → sau_crossbar_done` | **1 cycle** | 见 analysis §11 |

### 6.3 “one input token → one output token” 当前结论

**部分成立**。仅当：
* output token 定义为“1 个 256‑bit 外部写 beat”时与 result 1:1；
* 但 input token 与 output token **不是** 1:1 — 实测每 1 result 对应
  6.125 个 array_input（A、B 各 3.0625）；
* 32 个连续 (A,B) input 对 → 32 个连续 result 的稳态区间成立；
* flow 边界 171 cycle gap 必须显式建模，否则会把 SAU 计算量低估约
  `7×171 / 2036 ≈ 5.9%`。

## 7. 已知 invariant（任务 2 必须保持兼容）

1. `architecture.csv` 表头逐字与 §4 一致；不要为方便比较而改列名。
2. `command_id` 固定为 1（单命令基线）。
3. `address` 永远是 `0x%08x` 小写，read/write 地址都按 32 bytes 对齐。
4. 每个 `(event, stream)` 的 beat 从 0 严格递增 1；控制事件 beat=0。
5. 每个周期最多 1 行 `read_response_visible`，`(event, stream)` 由
   read FIFO 出队决定；任务 2 不应自行猜测。
6. `phase_changed` 必须先于同周期 anchor 事件出现；同周期事件顺序
   遵守 §4.2 规定。
7. 所有 trace 在 `rst_n_acc==1` 期间采样；reset 时归零。

## 8. 已知 workaround / 偏离（任务 2 应在文档中标注）

* **array_drain 触发条件**：§4.2 要求 “`last_flow_time_f && last_ins_time_s`
  与 `data_A_last/data_B_last` 同 cycle high”。本 RTL 中这两个信号
  **不**与 data_last 高同 cycle，所以本基线在 testbench 中用了 sticky
  `sau_last_ins_seen` 锁存 `last_ins_time_s`，然后在
  `(data_a_last || data_b_last) & sau_last_ins_seen` 触发 array_drain。
  ⇒ `phase=array_drain` 的行是确认存在的（cycle 1689），如果任务 2
  试图硬性校验 “三信号同时高”，会拿到空集——请按 sticky 解释接受。
  详见 `analysis.md` §“实现注记” 第 2 条。
* **dimen 偏离**：第 2 节已说明；任何把本基线数值外推到 32×32×32 的
  用户都应被告知风险。
* `diagnostic.csv` 中 `internal_write_valid` / `internal_write_last`
  分别对应 `register_wraddr_valid` / `sram_wr_last_ma`；后者只在
  `cycle=2035` 单 cycle 拉高（最后 1 个 write 的 end‑of‑packet 标记），
  不是每个 write cycle 都拉高——任务 2 不要把它写成 `write_req`。

## 9. 任务 2 的复跑 / 校验命令

如果你在远程服务器（`/home/xch/work/npu_lpnpu`）：

```bash
cd /home/xch/work/npu_lpnpu

# 只检查环境，不跑仿真
make -f sim/vcs/script/case_sau_regress/Makefile timing_trace_preflight

# 在新目录跑一次基线
make -f sim/vcs/script/case_sau_regress/Makefile timing_trace TEST_ID=0 \
     BASELINE_DIR=sim/vcs/build/sau_regress/baseline/my_run

# 两次结果比对
python3 sim/vcs/script/case_sau_regress/validate_sau_trace.py \
     --compare-run \
     sim/vcs/build/sau_regress/baseline/int8_gemm_32x32 \
     sim/vcs/build/sau_regress/baseline/my_run
```

如果要把本基线复制到 gem5 reference tree（需 `architecture.csv` 作为只读 golden）：

```bash
# 给 gem5 reference tree 的子集 — 仅 schema 与 trace，不要 diagnostic
cp /home/xch/work/npu_lpnpu/sim/vcs/build/sau_regress/baseline/int8_gemm_32x32/architecture.csv \
   <gem5_reference_dir>/sau/
cp /home/xch/work/npu_lpnpu/sim/vcs/build/sau_regress/baseline/int8_gemm_32x32/manifest.json \
   <gem5_reference_dir>/sau/
cp /home/xch/work/npu_lpnpu/sim/vcs/build/sau_regress/baseline/int8_gemm_32x32/analysis.md \
   <gem5_reference_dir>/sau/
```

## 10. 任务 2 不应做的事

* 不要修改 RTL（包括 `hardware/src/`、`sim/testbench/`、filelist 等）。
* 不要为了适配 gem5 当前实现回头改 `architecture.csv` 的 schema。
  如果发现 schema 不够用，先回到 `analysis.md` 中讨论是否扩展，不要私下加列。
* 不要把 `diagnostic.csv` 当作 gem5 公共接口；它只是核查证据。
* 不要假设 247 / 171 / 80 / 1 这些数字对其它维度也成立。
* 不要在没有用户提供 32×32×32 testcase 时声称 “RTL/gem5 周期对齐已完成”；
  本基线维度偏离意味着任务 2 的最终标定只能在用户接受 64×256×256 替代后
  才算完成，或需要任务 3 补一个 32×32×32 testcase。

## 11. 包内文件清单

```
sau_task1_baseline_handoff_to_gem5.md          ← 本文件
sau_task1_baseline.tar.gz                       ← 见下方打包命令
└── sau_task1_baseline/
    ├── architecture.csv
    ├── diagnostic.csv
    ├── manifest.json
    ├── summary.json
    ├── analysis.md
    ├── sim_run1.log
    ├── sim_run2.log
    ├── SHA256SUMS
    ├── validate_sau_trace.py                  ← 让任务 2 复用相同校验器
    ├── test_validate_sau_trace.py              ← 27 个单元测试
    ├── gen_manifest.py                          ← manifest 生成器
    ├── testcase_INT8_SAU_MATMUL_TEST_ID_0/     ← 5 个 hex，可复跑
    │   ├── instruction.hex
    │   ├── memory.hex
    │   ├── memory_mod_0.hex
    │   ├── memory_mod_1.hex
    │   └── memory_mod_2.hex
    └── source_diff/                            ← 让任务 2 agent 可选审计
        ├── top_sau_regress_tb.sv               ← instrumentation 写法参考
        ├── Makefile                            ← timing_trace target
        ├── verilog.f                           ← filelist 修复
        └── README.md                           ← 使用文档
```

## 12. 首接任务 2 时建议按此顺序读

1. 本 md → §2 维度偏离、§6 token/时序参数、§11 包内文件清单
2. `analysis.md` → §7 ~ §15
3. `architecture.csv` 头 30 行 + 末 30 行（看 phase 走向）
4. `manifest.json`（确认 testcase SHA 与本机 / 远程一致）
5. 若要写比较器：`validate_sau_trace.py` + `test_validate_sau_trace.py`
6. （可选）`source_diff/top_sau_regress_tb.sv` 末段 instrumentation 块，
   了解每个事件触发条件如何从 RTL 信号映射到 CSV
7. （可选）`source_diff/README.md` 的 timing_trace 章节

按以上顺序读完后，任务 2 应该能在不联机 RTL 服务器的条件下开始实现 gem5 侧
comparator 与单测；只有在需要新维度 / 新 testcase 时才回到 RTL 工作区。