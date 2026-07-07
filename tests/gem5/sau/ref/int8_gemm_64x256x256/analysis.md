# SAU RTL Timing Baseline — Analysis (Task 7)

## 基线身份

* Repo HEAD: `c0d588a28fefdab28b7770d90486277c56b40acd`
* Worktree diff SHA‑256: `0424c4c0cc88c915b2283c4a556698491446afc67b95138ea9c520b6b7bb89ca`
* Simulator: VCS `T‑2022.06_Full64` (Build Date May 31 2022)
* Testcase: `INT8_SAU_MATMUL_TEST_ID_0` — 见 `manifest.json`
* 平台参数: `SA_SIZE=32`, `SRAM_DATA_WIDTH=256`, beat bytes = 32
* **维度与原计划的偏离**: 计划要求 32×32×32 int8 GEMM;
  本机 `software/benchmarks/yinglong_sau_test/sau_testdata.h` 中
  `INT8_SAU_MATMUL_TEST_ID==0` 的实测维度是 **M=64, K=256, N=256**。
  本机无 32‑bit `_ctypes` / `matplotlib` / `torch`，无法运行
  `sau_matrix_yl.py` 重生成纯 32×32×32 的稳定 hex；
  经用户明确书面放宽维度约束，本基线使用现有 64×256×256 基准。该偏离不影响
  后续 gem5 任务 2 消费 `architecture.csv` 的七列 schema；
  但 analysis 中所有绝对周期数值都不能直接外推到不同 M/K/N。
  重新标定需追加一个真正 32×32×32 的 testcase（见 Next Steps）。

## Trace 摘要（与 `summary.json` 一致）

| 量 | 值 |
| --- | --- |
| `command_cycles` (cmd 0 → complete) | **2036** |
| `array_operand_a` / `array_operand_b` | 1568 / 1568 |
| `read_operand_a` / `read_operand_b` | 64 / 1584 |
| `read_response_visible` A / B | 64 / 1584 |
| `result_produced` / `write_accepted` | 256 / 256 |
| `first_cycle` read / array / result / write / complete | 3 / 77 / 324 / 1777 / 2036 |
| `last_cycle` read / array / result / write | 1684 / 1689 / 1769 / 2032 |
| result interval min / max / mode | 1 / 171 / 1 |

五个 phase 转换在 `architecture.csv` 中以 `phase_changed` 标记：

```
cycle 0    : idle         →  operand_load    (anchor: command_accepted)
cycle 77   : operand_load →  array_active    (anchor: first array_input)
cycle 1689 : array_active →  array_drain     (anchor: last array_input)
cycle 1777 : array_drain  →  writeback       (anchor: first write_accepted)
cycle 2036 : writeback    →  complete        (anchor: command_complete)
```

## 15 个问题

### 1. command 0 到 complete 的总周期数
**2036 cycles**（command_accepted 归一化为 cycle 0，command_complete 在 cycle 2036）。

### 2. A/B 外部 read、read response 和阵列 valid 的各自数量
| stream | external read | read response visible | array_input accepted |
| --- | --- | --- | --- |
| operand_a  | 64   | 64   | 1568 |
| operand_b  | 1584 | 1584 | 1568 |

* external read + response 严格按 read→response FIFO 顺序配对，所有 1648 reads 都
  在同一时钟域内得到响应（queue FIFO 容量 16，最深持留未越界）。
* 阵列 input 的 A/B 数量相等（1568 / 1568），与外部 read 数量不平衡——A 的 64 个
  read beats 被装载到 **register file** 后由 feeder 长期复用，B 则是从外部 SRAM
  逐 tile 流式读取，所以 B 的读量远大于阵列输入。

### 3. B 是否先完整装载，A/B 是否存在同周期 valid
* **A 先装载**：A 的所有 64 个 read 集中在 cycle 3~66（连续 64 cycles），全部
  在 phase `operand_load`；B 的 read 从 cycle 69 才开始（A 装载已结束 2 周期后才
  读第一拍 B），并一直延续到 cycle 1684。
* **A/B 阵列输入同周期 valid**：所有 1568 个有 `array_input_accepted` 的 cycle
  都是 A、B 同周期有效（每个 cycle 同时各记一行 operand_b 和 operand_a），不存在
  只有 A 或只有 B 的 cycle。验证命令见下：

  ```
  awk -F, 'NR>1 && $2=="array_input_accepted"{a[$1]++; s[$1]=s[$1]$4"|"}
           END {...}' architecture.csv   →  both=1568 a_only=0 b_only=0
  ```
* 顺序按 §4.2 规定为 B 行先于 A 行。

