# SAU RTL Timing Provenance

Last audited: 2026-07-15

## Baseline and scope

Step 5.5 uses the current files under `/home/xch/workspace/npu_lpnpu` as the
authoritative RTL baseline.  The RTL repository's commit state is deliberately
not used as a gate; the hashes below identify the exact source snapshot used by
the audit:

| File | SHA256 |
| --- | --- |
| `hardware/src/sa_element/SA_CORE.sv` | `9db76e1e08554fc51b52d41907dc3db9cc00b17f76ef9be43064fcc5ae23d9ca` |
| `hardware/src/sa_element/scheduler.sv` | `631df2bab359b94cfb71839b099fecda02e5001f458482b3b32b14df0bbdcbe6` |
| `hardware/src/sa_element/mem_addr.sv` | `5a91bfbcd07e14237e04cf788861142ee22c849a661d2e4c6b5bd8154393e31c` |
| `hardware/src/sa_element/mem_ctrl.sv` | `05f02abe3e0a3ebaa5b30d6ee71b3056515cb1102fff42055f1e7a0765024e51` |
| `hardware/src/sa_element/register_addr.sv` | `d1ff1c3e69e4c687aeb9ff8fae62e714d49c062bdf8e5378ef6b13325e462c24` |
| `hardware/src/sa_element/register_file_in.sv` | `19ad5f271a1cedbe5eba188f616fc7b0ca9b86c7973e53deed3c33aa6cc8a8a1` |
| `hardware/src/sa_element/feeder.sv` | `bb58a9c3acd0bd50c7a1e400e22e90e02fb55db28516106143265e9d000fc072` |
| `hardware/src/sa_execute/sa_feeder.sv` | `cd909205e01bdb06e0684e7a7fbdfe88e81be175f0c65ea7f28ffa95efdd2478` |
| `hardware/src/sa_execute/SA_ENGINE.sv` | `41ee498c4338cc27b0e3f9d1cafbb63700280fe29928c733f953eba2e18b8ac5` |
| `hardware/src/sa_element/register_file_out.sv` | `aa1dcc2983e3ad468c08d35d89eb1c4b2a9b2b129bc6d73c7824276701a6cc9f` |

The first implementation target remains the control contract already accepted
by the gem5 decoder: INT8 matmul, 32x32 SA, ATB/reuse-A, non-shift and
non-keep output.  Other RTL modes must not be inferred from this audit.

The RTL owner confirmed that the eight imported fixture packages were
captured from this latest RTL line. Their manifest commit/diff fields describe
the capture worktrees rather than an obsolete functional baseline, so their
`diagnostic.csv` files remain valid current-RTL boundary observations.

## Proven structural parameters

| gem5 concept | RTL source | Current value | Status |
| --- | --- | ---: | --- |
| SA rows/columns | `SA_pkg::SA_SIZE`; `SA_CORE.ROW_NUM/COL_NUM` | 32 | Source proven |
| external SRAM beat | `SA_pkg::SRAM_DATA_WIDTH`; `SA_CORE.SRAM_DATA_WIDTH` | 256 bits | Source proven |
| external SRAM delay | `SA_CORE.SRAM_DELAY` | 3 | Source proven |
| memory address delay | `SA_CORE.ADDR_DELAY` | 2 | Source proven |
| visible read-valid chain | `mem_ctrl.STATE_DELAY = SRAM_DELAY + 1` | 4 registers | Source proven |
| feeder control chain | `feeder.STATE_DELAY = SRAM_DELAY + ADDR_DELAY + MEMCTRL_DELAY` | 7 registers | Source proven |
| feeder output registers | `feeder.REGISTER_DELAY`, final output FF | 2 + 1 | Source proven |
| input-RF state/pad chain | `register_file_in.PAD_DELAY = 2 + SRAM_DELAY + 1 + 1` | 7 registers | Source proven |
| PE calculation pipeline | `SA_ENGINE.CALC_DELAY`; 4x4 PE macro parameters | 3; 4x4 | Source proven |
| macro rows/columns | `ROW_NUM/PE_ROW_NUM`, `COL_NUM/PE_COL_NUM` | 8x8 | Source proven |
| output write address/data chain | `register_file_out` valid/last d1,d2 | 2 registers | Source proven |
| write-finished chain | `register_file_out.sram_wr_pipe_done_d1..d4` | 4 registers | Source proven |

