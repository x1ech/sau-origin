# SAU 任务 1：RTL 确定性周期基线与交接计划

> **执行者须知：** 本计划是一个交付契约。执行者不仅要“让仿真跑起来”，
> 还必须交回本文规定的 trace、manifest、summary 和分析结论，后续 agent
> 才能据此实现 trace 比较器并标定 gem5 模型。

## 1. 任务目标

为当前 32×32、256-bit SRAM 接口的 SAU RTL 生成一个可重复的 int8 GEMM
周期基线。该基线用于：

1. 给后续任务 2 提供稳定的架构事件 CSV；
2. 解释 RTL 内部输入、阵列结果和外部写回之间的周期关系；
3. 给 gem5 的 token 粒度、阵列延迟、启动间隔和 drain 延迟提供实测依据；
4. 固定 testcase、RTL 源码状态、仿真器版本和运行命令，确保结果可追溯。

本任务不要求 RTL 寄存器级复刻，也不修改 SAU 功能逻辑。允许的 RTL 侧修改仅限
testbench instrumentation、仿真 Make 目标和 trace 校验工具。

## 2. 执行环境与当前已知事实

本计划由本地 gem5 工作区维护，但 RTL 编译与仿真在远程服务器执行。两个工程的
固定根目录为：

- 远程服务器 RTL 根目录：`/home/xch/work/npu_lpnpu`
- gem5 工程根目录：`/home/xch/workspace/gem5`

除显式使用绝对路径或 `git -C` 的命令外，Task 0～Task 8 中涉及 RTL、testcase、
VCS 和 `sim/vcs/build` 的命令都必须从以下目录执行：

```bash
cd /home/xch/work/npu_lpnpu
```

执行前必须在远程服务器重新核实以下内容，不能用本地 workspace 的检查结果代替：

- 仿真顶层：
  `sim/testbench/tb/top_sau_regress_tb.sv`
- 仿真 Makefile：
  `sim/vcs/script/case_sau_regress/Makefile`
- DUT 层级：`u_dut.u_dut_kui.SAU_1_inst`
- 当前参数来源：`hardware/src/sa_execute/SA_pkg.sv`
- 当前已观察到：
  - `SA_SIZE = 32`
  - `SRAM_DATA_WIDTH = 256`
  - 一个外部 SRAM beat 为 32 bytes

本地编写计划时没有远程服务器的 VCS、许可证和 testcase 状态，因此执行者必须通过
Task 1 和 `timing_trace_preflight` 得到真实结论。禁止因为计划中列出了目标 testcase
名称，就假定对应文件已经存在。

远程 `npu_lpnpu` 工作树可能包含用户既有修改。执行者必须先检查并保存现场，尊重
所有既有修改，不得 reset、checkout、clean 或整体覆盖文件。

## 3. 最终必须交付的结果

### 3.1 代码交付

预计只修改或新增以下文件：

- 修改 `sim/testbench/tb/top_sau_regress_tb.sv`
- 修改 `sim/vcs/script/case_sau_regress/Makefile`
- 新建 `sim/vcs/script/case_sau_regress/validate_sau_trace.py`
- 新建 `sim/vcs/script/case_sau_regress/test_validate_sau_trace.py`
- 必要时修改
  `sim/vcs/script/case_sau_regress/README.md`

除非 instrumentation 无法从 testbench 通过 XMR 访问所需信号，否则不得修改
`hardware/src/`。若确实无法访问，先停止并报告缺失信号，不得自行添加 RTL 功能端口。

### 3.2 基线数据包

成功运行后，必须在以下目录产生一个完整数据包：

```text
sim/vcs/build/sau_regress/baseline/int8_gemm_32x32/
├── architecture.csv
├── diagnostic.csv
├── manifest.json
├── summary.json
├── analysis.md
├── sim_run1.log
├── sim_run2.log
└── SHA256SUMS
```

其中：

- `architecture.csv` 是后续任务 2 唯一直接消费的事件 trace；
- `diagnostic.csv` 用于解释 RTL 内部周期行为，不作为 gem5 公共接口；
- `manifest.json` 固定输入、源码、工具和命令；
- `summary.json` 提供机器可读的计数与延迟；
- `analysis.md` 必须回答 Task 7 的全部 15 个问题；
- 两次运行日志用于证明回归通过；
- `SHA256SUMS` 覆盖上述文件以及 testcase 的输入 hex 文件。