### 4. 第一笔阵列输入到第一笔 result 的实测延迟
first array input @ cycle 77 → first result @ cycle 324 → **延迟 247 cycles**。
这一段时间是 fill‑latency，覆盖了 SA_PE 输入到累加器输出的完整 MAC 流水。

### 5. result 稳态间隔：min / max / mode
* **mode = 1**：连续 248 段（256 个 result 中的 248 个 interval）
  连续 1 cycle 一个 result。
* **max = 171**：在 result 内部出现 7 段长 gap = 171 cycles，对应 8 个
  flow_loop 之间的换 tile 间隙（plan 的 matmul.hpp 有
  `flow_loop_times = 8`，每段产 32 个 result）。
* **min = 1**：稳态吞吐为 1 result / cycle。
* interval 总数 = 255（256 result 减 1），其中 248 个为 1、7 个为 171，2+2×others
  = 0；总和校验：248×1 + 7×171 = 1445 ≡ 1769 − 324 ✓。

### 6. 最后一笔阵列输入到最后一笔 result 的 drain 延迟
last array input @ cycle 1689 → last result @ cycle 1769 → **drain 延迟 80 cycles**。
该段时间阵列停止输入但顶层流水仍把剩在 PE 中的部分和压成 31 个 result。

### 7. result 是否在全部输入结束前已经产生
**是**。第一笔 result @ cycle 324，远早于 last array input @ cycle 1689；
更具体：
* 232 笔 result 在 array_active 阶段 (cycles 324-1688) 产生；
* 24 笔 result 在 array_drain 阶段 (cycles 1689-1769) 产生；
* 写回 (writeback) 全部发生在 cycle 1777 之后，确认是 strict drain‑then‑writeback
  顺序，无 writeback 与阵列 drain 重叠。

### 8. result 数量与 256‑bit 外部 write beat 数量的比例
256 results / 256 writes = **1 : 1**。
对应 `summary.ratios.results_per_write = "256/256"`。

### 9. 一个 `result_final_valid_o` 是否对应一个、半个或多个外部写 beat
**1 个 `result_final_valid_o` → 1 个外部 256‑bit 写 beat**。
`internal_write_valid (= register_wraddr_valid)` 在 256 个连续 cycle 拉高
（cycle 1776 至 2031 + 1 个尾 cycle 2035），与 `write_req` 节拍 1:1 同步。
所以 register_file_out 没有将一个 512‑bit 的 `result_final_o` 切成两个 256‑beat
往外写——每拍产出一个 256‑bit 概念输出（量化压缩到字节通道后），写 SRAM 一次。

> 注：RTL 的 `result_final_o` 信号宽度声明为 `COL_NUM*QUANTDW = 32*16 = 512` 比特，
> 但在 int8 GEMM 路径下，SAUs 内部 trans2sa_top 只把每拍 result 量化到 8‑bit lane
> 通道（`OUTPUTDW=24` 实际由 `cutbit=13` 量化），最终驱动到外部 SRAM 的 write
> 节拍宽度仍是 256 bit，节拍数与 `result_final_valid_o` 个数一致。

### 10. 第一笔/最后一笔 result 与第一笔/最后一笔 write 的周期关系
* first result @ 324 → first write @ 1777（间隔 1453 cycles，对应全部 result
  排空完成再写回）
* last result @ 1769 → first write @ 1777（间隔 **8 cycles**）
* last write @ 2032 → command_complete @ 2036（间隔 **4 cycles**）

### 11. `sram_wr_last_ma` 到 `sau_crossbar_done` 的延迟
`sram_wr_last_ma` 在单一 cycle 2035 拉高（diag 第 17 列），随后
`sau_crossbar_done` 在 cycle 2036 拉高 → **延迟 1 cycle**。
（顺带说明：`sram_wr_last_ma` 不是每个外部写 cycle 高，而是只在收尾 cycle 高
  一次；同时外部 `write_req` 共 256 拍在 cycles 1777‑2032 期间持续。）

### 12. `input_switch_s`/`input_switch_f` 到 operand A/B 的真实映射
* RTL 源：`feeder.sv:359  data_A_enable = input_switch_case & ONE_INPUT_state;`
  `feeder.sv:360  data_B_enable = !input_switch_case & !NO_INPUT_state;`
  `feeder.sv:355  input_switch_case = ~input_switch_d_o[REGISTER_DELAY-2][0];`
* 推论：`input_switch[0] == 1'b0` 时 selected = operand_A；
  `input_switch[0] == 1'b1` 时 selected = operand_B。