These values may become named elaboration inputs, counter widths, or delay
queue depths.  They are not permission to precompute a workload-specific end
cycle.

## Event provenance table

| Stage | RTL start | RTL progress/guard | Source-audit result | Implementation state |
| --- | --- | --- | --- | --- |
| CSR start | `csr` accepts register 6 write and raises `start_reg` | `scheduler.ins_valid` is a registered copy; `mem_addr` also captures `start` | Register edges identified | Not implemented per tick |
| resident address/read | `register_addr.load_ins_valid_i = start` | x/y/c counters; registered `register_rdaddr_valid/last`; `mem_ctrl` read-valid/last chain | Counters and delay chain identified | Resident counter/last implemented; mem_ctrl visibility pending |
| resident RF fill | visible memory response in `REGISTER_LOAD` | `register_file_in.PAD_DELAY`, padding shifter, internal SRAM label/write | Delay chain identified; padding shifter latency still needs supported-mode edge check | Not implemented per tick |
| transpose setup | scheduler enters `TRANSPOSE_LOAD` | `transload_state_cnt == SA_SIZE-1` or `data_last` | Counter/guard identified | `RtlSchedulerSkeleton` implemented; runtime integration pending |
| streamed address/read | `vertical_cnt_start` in `mem_addr.WAIT_TRIG` | x/y/f/i counters and `vertical_cnt_last` | Counter logic identified | Address control and `load_done` implemented for validated x=1/y=32 matmul shape; mem_ctrl visibility pending |
| scheduler flow/ins | `data_last = load_done_flag` | `flow_times_cnt_clear`; `ins_times_cnt_clear`; `REUSE_LOAD/FIRST_LOAD/TRANSPOSE_CLIP` guards | Counter/guard identified | `RtlSchedulerSkeleton` implemented; runtime integration pending |
| feeder A/B | visible read plus delayed scheduler state/switch | state/switch delay array, input-RF counter, shift counter, two output stages and final FF | Register/counter chain identified | Existing array-input scheduler approximates cadence |
| SA execute | `sa_en_i` | `calc_cnt == get_exe_cycle(kernel)*flow_loop_times-1`; 8 macro-row finish stages; 8 macro-column finish stages | Counter and structural dimensions identified | Enable-driven counter, first-row execute pulse, and update register implemented in isolation; feeder enable/result stream pending |
| result stream | `storage_ready`/`sa_serial_o_flag` | SA_ROW token chain, 32-row output, output transposer valid/last, final result FF | State/counter chain identified; ping-pong interaction requires edge trace | Existing model uses result fill/gap formulas |
| D_OUT exit | scheduler is in `D_OUT` | current RTL `D_OUT_cond` is driven by `update_finished`, latched `update_finished_q`, and mode/final-instruction guards | Source semantics differ from Step 5 assumptions | Per-tick guard implemented; result pulse source pending |
| output RF accumulate | `result_final_valid_o` | internal x/y/flow/ins counters; `result_accum_done` latch; accumulator SRAM write pipeline | Counters and guard identified | Existing output FIFO is not this control skeleton |
| unload/writeback | `REGISTER_UNLOAD && result_accum_done` | output `register_addr`; two-stage valid/data; external one-beat writes | Counter/delay chain identified | Existing model waits on aggregate writeback-start delay |
| command finish | last output address | four-stage `sram_wr_pipe_done`; scheduler sees `write_finished` and raises `flow_end_o` | Delay/guard identified | Scheduler guard implemented; output delay chain pending |

## Phase B component boundary

`RtlSchedulerSkeleton` is implemented in the existing
`schedule_state.{hh,cc}` target so this first increment does not add a build
target or alter `SauModel` behavior. Its inputs are only the signals present at
the RTL scheduler boundary: CSR start write, memory `load_done`, resident
`register_load_done`, `update_finished`, `write_finished`, and the feeder's
last-flow clear. It owns the CSR start register, scheduler instruction-valid
register, core/instruction states, transpose/flow/instruction counters,
`update_finished_q`, input switch, and completion pulse.

