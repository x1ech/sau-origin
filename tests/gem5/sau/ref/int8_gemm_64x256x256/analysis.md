# SAU RTL Timing Baseline — Analysis (Task 7)

> **双命令基线**：本 firmware 在同一次仿真中串行发出 **两次 SAU 启动**，
> 每次执行一个完整的 64×256×256 int8 GEMM，`flow_loop_times=8`。
> architecture.csv 包含 `command_id=1` 和 `command_id=2`，
> 两命令的 trace 结构完全相同（均 2772 cycles / 5 phase 转换）。

## 基线身份

* Repo HEAD: `c0d588a28fefdab28b7770d90486277c56b40acd`
* Simulator: VCS `T‑2022.06_Full64`
* Testcase: `INT8_SAU_MATMUL_TEST_ID_0`，矩阵 **M=64, K=256, N=256**
* 平台: `SA_SIZE=32`, `SRAM_DATA_WIDTH=256`, `beat_bytes=32`

## 双命令概览

| | command_id=1 | command_id=2 |
| --- | --- | --- |
| 周期范围 | 0 → 2772 | 3125 → 5897 |
| 时长 | 2772 cycles | 2772 cycles |
| `read_operand_a` | 256 | 256 |
| `read_operand_b` | 2048 | 2048 |
| `array_input` A/B | 2048 / 2048 | 2048 / 2048 |
| `result_produced` | 256 | 256 |
| `write_accepted` | 256 | 256 |

两命令间隔 353 cycles（idle）。phase 转移完全相同（见下）。

## 每个命令的核心数据

| 量 | 值 |
| --- | --- |
| `command_cycles` (per cmd) | **2772** |
| `arrayFillLatency` (first_arr→first_result) | **343** |
| `arrayActiveSpan` | 2157 cyc (269→2425 / 3394→5550) |
| `flowBoundaryGap` | **235** × 7 次 |
| `arrayDrainLatency` | **80** (last_arr→last_result) |
| `lastResultToFirstWrite` | **8** |
| `writeback` | 256 beat × 1 cyc |
| `lastWriteToComplete` | **4** |
| result interval min/max/mode | 1 / 235 / 1 |

## 每个命令的 core_state 时间线

```
IDLE → REGISTER_LOAD(256) → TRANSPOSE_LOAD(32) →
REUSE_LOAD×8 + TRANSPOSE_CLIP×7 + FIRST_LOAD×1 + D_OUT×8 →
REGISTER_UNLOAD(265) → IDLE
```

完整见下表（以 cmd_id=1 为例，cmd_id=2 偏移 +3125 后完全相同）：

```
IDLE                    0 →    1    (  2 cyc)
REGISTER_LOAD           2 →  257    (256 cyc)   ← 装载全部 A
TRANSPOSE_LOAD        258 →  289    ( 32 cyc)   ← A transpose
REUSE_LOAD            290 →  520    (231 cyc)   ← Flow 1
TRANSPOSE_CLIP        521 →  553    ( 33 cyc)
D_OUT                 554 →  555    (  2 cyc)
REUSE_LOAD            556 →  786    (231 cyc)   ← Flow 2
TRANSPOSE_CLIP        787 →  819    ( 33 cyc)
D_OUT                 820 →  820    (  1 cyc)
                      ... 共 8 flow ...
REUSE_LOAD           2151 → 2382    (232 cyc)   ← Flow 8
FIRST_LOAD           2383 → 2415    ( 33 cyc)
D_OUT                2416 → 2506    ( 91 cyc)   ← drain
REGISTER_UNLOAD      2507 → 2771    (265 cyc)   ← writeback
IDLE                 2772 → 2772    (  1 cyc)
```

## 每个命令的 arch phase 转移

```
cycle cmd1 / cmd2
  0    3125   operan_load      (command_accepted)
269    3394   array_active     (first array input)
2425   5550   array_drain      (last array input)
2513   5638   writeback        (first write)
2772   5897   complete         (command_complete)
```

## 15 个问题回答

与上一版单命令 analysis 完全一致（所有参数吻合），因为两个命令是同一指令流的重复。

### 1. command 总周期
per-command: **2772** cycles。

### 2. A/B 读与阵列输入
per-command: read A=256, read B=2048, array A=2048, array B=2048。

### 3. A/B 装载顺序
A 先装载（REGISTER_LOAD 256 cyc），B 后流式读。

### 4. `arrayFillLatency`
**343** cycles (first_arr=269 → first_result=612)。

### 5. result interval
mode=1, max=235 (7 次 flow gap), 一个 command 内 256 results。

### 6. drain 延迟
**80** cycles (last_arr=2425 → last_result=2505)。

### 7. result 是否在输入结束前产生
是。first result @ cycle 612，远早于 last_arr @ 2425。

### 8. result / write beat 比例
1:1 (256:256 per command)。

### 9. result_final_valid_o 到外部写 beat
1:1。

### 10. result 与 write 的周期关系
last_result @ 2505 → first write @ 2513 (8 cycles)。
last write @ 2768 → complete @ 2772 (4 cycles)。

### 11. sram_wr_last_ma 到 crossbar_done
1 cycle。

### 12. input_switch_s[0] → A/B 映射
`input_switch_s[0]==0` → operand_a，`==1` → operand_b。

### 13. gem5 token 定义
1 token = 1 个 256‑bit SRAM beat (32 bytes)。

### 14. 时序参数
| 参数 | 值 |
| --- | --- |
| `arrayFillLatency` | 343 |
| `arrayInitiationInterval` | 1 |
| `flowBoundaryGap` | 235 × 7 |
| `arrayDrainLatency` | 80 |
| `lastResultToFirstWrite` | 8 |
| `lastWriteToComplete` | 4 |
| REGISTER_LOAD 期 | 256 cyc |
| TRANSPOSE_LOAD 期 | 32 cyc |

### 15. "one input → one output" 假设
部分成立（同上一版结论）。

## 确定性与校验

- run1 / run2 均 `TEST PASSED`，architecture/diagnostic/summary 逐字节一致。
- 27 unit tests PASS。
- SHA256SUMS 全通。