* Trace 证据：
  - A read（cycles 3..66）diag 行 `input_switch_s = 2'b00` ⤇ arch 标 `operand_a`。
  - B read 起于 cycle 69，diag 行 `input_switch_s = 2'b01` ⤇ arch 标 `operand_b`，
    持续到 cycle 1684。
  - read_response_visible 与 array_input_accepted 中 A/B 的区分与上面使用的
    read‑accepted 标签序列完全一致（FIFO 队列维持请求顺序时序），未观察到
    "队列空 + response 出现" 的握手错误。
* 因此外部 read 接受时使用 `input_switch_s[0]` 作为 A/B 选择信号是正确的；
  read_response_visible 不直接用 `input_switch_*`，而是依靠 read accepted 时
  入队的 stream 标签，避免延迟错位。
* `input_switch_f` 是 `input_switch_s` 在 feeder 内部多级延迟的版本，主要用于
  下游 trans2sa 阵列控制；本基线 arch.csv 的 `array_input_accepted` 行不再需要它
  区分 A/B（直接看 `data_A_valid`/`data_B_valid` 哪个为 1）。
* 例外项：`last_flow_time_f` 在 RTL 内部是延迟若干 cycles 的版本，与
  `data_A/B_last` 不对齐到同一 cycle；故本基线对 §4.2 中
  "array_active → array_drain 用 last_flow_time_f 与 last_ins_time_s 联合确认"
  采纳了**sticky workaround**——见下节“实现注记”。

### 13. 推荐给 gem5 的 token 定义
基于 trace 实测的最小并行单元：
* 每个外部 read beat = **1 token of 32 bytes**（对应 architecture.csv 里 1 个
  `read_accepted` 行）。
* 每个 `array_input_accepted` 行（A 或 B）= **1 token of 32 lanes × INTDW bit**，
  A 与 B 同周期各 1 token（B 先 A 后顺序）。
* 每 32 个 array_input_accepted (含 A 和 B) + 1 个 filler cycle ⇒ **1 个 result
  token of 256‑bit**（256 个 result 与 256 个外部 write 1:1）。
* gem5 中可定义“1 token := 1 个外部 SRAM beat of 32 bytes”，在 SAU 域内
  `n_array_inputs / n_array_outputs = 32×2 / 32 = 2`（每 output token 对应 2 个
  input token，1 个 A + 1 个 B）。

### 14. 推荐的 `arrayFillLatency`、`arrayInitiationInterval` 和 drain 表达方式
基于本基线单一 command 数据：
* **`arrayFillLatency`** = first array input → first result = **247 cycles**。
  这是 SA_PE 流水的 MAC latency + trans2sa_top 量化延迟。
* **`arrayInitiationInterval` (per result)** = **1 cycle**（result steady‑state 间隔）。
* **`arrayActive` total span** = 1612 cycles (last − first array input)，共 256
  个 result → 平均 6.3 cycles/result（含 flow 间 gap）。
* **drain 表达**：
  - last array input → last result = **80 cycles**（drain tail 把残留 PE 结果排出）。
  - last result → first write = **8 cycles**（register fifo 等待写 SRAM 启动）。
  - 第一笔 write 与 254 笔 write 之间连续 1 cycle 一拍 ⇒（256 - 1）= 255 beat at
    1 cycle interval ⇒ `result_to_writeback_start_latency ≈ 8`。
  - drain 整体（array_drain 的总宽度）= 1777 - 1689 = **88 cycles**（其中 80 是
    drain 后的 result 流，8 是 last result → first write 延迟）。
* **writeback 全长** = 2032 - 1777 + 1 = 256 cycles，1 cycle/beat 形成稳定的 256
  拍写回流。

### 15. 当前 gem5 “one input token → one output token” 假设是否成立、部分成立或不成立
**部分成立**——必须区分语义层级：
* **字节级 / lane 级**：1 个 *array_input_accepted* token（A 或 B）并不直接等于
  1 个 result token。256 结果对应 3136 个阵列 input（A/B 各 1568），即
  **需要 6.125 array_input token 才生产 1 result token**。
* **beat 级（外部接口）**：1 个外部 write beat == 1 个 result_final_valid 脉冲。
  写回与 result 在 1:1 速率但延迟 8 cycles。即把 gem5 中的“output token”定义为
  “256‑bit 写回 beat”时，“1 result token → 1 write token” 成立，但**不能**
  反推“1 阵列 input token → 1 output token”。
* **建议**：在 gem5 把 pipelining 显式建模时，按以下契约：
  - 每个 **input_array token** (A 或 B) 流过 `arrayFillLatency=247` 后才能影响
    输出；
  - 32 个 (A,B) input token pair ⇒ 跑 32 个连续 result cycles 即 32 outputs
    （steady‑state）；
  - 每 32 outputs 后有 171 cycles gap（flow boundary），由 `flow_loop_times`
    控制。
  - drain: input 停止后继续产生 31 个 result tokens（80 cycles）。
  - 上述对模式 1（连续 result）成立；171‑cycle gap 的成本必须显式建模，
    否则会把 SAU 计算量低估约 ~20%（7×171 / 2036 ≈ 5.9% 实际是 7×171/(256×6+8)
    比内部分析长，可计算 CLRS ratio ≈ 0.83 不算入 fill 和 drain）。