构建目录中的数据包默认不提交到 git。执行者必须在最终交接中给出其绝对路径。
是否复制 `architecture.csv` 到 gem5 reference tree 属于任务 2，不在本任务中执行。

### 3.3 交接状态

最终状态只能是以下两种之一：

- `COMPLETE`：真实 RTL 仿真连续运行两次，两次 trace 逐字节一致，且全部验收通过；
- `BLOCKED`：缺少 testcase、VCS、许可证或其他外部条件。

`instrumentation 已实现但没有实际运行` 仍然是 `BLOCKED`，不得标为完成。

## 4. 固定 trace 契约

### 4.1 采样语义

所有 trace 均在 `clk` 上升沿采样，读取该上升沿进入 active/NBA 更新之前可见的
稳定值。cycle 只在 `rst_n == 1` 时递增。

`command_accepted` 所在周期归一化为 cycle 0。CSV 不得使用仿真绝对时间作为
cycle。若同一周期有多个事件，必须按第 4.2 节规定的顺序输出。

### 4.2 `architecture.csv`

表头必须逐字一致：

```csv
cycle,event,command_id,stream,address,beat,phase
```

允许值：

```text
event:
  phase_changed
  command_accepted
  read_accepted
  read_response_visible
  array_input_accepted
  result_produced
  write_accepted
  command_complete

stream:
  none
  operand_a
  operand_b
  output

phase:
  idle
  operand_load
  array_active
  array_drain
  writeback
  complete
```

单命令基准的 `command_id` 固定为 1。`address` 使用小写十六进制
`0x00000000` 格式；没有地址的事件写 `0x00000000`。`beat` 为相应
`event + stream` 的零基连续编号。

事件触发条件：

| 事件 | RTL 采样条件 | stream/address |
| --- | --- | --- |
| `command_accepted` | `start` | `none` / 0 |
| `read_accepted` | `sau_sram_enable && sau_sram_wstrb == '0` | 根据 `input_switch_s` 映射 A/B；地址为 `sau_sram_addr` |
| `read_response_visible` | `core_register_data_out_valid` | 根据同周期 RTL stream 控制映射 A/B；无地址 |
| `array_input_accepted` | `data_A_valid` 或 `data_B_valid` | A、B 分别独立记一行；同周期都有效时先 B 后 A |
| `result_produced` | `result_final_valid_o` | `output` / 0 |
| `write_accepted` | `sau_sram_enable && sau_sram_wstrb != '0` | `output` / `sau_sram_addr` |
| `command_complete` | `sau_crossbar_done` | `none` / 0 |

不得根据地址范围猜测 A/B。执行者必须阅读 `scheduler.sv`、`feeder.sv` 和实际
波形，写出 `input_switch_s` 到 stream 的映射，并在 `analysis.md` 中给出证据。
如果控制信号与数据 valid 的周期不对齐，应使用 RTL 中与该 valid 对齐的
`input_switch_f` 或延迟版本，并记录选择理由。

抽象 phase 规则：

```text
idle
  -> operand_load  : command_accepted
  -> array_active  : 第一个 array_input_accepted
  -> array_drain   : 最后一个 data_A_last 或 data_B_last 对应的阵列输入；
                     必须结合 last_flow_time_f 和 last_ins_time_s 确认是全命令最后一次
  -> writeback     : 第一个 write_accepted
  -> complete      : command_complete
```

每次 phase 变化先输出一行 `phase_changed`，再输出触发该变化的锚点事件。其余同周期
顺序固定为：

```text
read_accepted
read_response_visible
array_input_accepted(operand_b)
array_input_accepted(operand_a)
result_produced
write_accepted
command_complete
```

如果实测发现 writeback 与 array drain 重叠，不得篡改事件；在 `analysis.md`
记录重叠，并提出 phase 表示方案。变更上述公共 phase 契约前必须由用户确认。

### 4.3 `diagnostic.csv`

表头必须逐字一致：

```csv
cycle,start,core_state,input_switch_s,input_switch_f,read_req,read_addr,read_response_valid,read_response_last,data_a_valid,data_a_last,data_b_valid,data_b_last,result_valid,result_last,internal_write_valid,internal_write_last,write_req,write_addr,command_done
```

从 `command_accepted` cycle 0 开始，每个周期恰好一行，直到并包含
`command_complete`。字段来源：

