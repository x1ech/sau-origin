# SAU RTL Timing Provenance

Last audited: 2026-07-22

## Baseline and scope

Step 5.5 uses the current files under `/home/xch/work/npu_lpnpu` together with
the passing `sim/vcs/build/yinglong/simv` waveform as its authoritative RTL
baseline. The previously audited `/home/xch/workspace/npu_lpnpu` snapshot and
all eight traces attributed to it are retired from golden/oracle status. The
hashes below identify the source snapshot required by the next RTL build and
trace package:

| File | SHA256 |
| --- | --- |
| `hardware/src/sa_element/SA_CORE.sv` | `c8f7f637070c68256c53c860822287ea623794826775f19c29375b28fcc4b5f1` |
| `hardware/src/sa_element/scheduler.sv` | `6a4bc35b7541fd5c9e61395f5acfdbe5d2618f58b94a779d7d9b40a4dee2416d` |
| `hardware/src/sa_element/mem_addr.sv` | `5a91bfbcd07e14237e04cf788861142ee22c849a661d2e4c6b5bd8154393e31c` |
| `hardware/src/sa_element/mem_ctrl.sv` | `05f02abe3e0a3ebaa5b30d6ee71b3056515cb1102fff42055f1e7a0765024e51` |
| `hardware/src/sa_element/register_addr.sv` | `d1ff1c3e69e4c687aeb9ff8fae62e714d49c062bdf8e5378ef6b13325e462c24` |
| `hardware/src/sa_element/register_file_in.sv` | `19ad5f271a1cedbe5eba188f616fc7b0ca9b86c7973e53deed3c33aa6cc8a8a1` |
| `hardware/src/sa_element/padding_shifter.sv` | `a1a3f2bd0614a78556418aa33fcf4a02e4b3f7da1c18b8e70bbfd1544177244c` |
| `hardware/src/sa_element/feeder.sv` | `bb58a9c3acd0bd50c7a1e400e22e90e02fb55db28516106143265e9d000fc072` |
| `hardware/src/sa_execute/sa_feeder.sv` | `851007b4b00e4b8cdea125a4f37103254abe3a34afc6cc4a8bce06e10144f4c1` |
| `hardware/src/sa_execute/SA_ENGINE.sv` | `00c99dd375f7b99f5f5b89578d9e2bce94dcb5a1285b37a23c2c7069e5397076` |
| `hardware/src/sa_element/register_file_out.sv` | `1a3e1cd65d2ea2cb103e323fb6b570eceaf8ca3741f6619eca62f72fba14964a` |
| `hardware/src/crossbar/crossbar_mi.sv` | `6f18d616836ad1bef4984287b1021b5a910d61a9e199f08b0180db8a6b26d719` |
| `hardware/src/memory/shared_memory_axi.sv` | `9da47926d0d1b9b097c466815cb7b6d67cae0a64c32558eeadca3c2eaa1c8330` |
| `hardware/ip/tcdm/tcdm_bank.sv` | `0561d3182c7cb072009c0e29ba49bb75a3ad9bd2bdd2f36169bf95105e67f434` |

The first implementation target remains the control contract already accepted
by the gem5 decoder: INT8 matmul, 32x32 SA, ATB/reuse-A, non-shift and
non-keep output.  Other RTL modes must not be inferred from this audit.