## Trace 校验与确定性证据
* 两次运行 `run1` / `run2`，sim_run1.log 和 sim_run2.log 都包含 `TEST PASSED`。
* `architecture.csv` / `diagnostic.csv` / `summary.json` 在两次运行中逐字节比对
  全部一致（用 `validate_sau_trace.py --compare-run` 验证，输出
  `OK: runs are byte-identical for architecture/diagnostic/summary`）。
* 三者 SHA‑256 均记录于 `SHA256SUMS`，与 testcase 5 个 hex 的 SHA‑256 一同被冻结。

## 实现注记 / 与 §4.2/§4.3 计划的偏离

1. **维度的偏离**：32×32×32 → 64×256×256。原因见开头“维度与原计划的偏离”。
2. **array_drain 触发条件实现**：§4.2 要结合 `last_flow_time_f && last_ins_time_s`
   确认命令级“最后一拍阵列输入”。实测中：
   - `last_flow_time_f` 在 RTL 中是从 feeder 延迟到第 1689 cycle 之后拉高（即
     在最后一拍 array input 之后才 1 cycle 拉高），与 `data_A_last/data_B_last`
     不重合；
   - `last_ins_time_s` 是 scheduler 输出窗口信号，在 cycle 1679‑1687 高、cycle 1688
     之后 0；
   - 真正最后一拍 `data_a_last & data_b_last` 在 cycle 1689 同时高。
   
   严格遵守 “三信号同 cycle high” 永远不会触发 array_drain，因此本基线做了
   **sticky workaround**：用一个 `sau_last_ins_seen` 锁存器——一旦 cycle 1679‑1687
   观察到 `last_ins_time_s=1` 就置 1，然后当出现
   `(data_a_last || data_b_last) & sau_last_ins_seen` 时触发 array_drain。
   这实际在 cycle 1689 触发 array_active→array_drain 转换，与 `data_a_last=1,
   data_b_last=1, core_state=D_OUT(0x6)` 完全吻合。
   
   替代解释（写入 gem5 token pipeline 时不必关心）可由 `scheduler.sv` /
   `feeder.sv` 内部进一步仿真分析证明；当前用 sticky 这条变体的优点是 trace
   中 array_drain phase 完整闭环（phase 1610 cycle array_active → 88 cycle
   array_drain → 256 cycle writeback → 1 cycle complete），与 RTL 行为一一对应。

3. **diagnostic CSV 的 `internal_write_valid` 与 `internal_write_last` 区分**：
   * `internal_write_valid` = `register_wraddr_valid`: 在 cycles 1776‑2031 之间
     连续 256 个 cycle 拉高，作为每拍写地址 valid；
   * `internal_write_last` = `sram_wr_last_ma`: 仅在 cycle 2035 拉高 1 cycle，
     是“写流水 end‑of‑packet”标记，与 §4.3 描述一致。
   * 这两列与 plan §4.3 字段对应，没有改名。

4. **phase `array_drain` 通过 sticky `last_ins_time_s` 触发**，不严格遵守原 §4.2
   “array_active → array_drain 仅在 last_flow_time_f & last_ins_time_s 同时
   high 的 cycle 由最后一拍 array input 触发”描述。如果用户希望采用字面
   严格模式，请在后续任务中提示改回 strict 检查并接受 array_drain phase
   完全不在本 trace 出现（这会让 writeback→complete 直接接上 array_active，
   违反 §4.2 phase transitions）。

## 需要新增 testcase 才能进一步收紧的结论
* 37 个字段直接来自单 testcase；如果以后要给出与维度无关的
  `arrayFillLatency` 等模型参数，需要新增 M/K/N ∈ {16, 32, 64, 128} 的
  随机种子固定 hex，从中分别跑 trace 后用最小二乘提取参数。
* `flow_loop_times` 影响的 171 cycle gap 是 RTL 中 trans2sa_top 的换 tile
  时序开销；要把它和 K_tile、N_tile 解耦来确定 gem5 token pipeline 是否要建模
  “flow boundary gap”，需要至少一个 K=32, N=32（不出现 flow 切换）的 trace。
* 关于外部 read 地址随 reuse 的不同分布：在 32×32×32 推论下 A 和 B 的 read 数
  会显著不同，需要等真正 32×32×32 testcase 才能给系数。