The component accepts no matrix dimension, fixture name, golden cycle, or
precomputed completion time. Unit tests cover the two-edge CSR-to-scheduler
start path, the 32-edge transpose counter, the flow/instruction clear guards,
the registered update pulse, and write-finished completion. It is not wired
into runtime until the memory-last and result/update producers are implemented.

`RtlResidentLoadSkeleton` is the first producer. It copies only
`register_addr.sv`'s x/y/channel loop and registered valid/last outputs. The
baseline CSR extent `8 * 32 * 1` naturally produces 256 request cycles and
drives scheduler `REGISTER_LOAD -> TRANSPOSE_LOAD` at command-relative cycle
258, matching the captured 28914-to-29172 edge without a resident-load delay
constant.

`RtlStreamLoadSkeleton` is the second producer. It copies `mem_addr.sv`'s
`WAIT_TRIG/RUNNING/DONE` state, x/y/flow/instruction counters, registered
vertical read valid/last, one-edge `rdaddr_last_d[1]`, one-edge
`last_flow_time_d[1]`, and the nine-edge fallback counter used by the
combinational `load_done_flag`. The first supported contract is deliberately
limited to `x_burst=1, y_cycle=32`, which is the common vertical shape in all
eight current CSR snapshots; flow and instruction extents remain independent
CSR-driven counters. Focused tests require one 32-read burst per `load_done`
and a 33-edge retrigger cadence between consecutive flows. This component is
also isolated from `SauModel`. Its focused tests passed in the
developer-built 2026-07-15 binary. A coupled test feeds the resident
and streamed `load_done` pulses to the scheduler from one pre-edge snapshot
and checks the baseline's source-derived first-instruction boundaries through
`D_OUT`. All 14 schedule-state tests pass, closing this producer before
result/update work.

`RtlExecuteUpdateSkeleton` is the first result-side producer increment. For
the supported normal-int8/non-retain mode it copies `SA_ENGINE.calc_cnt`,
`internal_finish_pulse`, `delay_finish_flag[0]`, the first macro row's
`acc_finish_flag_d1/PE_valid_o`, and `sa_feeder.update_state/update_finished`.
The calculation counter advances only on actual `sa_en_i` edges; idle bubbles
do not become an elapsed-cycle formula. A non-final instruction registers
`execute_done_flag_o` into `update_finished`, while a final instruction waits
for registered `result_last_o`. It is intentionally not yet coupled to the
feeder enable or result serializer. Its three focused tests pass in the
developer-built 2026-07-15 binary, bringing the schedule-state target to
17/17 before that boundary is expanded.

### Unresolved feeder-to-execute observation boundary

The exported diagnostic fields are not sufficient to reconstruct
`SA_ENGINE.sa_en_i` without an assumption. For baseline command 1, treating
`sa_en_i` as one-hot `input_switch_f` combined with the previous cycle's
`data_A_valid || data_B_valid` gives only 246 accepted enable edges when the
scheduler first enters `D_OUT`, 247 when it exits, and the 256th edge nine
cycles later. The source counter limit is unambiguously
`get_exe_cycle(0) * flow_loop_times = 32 * 8 = 256`; therefore the difference
must not be encoded as a ten-cycle correction. The single-flow exports show a
similar ten-edge lead into `D_OUT`, but their exit behavior differs because
result/update serialization is active.

Before coupling `RtlExecuteUpdateSkeleton`, one targeted internal trace is
needed for a single existing baseline command. Required signals are:

```text
u_trans2sa_top.input_switch_i
u_trans2sa_top.EN_i u_trans2sa_top.EN_i_d u_trans2sa_top.sa_en_i
u_trans2sa_top.u_SA_TOP.calc_cnt
u_trans2sa_top.u_SA_TOP.internal_finish_pulse
u_trans2sa_top.u_SA_TOP.delay_finish_flag[0]
u_trans2sa_top.u_SA_TOP.PE_valid_out[0]
u_trans2sa_top.execute_done_flag_o
u_trans2sa_top.update_finished_flag
u_trans2sa_top.update_finished
scheduler_inst.update_finished_q scheduler_inst.D_OUT_cond
```