The eight imported fixture packages are historical diagnostics only. They
must not be used as golden timing, strict acceptance, calibration input, or a
completion gate for this baseline. New packages require these source hashes
and an explicit simulator/build identity.

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
| resident address/read | `register_addr.load_ins_valid_i = start` | x/y/c counters; registered `register_rdaddr_valid/last`; `mem_ctrl` read-valid/last chain | 2..257 request and 7..262 visible-data windows identified | Resident counter and mem_ctrl visibility implemented in isolation; runtime integration pending |
| resident RF fill | visible memory response in `REGISTER_LOAD` | feeder write-valid FF; delayed core state; padding-shifter valid FF; internal SRAM write | 8..263 feeder and 9..264 SRAM-write windows identified | `RtlResidentFillSkeleton` implemented; 27 focused tests passed |
| transpose setup | scheduler enters `TRANSPOSE_LOAD` | `transload_state_cnt == SA_SIZE-1` or `data_last` | Counter/guard identified | `RtlSchedulerSkeleton` implemented; runtime integration pending |
| streamed address/read | `vertical_cnt_start` in `mem_addr.WAIT_TRIG` | x/y/f/i counters and `vertical_cnt_last` | Counter logic identified | Address control and `load_done` implemented for validated x=1/y=32 matmul shape; mem_ctrl visibility pending |
| scheduler flow/ins | `data_last = load_done_flag` | `flow_times_cnt_clear`; `ins_times_cnt_clear`; `REUSE_LOAD/FIRST_LOAD/TRANSPOSE_CLIP` guards | Counter/guard identified | `RtlSchedulerSkeleton` implemented; runtime integration pending |
| feeder A/B | visible read plus delayed scheduler state/switch | state/switch delay array, input-RF counter, shift counter, two output stages and final FF | Register/counter chain and 266..303 waveform edges identified | Input-RF/feeder valid cadence and `EN_i/EN_i_d/input_switch/sa_en_i` implemented in isolation; focused rebuild pending |
| SA execute | `sa_en_i` | `calc_cnt == get_exe_cycle(kernel)*flow_loop_times-1`; 8 macro-row finish stages; 8 macro-column finish stages | Counter and structural dimensions plus 302/303/565 edges identified | Enable boundary coupled to execute counter in focused test; result stream pending |
| result stream | `storage_ready`/`sa_serial_o_flag` | 8-column finish delay, 32-row SA_ROW token stream, 32-beat output transposer, final valid/last FF | Source chain and 565..643 waveform edges identified | Control-only serializer implemented in isolation; runtime integration pending |
| D_OUT exit | scheduler is in `D_OUT` | non-final: `!shift_mode && flow_times_i != 1 && !keep_mode`; final: `update_finished`; either path also sees `update_finished \| write_finished` | Current source and 554/555 waveform agree | Per-tick guard updated; runtime integration pending |
| output RF accumulate | `result_final_valid_o` | internal x/y/flow/ins counters; `result_accum_done` latch; accumulator SRAM write pipeline | CSR values and 2505/2506 final edge identified | Control-only accumulation counter implemented in isolation; runtime integration pending |
| unload/writeback | `REGISTER_UNLOAD && result_accum_done` | output `register_addr`; two-stage valid/data; native SRAM-write outputs | 8x32x1 address extent and 2512..2767 valid window identified | Output address and delay skeleton implemented in isolation; runtime integration pending |
| native write transport | `register_wraddr_valid` | registered `mem_ctrl` master request; ACTIVE `crossbar_mi` slave request; next-edge TCDM sampling | 2512/2513/2514/2515 and 2767/2768/2769/2770 edges identified | Control-only write transport implemented; 27 focused tests passed |
| command finish | last output address | four-stage `sram_wr_pipe_done`; scheduler sees `write_finished` and raises `flow_end_o` | 2767/2771/2772 edge chain identified | Output finish chain coupled to scheduler; 27 focused tests passed |

## Phase B component boundary

`RtlSchedulerSkeleton` is implemented in the existing
`schedule_state.{hh,cc}` target so this first increment does not add a build
target or alter `SauModel` behavior. Its inputs are only the signals present at
the RTL scheduler boundary: CSR start write, memory `load_done`, resident
`register_load_done`, `update_finished`, `write_finished`, and the feeder's
last-flow clear. It owns the CSR start register, scheduler instruction-valid
register, core/instruction states, transpose/flow/instruction counters, input
switch, and completion pulse. The current RTL has no `update_finished_q`.

The component accepts no matrix dimension, fixture name, golden cycle, or
precomputed completion time. Unit tests cover the two-edge CSR-to-scheduler
start path, the 32-edge transpose counter, the flow/instruction clear guards,
single-flow update waiting, baseline non-final 554/555 D_OUT overlap, and
write-finished completion. It is not wired
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

`RtlInputFeederSkeleton` closes the current input-side producer boundary for
the fixed ATB/reuse-A path. It derives feeder `STATE_DELAY` from the named
SRAM/address/mem_ctrl delays, advances the input-RF x/y/flow/instruction read
counters per edge, and carries RF valid through the bypass shift/count path,
the two-stage B-valid pipe and final A/B/input-switch registers. Its coupled
test preserves the shared pre-edge snapshot into `RtlSaEnableSkeleton`, so
`sa_en_i` becomes visible at edge 302 while the execute counter first samples
it at 303. The source and three tests pass static checks; developer build
verification is pending.

`RtlResultSerializerSkeleton` is the next result-side producer. For the fixed
32x32 SA with 4x4 PE macros, it explicitly shifts the internal-finish token
through eight macro columns, starts the 32-row SA_ROW stream through the
storage-ready/output-start registers, loads and drains the 32-entry output
transposer, and registers final valid/last. It carries no arithmetic data and
accepts no elapsed/result-gap constant. A focused test checks every observed
edge and exactly 32 result-valid cycles. The final-instruction execute test now
consumes this produced `resultLast` instead of injecting it manually, proving
the extra registered `update_finished` edge. Runtime remains unchanged.