- `core_state`：输出枚举名称
  `IDLE/FIRST_LOAD/REGISTER_LOAD/TRANSPOSE_LOAD/TRANSPOSE_CLIP/REUSE_LOAD/D_OUT/REGISTER_UNLOAD`
- `read_req`：外部 SRAM read 接受条件
- `internal_write_valid`：`register_wraddr_valid`
- `internal_write_last`：`sram_wr_last_ma`
- `write_req`：外部 SRAM write 接受条件
- 其他字段使用 `SA_CORE.sv` 中同名或语义相同的内部信号

布尔字段只能是 `0` 或 `1`，控制/地址字段不得含 X/Z。若出现 X/Z，校验失败并在
报告中指出首个 cycle 和字段。

### 4.4 `manifest.json`

至少包含以下键，不得以文字报告代替：

```json
{
  "schema_version": 1,
  "case_name": "INT8_SAU_MATMUL_TEST_ID_0",
  "operation": "int8_gemm",
  "matrix": {"m": 32, "k": 32, "n": 32},
  "sa_rows": 32,
  "sa_cols": 32,
  "sram_data_bits": 256,
  "beat_bytes": 32,
  "npu_repo_head": "<git rev-parse HEAD>",
  "npu_worktree_diff_sha256": "<tracked working-tree diff hash>",
  "simulator": "VCS",
  "simulator_version": "<vcs -ID 或等价版本输出>",
  "run_command": "<完整可复制命令>",
  "testcase_dir": "<绝对路径>",
  "testcase_sha256": {
    "instruction.hex": "<sha256>",
    "memory.hex": "<sha256>",
    "memory_mod_0.hex": "<sha256>",
    "memory_mod_1.hex": "<sha256>",
    "memory_mod_2.hex": "<sha256>"
  }
}
```

若 testcase 的矩阵维度不能从现有 metadata 可靠读取，禁止根据目录名猜测。
应从 testcase 生成配置或固件宏中取得并记录证据；无法证明 32×32×32 时，本任务
阻塞。

### 4.5 `summary.json`

至少包含：

```json
{
  "command_cycles": 0,
  "counts": {
    "read_operand_a": 0,
    "read_operand_b": 0,
    "array_operand_a": 0,
    "array_operand_b": 0,
    "results": 0,
    "writes": 0
  },
  "first_cycle": {
    "read": 0,
    "array_input": 0,
    "result": 0,
    "write": 0,
    "complete": 0
  },
  "last_cycle": {
    "read": 0,
    "array_input": 0,
    "result": 0,
    "write": 0
  },
  "latency_cycles": {
    "command_to_first_read": 0,
    "first_array_input_to_first_result": 0,
    "last_array_input_to_last_result": 0,
    "last_result_to_first_write": 0,
    "last_write_to_complete": 0
  },
  "result_intervals": {
    "min": 0,
    "max": 0,
    "mode": 0
  },
  "ratios": {
    "array_a_per_result": "0/0",
    "array_b_per_result": "0/0",
    "results_per_write": "0/0"
  }
}
```

所有 latency 均按“后者 cycle - 前者 cycle”计算。没有对应事件时写 `null`，
禁止写猜测值。

## 5. 执行任务

### Task 0：保护现场并建立证据

1. 阅读：
   - 根目录及上级 `AGENTS.md`；
   - 本计划；
   - `sim/vcs/script/case_sau_regress/README.md`；
   - `top_sau_regress_tb.sv`；
   - `SA_CORE.sv`、`scheduler.sv`、`feeder.sv`、
     `register_file_out.sv`、`SA_pkg.sv`；
   - testcase 生成脚本。
2. 运行并保存：

   ```bash
   git -C /home/xch/work/npu_lpnpu status --short
   git -C /home/xch/work/npu_lpnpu diff -- \
       sim/testbench/tb/top_sau_regress_tb.sv \
       sim/vcs/script/case_sau_regress/Makefile
   ```

3. 确认目标文件中的既有修改。只增加本任务相关小块，不覆盖、回滚或格式化既有代码。
4. 记录 `git rev-parse HEAD`，并对 `git diff --binary HEAD` 的输出计算 SHA-256，
   作为当前 RTL 工作树指纹。

**检查点：** 若无法区分既有改动与本任务改动，停止并向用户报告，不要继续写文件。

### Task 1：取得并冻结单一 testcase

