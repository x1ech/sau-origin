# SAU PLAN2：Int8 GEMM RTL Golden Package 采集交付说明

本文交付给具备 VCS 的工作站 agent。目标是在 RTL 仿真侧生成可供 gem5
PLAN2 使用的、可复现的 Int8 GEMM golden package；**不得伪造 CSV、snapshot
或时序数据**。若 VCS、testcase 或设计配置不可用，请交付执行命令、完整错误和
已确认的阻塞原因。

## 1. 固定建模范围

本阶段只采集真实 matmul 所用的控制路径：

```text
precision   = int8
operation   = GEMM / matmul
trans_mode  = 2'b01   // ATB
reuse_mode  = 2'b01   // reuse Operand-A
```

不要为其他 `trans_mode` 或 `reuse_mode` 组合生成伪造或推断的 package。它们不在
本阶段范围内；若 RTL testcase 自然产生其他组合，请在交付说明中记录 raw 值，
但不要把它标为本阶段 supported fixture。

## 2. 必须生成的八组 GEMM 测试

每组必须使用真实 CSR 写入启动 SAU，不能通过直接 force 内部状态或修改测试台
绕开 CSR 配置。

| Fixture 名称 | 矩阵尺寸（M × K × N） | 命令/flow 要求 | 用途 |
| --- | --- | --- | --- |
| `int8_gemm_32x32x32_single_flow` | `32 × 32 × 32` | 1 个 command；`flow_loop_times=1` | 最小路径与 CSR/decode 验证 |
| `int8_gemm_32x256x256_m_sweep` | `32 × 256 × 256` | 与 baseline 保持 K/N 和控制模式；记录真实 command/flow | 隔离 M 维度变化 |
| `int8_gemm_64x32x256_k_sweep` | `64 × 32 × 256` | 与 baseline 保持 M/N 和控制模式；记录真实 command/flow | 隔离 K 维度变化 |
| `int8_gemm_64x256x32_n_sweep` | `64 × 256 × 32` | 与 baseline 保持 M/K 和控制模式；记录真实 command/flow | 隔离 N 维度变化 |
| `int8_gemm_64x256x256_baseline` | `64 × 256 × 256` | 使用真实 `INT8_SAU_MATMUL_TEST_ID_0`；保留其实际的双 command 与 `flow_loop_times=8` | 多 flow、完整时序与 legacy 基线衔接 |
| `int8_gemm_96x256x256_m_holdout` | `96 × 256 × 256` | 与 baseline 保持 K/N 和控制模式；记录真实 command/flow | 公式冻结后的 M 维度 hold-out |
| `int8_gemm_64x128x256_k_holdout` | `64 × 128 × 256` | 与 baseline 保持 M/N 和控制模式；记录真实 command/flow | 公式冻结后的 K 维度 hold-out |
| `int8_gemm_64x256x128_n_holdout` | `64 × 256 × 128` | 与 baseline 保持 M/K 和控制模式；记录真实 command/flow | 公式冻结后的 N 维度 hold-out |

baseline fixture 是当前 gem5 已导入 trace 的真实基线；请不要改成 32×32×32 后仍命名为
baseline。每个 sweep 只改变名称所示的一维，其他两维应与 baseline 相同。第一组
若当前 RTL/firmware 确实不能产生 `32 × 32 × 32` 且 `flow_loop_times=1`，或任一
sweep/hold-out 无法被真实接受，不得自行替换尺寸：请说明限制，并提供实际的
`M/K/N`、flow 值和拒绝原因，等待范围确认后再替换。前三个 hold-out 仅用于
gem5 公式冻结后的独立验证，交付时必须在 `manifest.json` 中标记
`fixture_role: "holdout"`；它们不能被用于为单个尺寸补 timing 参数。

## 3. 每个 package 的目录与文件

建议目录：

