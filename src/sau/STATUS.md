# SAU Cycle-Level Behavioral Model Status

Status type: Current snapshot
Last updated: 2026-07-28

- Active branch: `feature/sau-command-types`
- Snapshot HEAD before this documentation migration: `bef303b86c`
- Plan entry: [`PLAN.md`](PLAN.md)
- Active plan: [`PLAN3.md`](PLAN3.md), Step 5 complete; Step 6 scope checkpoint
- Historical status index: [`docs/status/INDEX.md`](docs/status/INDEX.md)

## Current Goal

Implement the CSR-driven Int8 GEMM functional datapath as a cycle-level gem5
resource model. Real 256-bit payloads must pass through the modeled input,
transpose/reuse, 32x32 systolic-array, output-RF, serializer, and writeback
resources. Final memory bytes and strict boundary timing must match the frozen
RTL evidence without replacing the datapath with a direct GEMM calculation.

## Current Stage

- The original timing-model milestone and PLAN2 CSR/control/timing alignment
  are complete. PLAN2 commit `458ad7e3cb` remains the timing regression
  baseline.
- PLAN3 Steps 0–5 are complete for the currently frozen Reuse-A domain
  (`reuse_mode=01`); broader Step 6 coverage awaits the Flow3 scope decision.
- PLAN3 Step 5 increment 4 consumes the registered unload FIFO at strict
  writeback and submits real payload bytes. Both cutbit-8 and cutbit-1 strict
  outputs match frozen RTL final memory for 1024/1024 bytes.
- Increment 5 enables strict Flow1 and multi-flow/multi-instruction array
  tiles. The 64x160x64 two-command fixture matches 4096/4096 final bytes and
  both commands match the RTL 690-cycle extent.
- The Flow2 K512 `[flow2, flow0]` sequence is now end-to-end validated. Its
  command extents are 569/685 cycles with a 93-cycle gap; the retain command
  emits no result/write beats and the final command emits 32 of each. Final
  memory matches 1024/1024 bytes.
- K768 `[flow2, flow2, flow0]` also matches RTL at 569/569/685 cycles with
  82/89-cycle gaps, proving state survives two consecutive retain boundaries.
- Payload beat stats are checked against trace events; final-memory comparison
  remains verifier-owned.
- `reuse_mode=00/10/11` remains losslessly decoded with existing interfaces
  preserved, but is deferred while the RTL evolves and is not in the current
  functional acceptance scope.

## Current Implementation Boundary

### Completed PLAN3 Step 5 Boundary

- `SauOutputResourceConfig` owns raw output x/y/flow/instruction steps,
  bursts, and unload counters.
- `OutputRegisterFile` implements 256 logical rows over two 128-entry SRAM
  halves selected by address bit 7.
- Accepted result rows use raw nested-counter addressing. Flow mode 0/1
  overwrites stale data; flow mode 2 applies signed 16-bit wrap accumulation,
  RAW-forwarded same-address updates, and signed-int8 saturation.
- Sticky accumulation completion and hard reset are separate.
- `ResultSerializer` owns one finite 32-row result bank. Flow mode 0 drains
  rows, flow mode 1 drains reversed columns, and flow mode 2 retains output
  data for the next accumulation. `trans_mode` has no output-order effect.
- Raw flow mode 3 remains decoded but functionally unconfirmed.
- `OutputRegisterFile::startUnload/tickUnload` uses the shared raw
  `register_addr` program, reads both SRAM halves, and models the registered
  address output plus valid/address/data d1/d2 pipeline. The first payload
  appears four ticks after launch and the baseline 256-beat extent ends at
  tick 259.
- `StrictPayloadDatapath` now accepts real array rows into the serializer,
  consumes driver result-valid edges into output RF, and advances
  `REGISTER_UNLOAD` into a queued native payload/address/last stream.
- `SauModel::issueWrites()` consumes that stream as the strict payload
  authority, checks token/address/last agreement, and commits real bytes.
- CSR-driven resident Operand-A reads now use the shared
  `RtlResidentAddressProgram`, preserving `register_addr.sv` x/y/channel
  stepping and padding address behavior. Synthetic direct commands retain
  their existing linear `StreamDesc` address path.
- Array finish is derived from `32 * flow_loop_times` per instruction rather
  than a transposer-bank tail. Completed non-retain tiles snapshot and clear
  their PEs before the next instruction while preserving row-stream overlap.
- Eleven focused tests now cover serializer capacity/order/reset, both SRAM
  halves, saturation, signed16 overflow, same-address forwarding, pointer
  nesting, registered unload timing/address/payload/last, phase ownership,
  completion clear versus reset, and invalid zero dimensions.