1. 检查：

   ```bash
   test -f testcase/sau_regress_matmul/INT8_SAU_MATMUL_TEST_ID_0/instruction.hex
   test -f testcase/sau_regress_matmul/INT8_SAU_MATMUL_TEST_ID_0/memory.hex
   test -f testcase/sau_regress_matmul/INT8_SAU_MATMUL_TEST_ID_0/memory_mod_0.hex
   test -f testcase/sau_regress_matmul/INT8_SAU_MATMUL_TEST_ID_0/memory_mod_1.hex
   test -f testcase/sau_regress_matmul/INT8_SAU_MATMUL_TEST_ID_0/memory_mod_2.hex
   ```

2. 若不存在，按项目 README 的真实工具链生成一个 32×32×32 int8 GEMM case，
   或让用户提供已生成 case。不得把 `testcase/minsys/test_sau_matmul` 的 mod4
   镜像直接冒充当前 testbench 需要的 mod3 testcase。
3. 生成脚本包含随机输入时，必须固定 NumPy/Python 随机种子，或将最终 hex 文件冻结
   并以 SHA-256 定义 testcase 身份。
4. 不得运行会清空已有目录的生成命令。优先在一个新建的空输出目录生成，再复制单个
   明确文件；如脚本只能批量清理，停止并请求用户处理。
5. 证明 case 的 M/K/N 均为 32，并把证据来源写入 manifest/analysis。

**检查点：** 没有上述五个输入文件或无法证明矩阵维度时，输出 `BLOCKED` 交接，
明确缺少内容和建议生成命令；不得进入 golden 生成阶段。

### Task 2：先实现 trace 校验测试

在修改 testbench 前，创建 `test_validate_sau_trace.py`，使用临时目录和最小伪造
CSV/JSON，至少覆盖：

1. 接受合法的 architecture 与 diagnostic trace；
2. 拒绝错误表头；
3. 拒绝非单调 cycle；
4. 拒绝缺失或重复的 command start/complete；
5. 拒绝非法 event/stream/phase；
6. 拒绝 diagnostic 中的 X/Z；
7. 拒绝非连续 beat 编号；
8. 拒绝 read/write 地址未按 32 bytes 对齐；
9. summary 中的事件计数必须与 architecture.csv 一致；
10. 两次运行的 architecture/diagnostic 必须逐字节一致。

先运行测试并确认因实现缺失而失败：

```bash
python3 -m unittest \
  sim.vcs.script.case_sau_regress.test_validate_sau_trace -v
```

然后用 Python 标准库实现 `validate_sau_trace.py`，不引入第三方依赖。再次运行并确认
全部通过。

CLI 至少支持：

```bash
python3 sim/vcs/script/case_sau_regress/validate_sau_trace.py \
  --architecture <architecture.csv> \
  --diagnostic <diagnostic.csv> \
  --manifest <manifest.json> \
  --summary-out <summary.json>
```

以及：

```bash
python3 sim/vcs/script/case_sau_regress/validate_sau_trace.py \
  --compare-run <run1-dir> <run2-dir>
```

### Task 3：增加 opt-in testbench instrumentation

1. 在 testbench 中增加两个独立 plusarg：

   ```text
   +sau_arch_trace=<path>
   +sau_diag_trace=<path>
   ```

2. 未传入 plusarg 时，不打开文件、不改变原回归行为。
3. 文件打开失败必须 `$fatal`，不能静默禁用 trace。
4. reset 时清空 cycle、phase 和所有 beat counter。
5. 仿真结束时关闭文件。
6. 严格实现第 4 节采样和字段契约。
7. trace monitor 只能观察信号，不能驱动 DUT 或改变时序。
8. 编译前逐一用 `rg` 核实 XMR 信号名；不得照抄旧计划中的不存在信号。

完成后审查 diff，确认没有修改 DUT stimulus、clock/reset、memory model、pass/fail
逻辑或 timeout。

### Task 4：增加确定性 Make 入口

在现有 Makefile 中复用 `BUILD_DIR`、`SIM_FLAG`、`REGRESS_DIR`、`CASE` 和
`TIMEOUT_NS`，增加类似以下接口：

```bash
make -f sim/vcs/script/case_sau_regress/Makefile \
  timing_trace TEST_ID=0
```

目标要求：

1. 依赖已有 `$(BUILD_DIR)/simv`，不要每次无条件全量重编；
2. 输出到固定 baseline 目录；
3. 使用 `INT8_SAU_MATMUL_TEST_ID_0`；
4. 保留完整 sim log；
5. 只有 log 包含 `TEST PASSED` 才成功；
6. 自动调用 trace validator 生成 `summary.json`；
7. 不调用 `clean`，不删除旧构建目录；
8. 支持 `BASELINE_DIR=<path>` 覆盖输出目录，便于两次独立运行。