Only the first streamed instruction through its first `D_OUT` exit is needed.
This is a signal-origin check, not a new calibration workload.

## Step 5 formulas that must become signal-driven

The eight golden packages remain the strict acceptance oracle, but keeping
them green is not sufficient reason to retain aggregate timing formulas:

1. `scheduler.D_OUT_cond_A` now depends on `update_finished` and the latched
   `update_finished_q`.  The prior `flow_times_i == 1` short-D_OUT rule and
   the XOR-based early-unload rule are not the current source semantics.
2. `REGISTER_UNLOAD` is additionally gated by `result_accum_done`, and
   `write_finished` is four explicit registers after the last two-stage output
   address/data pipeline.  The old writeback/completion sums do not express
   this control path.
## Boundary edges in the captured waveform export

The packages do not contain the original FSDB, but their diagnostic exports
retain the relevant top-level edges:

| Case/command | Observed signal path and edge |
| --- | --- |
| baseline command 1 | `start=1` at 28914; `IDLE->REGISTER_LOAD` at 28916; final `result_last` at 31419; `D_OUT->REGISTER_UNLOAD` at 31421; writes 31427..31682; `command_done` at 31686 |
| K sweep command 1 | `REGISTER_UNLOAD` at 29512 precedes `result_last` at 29578; writes wait until 29585..29840; done at 29844 |
| N sweep command 1 | `REGISTER_UNLOAD` at 29469 precedes the first result at 29526 and final result at 29557; writes wait until 29564..29595; done at 29599 |
| single-flow command 1 | final `result_last` at 29300; `REGISTER_UNLOAD` at 29302; writes 29308..29339; done at 29343 |

These observations prove that entering `REGISTER_UNLOAD` and starting output
RF reads are distinct events. Early scheduler unload can overlap a pending
result stream, while `result_accum_done` holds actual writeback. The last
accepted write to command-done interval is four cycles in every captured
shape, matching `sram_wr_pipe_done_d1..d4`.

Three internal relations are not exported directly. They must be reproduced
from the source guards and checked against the boundary edges above:

1. The exact edge at which `mem_addr.rdaddr_last_flag` pulses relative to
   `vertical_cnt_last`, `sram_rdaddr_last`, and `mem_ctrl` visible last.  The
   RTL expression is marked `fixme` and includes the nine-cycle fallback
   counter.
2. The first and last result edges through `SA_ENGINE` macro-row tokens,
   `sa_serial_o_flag`, output transposer 2, `result_last_o`, and the registered
   `update_finished` pulse.
3. The edge relationship between final `result_final_valid_o`,
   `result_accum_done`, entry to `REGISTER_UNLOAD`, first/last external write,
   `sram_wr_last_o`, and scheduler `flow_end_o`.

No new RTL run is required before per-tick implementation. If the original
FSDB becomes available, `npi_fsdb_probe` should additionally inspect these
internal signal suffixes rather than generating a new calibration shape:

```text
start core_state_s input_switch_s input_switch_f load_done_flag
register_rdaddr_valid register_rdaddr_last
sram_rd_enable sram_rdaddr_last core_register_data_out_valid
core_register_data_out_last
scheduler_inst.transload_state_cnt
scheduler_inst.flow_times_cnt scheduler_inst.flow_times_cnt_clear
scheduler_inst.ins_times_cnt scheduler_inst.ins_times_cnt_clear
scheduler_inst.data_last scheduler_inst.D_OUT_cond
scheduler_inst.update_finished_q
data_A_valid data_B_valid
u_trans2sa_top.cur_state u_trans2sa_top.EN_i u_trans2sa_top.sa_en_i
u_trans2sa_top.u_SA_TOP.calc_cnt
u_trans2sa_top.u_SA_TOP.internal_finish_pulse
execute_finished storage_ready_o result_final_valid_o result_last_o
update_finished
u_register_file_out.result_accum_done
u_register_file_out.register_out_state
register_wraddr_valid core_register_data_in_last
sram_wr_last_ma flow_end
```

All waveform conclusions must record the command-relative positive-edge index,
the observed signal transition, and the source guard that caused it.  Golden
cycle totals alone are insufficient.