- The refreshed K512 RTL run accepts a flow-mode-2 command followed by a
  flow-mode-0 command. The second start is 93 SAU cycles after the first done;
  only the final command produces the 32-row output/write burst, and final
  memory matches 1024/1024 bytes.
- Flow1 executes two sequential M64/K160/N64 commands. Each command consumes
  320 array inputs, emits and writes 64 results, and completes at edge 690.
  Final memory matches the packaged RTL oracle for 4096/4096 bytes.
- Flow2 retains the 32x32 PE accumulator state across command completion and
  drains any still-live macro pipeline state before capture. The following
  Flow0 command restores that state and performs the only final writeback.
- The permanent Flow2 functional verifier checks final memory plus the
  packaged command extents, inter-command gap, and per-command result/write
  counts.

## Authoritative Sources and Baselines

- Frozen payload RTL source: fetched commit `e722852bd9ab`; file hashes and
  elaborated hierarchy are recorded in `RTL_TIMING_PROVENANCE.md`. The
  evolving current `npu_lpnpu` HEAD is not automatically authoritative.
- Frozen `register_file_out.sv` SHA-256:
  `1a3e1cd65d2ea2cb103e323fb6b570eceaf8ca3741f6619eca62f72fba14964a`.
- Step 4 ATBD array-boundary golden:
  `/home/xch/work/npu_lpnpu/tmp/plan3_step4_atbd_array_boundary.csv`,
  SHA-256
  `ecd53b769068b32041463db92b748ad9030eb93f677c901be63c847d65b9794b`.
- Eight current coverage/hold-out packages under `tests/gem5/sau/ref` remain
  the PLAN2 strict architecture/state and timing-memory causal regression
  baseline.

## Verification

| Scope | Result | Status |
| --- | --- | --- |
| SAU quick regression | 72/72 checks across 26 suites, including Flow1 and both Flow2 fixtures | Passed |
| Step 4 focused targets | `boundary_trace` 4/4, `payload_datapath` 4/4, `schedule_state` 33/33 | Passed |
| Step 4 strict runtime boundary | 32 input pairs and 32 result rows; payload/sequence/cycles equal frozen RTL | Passed |
| Step 5 focused resources | address 5/5, A RF 7/7, CSR 10/10, output 11/11, payload 4/4, array 9/9, command driver 35/35 | Passed |
| Step 5 cutbit-8 final memory | 1024/1024 bytes; identical SHA-256 | Passed |
| Step 5 cutbit-1 final memory | FSDB replay; 1024/1024 bytes; identical SHA-256 | Passed |
| Flow2 K512 RTL rerun | `[2,0]` accepted sequentially; 1024/1024 bytes, zero mismatches | Passed |
| Flow2 K512 gem5 runtime | 569/685-cycle extents; 93-cycle gap; 1024/1024 bytes; identical SHA-256 `0fb6c4…e6c5` | Passed |
| Flow2 K768 gem5 runtime | 569/569/685-cycle extents; 82/89-cycle gaps; 1024/1024 bytes; identical SHA-256 `6336cf…39ae` | Passed |
| Flow1 64x160x64 fixture | Two commands, 690 cycles each; 4096/4096 final bytes | Passed |
| Step 5 performance baseline | Three-run medians 0.16–0.17 s and 66,328–70,140 KiB RSS; [report](docs/reports/plan3-step5-performance-baseline.md) | Passed |

## Blockers and Risks

- The packaged Flow2 K512 evidence comes from the evolving Yinglong RTL
  worktree recorded in its manifest; it does not silently replace the frozen
  `e722852bd9ab` baseline for existing strict checkpoints.
- Reuse modes other than Reuse-A are intentionally deferred; do not expand
  acceptance scope while the source RTL is evolving.
- Deferred reuse coverage remains open.
- Raw Flow3/transpose-retain remains decoded but functionally unconfirmed.

## Next Actions

1. Decide whether Flow3/transpose-retain belongs in the current RTL-stable
   acceptance domain; otherwise keep its explicit fail-fast boundary.
2. Advance Step 6 integration coverage without expanding the
   deferred non-Reuse-A scope.
3. Add command-to-command memory-dependency coverage within that agreed scope.

## Historical Detail

Pre-migration detail is preserved at the
[status archive](docs/status/archive/status-through-2026-07-27-plan3-step5-increment1.md);
use [`docs/status/INDEX.md`](docs/status/INDEX.md) only for provenance or
regression investigation.