另加只做环境检查、不运行仿真的目标：

```bash
make -f sim/vcs/script/case_sau_regress/Makefile timing_trace_preflight
```

它必须检查 simulator、testcase 五个输入文件和 Python 校验工具，并对每个缺失项
给出明确错误。它还要报告 simv 是否存在；simv 不存在表示下一步需要编译，不应把
缺少旧 simv 当成外部阻塞。

### Task 5：编译与单次试运行

1. 运行 Python 单元测试。
2. 运行 `timing_trace_preflight`。
3. 如 RTL instrumentation 尚未编入 simv，使用项目已有 `compile_inc` 或 `compile`
   重新构建，不自行拼装 VCS flags。
4. 运行一次到临时目录：

   ```bash
   make -f sim/vcs/script/case_sau_regress/Makefile \
     timing_trace TEST_ID=0 \
     BASELINE_DIR=sim/vcs/build/sau_regress/baseline/run1
   ```

5. 确认：
   - `TEST PASSED`；
   - 恰好一个 command accepted/complete；
   - trace 无 X/Z；
   - cycle 与 beat 单调；
   - 外部 read/write 地址均 32-byte 对齐；
   - manifest 参数为 32×32、256 bit、32 bytes。

若 VCS 命令不存在或许可证不可用，保留已完成的 instrumentation 与单元测试结果，
按 `BLOCKED` 交接，不得改用未经验证的 Verilator/iverilog 结果作为 golden。

### Task 6：重复运行并验证确定性

使用相同 simv、testcase 和参数运行第二次：

```bash
make -f sim/vcs/script/case_sau_regress/Makefile \
  timing_trace TEST_ID=0 \
  BASELINE_DIR=sim/vcs/build/sau_regress/baseline/run2
```

调用 `--compare-run`。至少要求两次的以下内容逐字节一致：

- `architecture.csv`
- `diagnostic.csv`
- `summary.json`

日志中的时间戳或许可证文本可不同，但两次都必须 `TEST PASSED`。将 run1 中的稳定产物
复制为第 3.2 节规定的最终数据包，并生成 `SHA256SUMS`。一次只复制明确文件，不清理
run1/run2 目录。

### Task 7：完成分析报告

根据 trace 写 `analysis.md`，禁止只复述代码。报告必须包含：

1. command 0 到 complete 的总周期数；
2. A/B 外部 read、read response 和阵列 valid 的各自数量；
3. B 是否先完整装载，A/B 是否存在同周期 valid；
4. 第一笔阵列输入到第一笔 result 的实测延迟；
5. result steady-state 间隔：最小、最大、众数；
6. 最后一笔阵列输入到最后一笔 result 的 drain 延迟；
7. result 是否在全部输入结束前已经产生；
8. result 数量与 256-bit 外部 write beat 数量的比例；
9. 一个 `result_final_valid_o` 是否对应一个、半个或多个外部写 beat；
10. 第一笔/最后一笔 result 与第一笔/最后一笔 write 的周期关系；
11. `sram_wr_last_ma` 到 `sau_crossbar_done` 的延迟；
12. `input_switch_s/input_switch_f` 到 operand A/B 的真实映射；
13. 推荐给 gem5 的 token 定义；
14. 推荐的 `arrayFillLatency`、`arrayInitiationInterval` 和 drain 表达方式；
15. 当前 gem5 `one input token -> one output token` 假设是成立、部分成立还是不成立，
    并给出 trace 证据。

第 13～15 项是本任务与后续 gem5 工作衔接的核心输出。无法从单一 testcase 确定的
内容必须标为“尚不能确定”，并列出需要增加的 testcase，不能猜。

### Task 8：最终验证与交接

运行：

```bash
python3 -m unittest \
  sim.vcs.script.case_sau_regress.test_validate_sau_trace -v

python3 sim/vcs/script/case_sau_regress/validate_sau_trace.py \
  --architecture sim/vcs/build/sau_regress/baseline/int8_gemm_32x32/architecture.csv \
  --diagnostic sim/vcs/build/sau_regress/baseline/int8_gemm_32x32/diagnostic.csv \
  --manifest sim/vcs/build/sau_regress/baseline/int8_gemm_32x32/manifest.json \
  --summary-out /tmp/sau-summary-check.json

git diff --check -- \
  sim/testbench/tb/top_sau_regress_tb.sv \
  sim/vcs/script/case_sau_regress/Makefile \
  sim/vcs/script/case_sau_regress/validate_sau_trace.py \
  sim/vcs/script/case_sau_regress/test_validate_sau_trace.py \
  sim/vcs/script/case_sau_regress/README.md
```

