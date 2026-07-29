# SAU Cycle-Level Behavioral Model Status

Status type: Current snapshot
Last updated: 2026-07-29

- Active branch: `feature/sau-command-types`
- Plan entry: [`PLAN.md`](PLAN.md)
- Active plan: [`PLAN3.md`](PLAN3.md), Step 6 increment 13 complete
- Historical status index: [`docs/status/INDEX.md`](docs/status/INDEX.md)

## Current Goal

Implement the CSR-driven Int8 GEMM functional datapath as a cycle-level gem5
resource model, carrying real 256-bit payloads through every finite resource.
Final memory and strict boundary timing must match RTL without direct GEMM.

## Current Stage

- PLAN2 is complete; Step 6 increment 13 is complete on frozen Reuse-A.
- ABTD boundary passes; its pair differs from ATBD only by `trans_mode=1→2`
  and has distinct RTL final memory.
- The generic payload runtime consumes delayed `outputInputSwitch()`: ABTD
  B-valid schedules next-edge bank load and switch bit 1 selects A/B payload.
  Increment 7 routes switch-01 transposer output to activation and registered B
  to weight; payload passes 5/5. Increment 8 adds reproducible ABTD CSR replay
  with inferred fixed address order and sampled mode/start cross-checks.
  Increment 12 payload passes 5/5; increment 13 narrowly opens ABTD Flow0.
- Yinglong crossbar serializes CPU CSR access; every no-poll probe start stayed
  in IDLE. Forced internal starts and UVM are outside this acceptance claim.
- A four-command chain proves strict memory persistence: command 4 reads the
  32 beats written by command 3 after visibility; 1024/1024 bytes match RTL.
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
| SAU quick regression | 78/78 checks across 28 suites, including permanent ABTD unintended-output coverage | Passed |
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
| Step 6 start admission | focused 37/37; Yinglong starts remain after done; [report](docs/reports/plan3-step6-yinglong-start-serialization.md) | Passed |
| Step 6 memory chain | `[2,2,0,0]`; command 3 write → command 4 Operand-A read; 1024/1024 bytes | Passed |
| Step 6 ABTD exact boundary regression | SRAM read, feeder A/B, transposer input/output payload and cycles | Passed |
| Step 6 ATBD/ABTD pair contract | Same input/config except `trans_mode=1→2`; distinct RTL final memory | Passed |
| Step 6 ABTD feeder/array routing | Registered mux and switch-01 array extension; payload 5/5 | Passed |
| Step 6 ABTD CSR replay | 7 inferred writes, decoded snapshot, package SHA-256; Python 21/21 | Passed |
| Step 6 ABTD command timing | Result 157–188, write 196–227, done 231; schedule 38/38 | Passed |
| Step 6 ABTD feeder valid | A=64, B=1 resident tail + 32 streamed, SA=32; schedule 38/38 | Passed |
| Step 6 ABTD feeder payload | Resident tail, 32 streamed B, 32 transposer rows; payload 5/5 | Passed |
| Step 6 ABTD array inputs | 31 zero activations, then first real column; payload 5/5 | Passed |
| Step 6 ABTD strict runtime | Boundary exact; one transposer column; done=231; final memory 1024/1024, SHA-256 `2c2dd1…075` | Passed |

## Blockers and Risks

- The packaged Flow2 K512 evidence comes from the evolving Yinglong RTL
  worktree recorded in its manifest; it does not silently replace the frozen
  `e722852bd9ab` baseline for existing strict checkpoints.
- Reuse modes other than Reuse-A are intentionally deferred; do not expand
  acceptance scope while the source RTL is evolving.
- Raw Flow3/transpose-retain remains decoded but functionally unconfirmed.
- Verdi license is unavailable; new ABTD array FSDB signals cannot be exported.

## Next Actions

1. Expand integrated transpose coverage beyond the single ABTD fixture.
2. Verify remaining reset/clear/retain state boundaries.
3. Keep Reuse-B/AB/none deferred until the evolving RTL is frozen.

## Historical Detail

Pre-migration detail is in the [status archive](docs/status/archive/status-through-2026-07-27-plan3-step5-increment1.md); use [`docs/status/INDEX.md`](docs/status/INDEX.md) only for provenance/regression.