```text
<output-root>/
  int8_gemm_32x32x32_single_flow/
  int8_gemm_32x256x256_m_sweep/
  int8_gemm_64x32x256_k_sweep/
  int8_gemm_64x256x32_n_sweep/
  int8_gemm_64x256x256_baseline/
  int8_gemm_96x256x256_m_holdout/
  int8_gemm_64x128x256_k_holdout/
  int8_gemm_64x256x128_n_holdout/
```

每个目录必须包含：

```text
csr_writes.csv
csr_snapshot.json
architecture.csv
diagnostic.csv
manifest.json
SHA256SUMS
README.md
```

除以上文件外，应保留 testcase/firmware、memory image、仿真日志及必要的 TB/Makefile
source diff；这些大文件可随 package 一起压缩，或在 `manifest.json` 中列出取得路径
和 SHA-256。

## 4. `csr_writes.csv`：原始 CSR 写入轨迹

表头必须完全为：

```csv
cycle,csr_addr,csr_operation,csr_wdata,accepted
```

要求：

- 只记录写往 SAU CSR 的 transaction，按实际接受顺序输出；保留 raw address、
  2-bit operation 和完整 64-bit wdata，使用固定十六进制格式。
- `cycle` 使用与 `architecture.csv`、`diagnostic.csv` 相同的 RTL 时钟和同一零点。
- `accepted` 必须是该 transaction 的真实握手结果。若 `csr_ready` 相对写入有
  寄存器延迟，testbench 必须关联到原 transaction；不能简单采样同周期信号。
- 同周期多笔写入必须按 testbench 观察顺序稳定排序；在 README 说明该排序规则。
- 必须包含配置写与导致 command 开始的实际 start 写；不能只导出最终字段。

在当前 RTL 中，配置的 register index 来自 `csr_addr[3:1]`，`trans_mode` 和
`reuse_mode` 位于 index `3'h3` 的 wdata `[1:0]` 与 `[5:4]`。请保留 raw 写入，
不要在 CSV 中只输出已解码的字段。

## 5. `csr_snapshot.json`：每个 command 的 start 快照

每个真实 start command 一个对象，至少包含：

```json
{
  "command_id": 1,
  "start_write_cycle": 0,
  "start_write_row": 0,
  "trans_mode": "0x1",
  "reuse_mode": "0x1",
  "vertical_address": "0x00000000",
  "horizontal_address": "0x00000000",
  "output_address": "0x00000000",
  "bias_address": "0x00000000",
  "flow_loop_times": 1,
  "pe_work_mode": "0x0",
  "sa_flow_mode": "0x0",
  "register_mode": "0x0",
  "conv_kernal": "0x0",
  "stride_flag": 0,
  "shift_flag": 0,
  "cutbit": 0,
  "input": { "x_step": 0, "x_burst": 0, "y_step": 0, "y_burst": 0,
             "flow_step": 0, "flow_burst": 0, "ins_step": 0, "ins_burst": 0 },
  "vertical": { "x_step": 0, "x_burst": 0, "y_step": 0, "y_cycle": 0,
                "flow_step": 0, "flow_cycle": 0, "ins_step": 0, "ins_cycle": 0 },
  "register_input": { "x_burst": 0, "y_step": 0, "y_cycle": 0,
                        "c_step": 0, "c_cycle": 0,
                        "valid_y_start": 0, "valid_y_end": 0,
                        "valid_x_start": 0, "valid_x_end": 0 },
  "output": { "x_step": 0, "x_burst": 0, "y_step": 0, "y_burst": 0,
              "flow_step": 0, "flow_burst": 0, "ins_step": 0, "ins_burst": 0,
              "register_x_burst": 0, "register_y_step": 0,
              "register_y_cycle": 0, "register_c_step": 0,
              "register_c_cycle": 0 }
}
```

数值仅为 schema 示例，必须替换为真实值。`start_write_cycle` 的语义是“该 start
write 已被 RTL 接受、且本 command 取用该配置”的时钟边沿；README 必须说明其与
`architecture.csv` 中 `command_accepted` 的关系。