### Feeder-to-execute observation boundary (resolved)

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
scheduler_inst.D_OUT_cond
```

Only the first streamed instruction through its first `D_OUT` exit is needed.
This is a signal-origin check, not a new calibration workload.

### 2026-07-22 targeted waveform result

A fresh passing 64x256x256 run was captured from
`/home/xch/work/npu_lpnpu/sim/vcs/build/yinglong/simv` into
`yinglong.fsdb`.  The run reported 16,384 checked output elements and zero
mismatches.  Raw internal exports are retained in that repository as
`tmp/step5_5_first_command.csv` and `tmp/step5_5_second_command.csv`.
Both commands reproduce the same command-relative positive-edge indices:

| Relative edge | Observed transition/state | Post-edge `calc_cnt` | Source guard |
| ---: | --- | ---: | --- |
| 302 | `input_switch_i=01`, `sa_en_i=1` | 0 | `sa_en_i=EN_i_d` for switch `01` |
| 303 | first accepted SA enable | 1 | `SA_ENGINE` samples the pre-edge `sa_en_i` |
| 554 | scheduler enters first `D_OUT` | 245 | streamed scheduler counter clear |
| 555 | scheduler exits first `D_OUT` to `REUSE_LOAD` | 246 | non-final `D_OUT_cond` is already true |
| 565 | `internal_finish_pulse=1`, counter wraps | 0 | old `calc_cnt == CALC_CYCLE_i-1` on the 256th accepted enable |
| 566 | `delay_finish_flag[0]=1` | 0 | registered copy of `internal_finish_pulse` |
| 567 | `PE_valid_out[0]=1`, `execute_done_flag_o=1`, `update_finished_flag=1` | 0 | first macro-row finish register and `FIRSTOUT` update guard |
| 568 | `update_finished=1` | 0 | registered copy of `update_finished_flag` |

The subsequent internal-finish pulses are at relative edges 831, 1097, 1363,
1629, 1895, 2161, and 2427: a stable 266-edge cadence that includes real
feeder bubbles while requiring exactly 256 accepted enables per calculation.
The first seven `D_OUT` entries are at 554 + 266*n and exit one edge later.
The final `D_OUT` begins at edge 2416, waits for `result_last_o` and
`update_finished_flag` at 2505, sees registered `update_finished` at 2506,
and enters `REGISTER_UNLOAD` at 2507.

The result serializer for every internal-finish pulse follows the same source
chain. For the first pulse at edge 565, the observed edges are:

| Relative edge | Result-path event | RTL source |
| ---: | --- | --- |
| 574 | `macro_valid_out[0]=1` | finish token crosses eight macro columns |
| 575 | `SA_ENGINE.storage_ready=1` | registered first-macro valid |
| 576 | `output_start_flag=1`, serializer active | storage-ready rising edge |
| 578 | registered `row_score_valid=1` | SA_ROW token stream begins |
| 610 | output transposer `ready_o=1` | 32 input rows accepted |
| 611 | output transposer `valid_o=1` | first registered read enable |
| 612 | `result_final_valid_o=1` | final result-valid register |
| 642 | output transposer `last_o=1` | 32nd output beat |
| 643 | `result_last_o=1` | final result-last register |
| 644 | `update_finished=1` | update FSM output register |

The final calculation uses the same offsets: internal finish 2427, result last
2505, update finished 2506, and scheduler entry to `REGISTER_UNLOAD` at 2507.
The raw export is
`/home/xch/work/npu_lpnpu/tmp/step5_5_result_serializer.csv`.

The current output/writeback chain was captured from the same passing command.
The RTL ports expose accumulation extents `1x32x1x8` and output-address extents
`8x32x1`; both products are 256 tokens. The final edges are:

| Relative edge | Output-path event | RTL source |
| ---: | --- | --- |
| 2505 | final `result_final_valid_o` and `result_last_o` are high | serializer final registers |
| 2506 | `result_accum_done=1` | valid with x/y/flow/ins counters at their final values |
| 2507 | scheduler enters `REGISTER_UNLOAD` | registered `update_finished` satisfies final D_OUT guard |
| 2508 | `register_out_state=1`, one-cycle start flag | unload state and sticky accumulation done |
| 2510 | output address valid begins | registered `register_addr` RUNNING output |
| 2512 | native `sram_wr_valid_o` begins | address valid d1/d2 |
| 2765 | output address last | x=7, y=31, c=0 |
| 2767 | `sram_wr_data_last_o=1`, final native write-valid beat | last and valid d1/d2 |
| 2771 | `sram_wr_last_o=1`, `flow_end_o=1` | pipe-done d1..d4 and scheduler guard |
| 2772 | scheduler returns IDLE, `crossbar_done=1` | registered `flow_end_o` |

The raw exports are
`/home/xch/work/npu_lpnpu/tmp/step5_5_output_config.csv` and
`/home/xch/work/npu_lpnpu/tmp/step5_5_output_writeback.csv`.

The native write transport adds two visible registered boundaries and one
sampling edge. `mem_ctrl` changes `sau_sram_enable` one edge after
`register_wraddr_valid`; ACTIVE `crossbar_mi` changes the selected
`slave_req[1]` one edge later; the synchronous TCDM samples that registered
request on the following positive edge. There is no ready input or retry path
for this native master. The resulting windows are:

| Boundary | First edge | Last edge | Count |
| --- | ---: | ---: | ---: |
| `register_wraddr_valid` | 2512 | 2767 | 256 |
| `sau_sram_enable` / `master_req[0]` | 2513 | 2768 | 256 |
| selected `slave_req[1]` | 2514 | 2769 | 256 |
| physical TCDM write sampling | 2515 | 2770 | 256 |

`flow_end_o` is at 2771, one edge after the final physical SRAM write, and
`crossbar_done` is at 2772. The crossbar consumes that done pulse and leaves
ACTIVE at 2773. The raw export is
`/home/xch/work/npu_lpnpu/tmp/step5_5_crossbar_write.csv`.

The resident input tail uses a different completion boundary from the
scheduler. For the baseline `8x32x1` resident extent:

| Boundary | First edge | Last edge | Count |
| --- | ---: | ---: | ---: |
| registered resident address request | 2 | 257 | 256 |
| `core_register_data_out_valid` | 7 | 262 | 256 |
| feeder `register_file_wvalid_o` | 8 | 263 | 256 |
| input-RF `padding_shifted_valid` / SRAM write | 9 | 264 | 256 |

`mem_ctrl.STATE_DELAY=SRAM_DELAY+1` supplies four request-valid registers and
`core_register_data_out_valid` adds the fifth edge. The feeder adds one edge,
then `stream_padding_shifter.valid_o` adds one more. Scheduler enters
`TRANSPOSE_LOAD` at edge 258 from the resident address last; delayed
`core_state_i_reg` keeps the input-RF write gate in REGISTER_LOAD long enough
for edges 259..264 to drain. Therefore address completion and resident-data
availability must remain distinct model events. The raw export is
`/home/xch/work/npu_lpnpu/tmp/step5_5_input_frontend.csv`.

This closes the enable/counter observability gap.  The earlier 246/247
reconstruction counted the newly visible combinational `sa_en_i` at edge 302,
but the `SA_ENGINE` sequential block first consumes it at edge 303.  The
counter waveform proves that no ten-cycle correction is valid.  Coupling must
model `EN_i -> EN_i_d`, the input-switch register, combinational `sa_en_i`,
and pre-edge sampling explicitly; scheduler progress is allowed to overlap
the unfinished SA calculation.

The current passing `yinglong` snapshot is now authoritative. The obsolete
`update_finished_q` guard has been removed from `RtlSchedulerSkeleton`, and an
isolated `RtlSaEnableSkeleton` now models `EN_i`, registered `EN_i_d`,
combinational `sa_en_i`, and pre-edge consumption by the execute skeleton.
Runtime integration still waits for focused-test compilation and a newly
captured acceptance package.

## Step 5 formulas that must become signal-driven

The old eight packages are not a strict acceptance oracle. Aggregate timing
formulas must be replaced from current RTL source and new independent traces:

1. `scheduler.D_OUT_cond_A` immediately releases a non-final, non-shift,
   non-keep instruction when `flow_times_i != 1`; the final instruction waits
   for `update_finished`. Scheduler progress therefore overlaps SA execute.
2. `REGISTER_UNLOAD` is additionally gated by `result_accum_done`, and
   `write_finished` is four explicit registers after the last two-stage output
   address/data pipeline.  The old writeback/completion sums do not express
   this control path.
## Boundary edges in the captured waveform export

The following old package edges are retained only as historical diagnostics;
they are not current golden observations:

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