对比 `/tmp/sau-summary-check.json` 与交付的 `summary.json`。不得删除 `/tmp` 之外的
任何旧结果。

如果 Task 0 发现目标文件已有既有修改，默认不要执行 `git add` 或 `git commit`。
最终交接提供精确文件列表和 diff；只有用户明确要求，且能确保不夹带既有修改时才
提交。

## 6. 验收标准

只有全部满足才可报告 `COMPLETE`：

- [ ] testcase 五个输入文件存在且 SHA-256 已记录；
- [ ] 有证据证明 testcase 为 int8 GEMM 32×32×32；
- [ ] manifest 固定了 RTL HEAD、工作树 diff 指纹、工具版本和完整命令；
- [ ] 未修改 SAU 功能 RTL；
- [ ] 未传 plusarg 时原回归行为不变；
- [ ] Python 校验测试全部通过；
- [ ] 两次真实 RTL 仿真均 `TEST PASSED`；
- [ ] 两次 architecture/diagnostic/summary 逐字节一致；
- [ ] architecture.csv 严格符合七列公共 schema；
- [ ] diagnostic.csv 覆盖 command 的每个 cycle；
- [ ] trace 无 X/Z，cycle/beat 单调，地址按 32 bytes 对齐；
- [ ] analysis.md 回答 Task 7 的全部 15 个问题；
- [ ] 明确给出 gem5 token 粒度与时序参数建议；
- [ ] 交接中列出所有修改、命令、结果、未验证项和风险。

## 7. 执行者最终回复模板

执行者必须按以下格式回复，不要只说“已完成”：

```md
## Status
- COMPLETE / BLOCKED
- 阻塞原因：（COMPLETE 时写“无”）

## Delivered Artifacts
- Code:
  - `<绝对路径>`：用途
- Baseline bundle:
  - `<绝对路径>`
- Testcase SHA-256:
  - `instruction.hex`: ...
  - ...

## Key RTL Findings
- SA dimensions / SRAM beat:
- A/B 输入顺序和数量:
- first input -> first result:
- result initiation interval:
- drain latency:
- result count / write beat count:
- 推荐 gem5 token 定义:
- 当前 one-in/one-out 假设结论:

## Verification
- `<完整命令>`：PASS/FAIL
- run1 trace SHA-256:
- run2 trace SHA-256:
- 两次是否逐字节一致:

## Changed Files
- `<路径>`：修改内容

## Preserved Pre-existing Changes
- `<路径>`：执行前已存在、未由本任务引入的修改

## Unverified / Risks
- ...

## Handoff to Next Agent
- 任务 2 应直接消费：`<architecture.csv 绝对路径>`
- manifest：`<manifest.json 绝对路径>`
- token/时序结论：`<analysis.md 绝对路径>`
```

## 8. 明确禁止事项

- 不得伪造或手写 golden trace；
- 不得用 `testcase/minsys` 的不兼容镜像冒充 sau_regress testcase；
- 不得在缺少 VCS 时把其他 simulator 的未验证结果标为正式基线；
- 不得删除或跳过 failing test；
- 不得通过放宽校验来接受 X/Z、事件缺失或非确定结果；
- 不得 reset、clean、checkout 或覆盖用户既有修改；
- 不得批量删除 build/testcase 目录；
- 不得修改 gem5 token pipeline 来迁就尚未证实的 RTL 假设；
- 不得在任务 1 中提前实现任务 2 的 gem5 trace comparator。

## 9. 下一 agent 的消费边界

任务 2 agent 只应依赖三个稳定输出：

1. `architecture.csv`：公共七列 trace；
2. `manifest.json`：基线身份与 32-byte beat 契约；
3. `analysis.md`：token 粒度和时序解释。

`diagnostic.csv` 是核查证据，不应成为 gem5 公共 trace schema。若任务 1 未达到
`COMPLETE`，任务 2 可以实现比较器和单元测试，但不得宣称已完成 RTL/gem5 周期对齐。