## 6. `architecture.csv`：公开架构事件轨迹

表头固定，不得增删列：

```csv
cycle,event,command_id,stream,address,beat,phase
```

事件至少包含：

```text
phase_changed
command_accepted
read_accepted
read_response_visible
array_input_accepted
result_produced
write_accepted
command_complete
```

使用既有 Task-1 事件命名、stream 命名和 phase 映射。`command_id` 从 1 起，按
实际 command 接受顺序分配；baseline 的两个 command 都必须保留。地址和 beat
必须来自真实 RTL 可观察接口，不能从矩阵尺寸在后处理中推算。

## 7. `diagnostic.csv`：状态与 guard 诊断

保留既有诊断字段，并新增或能够无歧义推导 `command_id`。建议表头为：

```csv
cycle,command_id,start,core_state,input_switch_s,input_switch_f,
read_req,read_addr,read_response_valid,read_response_last,
data_a_valid,data_a_last,data_b_valid,data_b_last,
result_valid,result_last,internal_write_valid,internal_write_last,
write_req,write_addr,command_done
```

采集从每个 command 的首个相关 CSR 写入开始，到 `command_done` 后至少两个 SAU
时钟周期结束。`core_state` 保留 RTL 名称（如 `REGISTER_LOAD`、`TRANSPOSE_LOAD`、
`REUSE_LOAD`、`D_OUT`、`REGISTER_UNLOAD`），`input_switch_s/f` 保留原始 2-bit
表示。该文件用于后续生成 gem5 semantic-state trace，不要求在此阶段输出 gem5
状态名。

## 8. `manifest.json` 与可复现性

每个 manifest 至少记录：

- fixture 名称、`fixture_role`（`coverage` 或 `holdout`）、`M/K/N`、precision、
  case/test ID、实际 command 数、实际 `flow_loop_times`；sweep/hold-out fixture
  还必须注明相对于 baseline 的唯一变化维度；
- `trans_mode=01`、`reuse_mode=01`，以及 snapshot 对应 command ID；
- RTL commit、工作树 diff SHA-256、VCS 版本、仿真命令；
- 所有 elaboration 参数：至少 `SA_SIZE`/`ROW_NUM`/`COL_NUM`、`REGDEPTH`、
  `SRAM_DELAY`、`ADDR_DELAY`、SRAM data width、clock period，以及影响配置的
  `define`（特别是 `DEBUGCASE`）；
- reset 长度、trace cycle 0 的定义、CSR transaction 的 accepted/排序语义；
- testcase、firmware、memory image、各 CSV/JSON 的 SHA-256。

`SHA256SUMS` 必须能在 package 根目录直接用 `sha256sum -c SHA256SUMS` 验证。

## 9. 工作站侧验收

对每个 fixture：

1. VCS 日志显示 testcase 通过，且未 timeout。
2. `csr_writes.csv` 非空，存在至少一个 accepted start 写；每个 start 都有一个
   `csr_snapshot.json` 对象。
3. 每个 snapshot 都满足 `trans_mode=01`、`reuse_mode=01`；否则该 package 不可
   标为 PLAN2 supported。
4. `architecture.csv` 中 `command_accepted` 与 `command_complete` 数量相等；
   baseline 必须为两个 command。
5. `diagnostic.csv` 覆盖所有 active command 周期，能定位 core state、
   input switch、读请求/响应、array 输入、结果和写回。
6. 全部文件通过 `SHA256SUMS` 校验，README 写明实际运行命令和结果。

## 10. 交付方式

交付八个完整目录及其压缩包，并附简短交接说明：实际 testcase 路径、VCS 命令、
提交/工作树版本、验证结果、任何与本文尺寸或模式要求不同之处。不要修改 gem5
模型来适配 RTL 输出；gem5 侧会在收到 package 后实现 CSR replay、decode、时序
ledger 与 strict/causal 对齐。
