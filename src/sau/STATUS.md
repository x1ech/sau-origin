# SAU Cycle-Level Behavioral Model Status

Last updated: 2026-07-27

## Goal

Build a cycle-level behavioral model of the Systolic Array Unit (SAU) in
gem5 for system-level performance analysis and design-space exploration.
The model targets architecture-relevant cycle timing rather than RTL
register-level equivalence, and the first milestone does not perform
arithmetic computation.

## Session Handoff Checkpoint — 2026-07-26

Read this section first when resuming PLAN3 work in a new session.

- Committed state: `07c43b15f5` (PLAN3 Step 1), `6bcfe89b41` (PLAN3
  Step 2), and `b289bc47dc` (PLAN3 Step 3 increments 1-7) sit on top
  of the pushed PLAN2 baseline `458ad7e3cb`; the full SAU quick suite
  stays at 63/63 across 23 suites. Step 1 is closed except two
  consumer-deferred items (strict read-payload consumption lands with
  Step 3 runtime integration; real write payloads with Step 5). Step 2
  is closed with all seven checklist items checked.
- PLAN3 Step 3 increments 1-8 are implemented (dated sections at the
  end of this file): input-datapath payload resources, feeder A/B
  payload chains, the transposer bank and sa_feeder arbiter, the
  generic boundary comparator, and — in increment 8 — the ABTD
  control chain derived from the frozen
  `scheduler.sv`/`feeder.sv`/`sa_feeder.sv`. The model boundary trace
  now matches the ABTD golden in both sequence and cycles modes across
  `sau_sram_rdata`, `data_A`, `data_B`, `trans0_inRow`, and
  `trans0_outCol`, reproducing the raw `[A31, B0, A1..A30]` bank load;
  the first non-default milestone (B -> B^T `data_functional`) is
  closed and increment 8 is developer-verified.
- Payload-source authority: the frozen `sa_execute`/`sa_element`
  sources are extracted from fetched commit `e722852bd9ab` (hashes
  frozen in `RTL_TIMING_PROVENANCE.md`); the current `npu_lpnpu` HEAD
  has evolved past the contract in seven files and is not authority.
- Step 3 is complete for the current supported domain. Increment 9 integrated
  strict runtime payload resources and transposer/reuse statistics into
  `SauModel`; increment 10 matched data_A/data_B and T0/T1 inRow/outCol
  payloads and exact cycles against the deep ATBD RTL export. Focused tests
  pass 11/11 for the transposer and 3/3 for the payload datapath; both
  boundary comparison modes pass and the full quick suite remains 63/63.
  On 2026-07-27 the user froze current reuse support to R-A
  (`reuse_mode=01`) while the RTL project evolves. R-none/R-B/R-AB remain
  decoded and existing code is preserved, but they are deferred rather than
  Step 3 blockers. Step 4 is next.
- Per `src/sau/AGENTS.md`, gem5 builds remain developer-owned; provide
  incremental focused-target commands first.

## Session Handoff Checkpoint — 2026-07-23 (historical)

This checkpoint covers the completed Step 5.5 bring-up and is retained
for provenance.

- Authoritative RTL source: `/home/xch/work/npu_lpnpu`.
- Authoritative passing simulation artifact:
  `/home/xch/work/npu_lpnpu/sim/vcs/build/yinglong/yinglong.fsdb`, generated
  by the passing 64x256x256 matmul run.
- gem5 worktree:
  `/home/xch/work/sau_n_gem5/sau_origin_feature_sau_command_types_b9fbc18_20260720`.
- Focused target status: the developer rebuild after adding the multi-shape
  `RtlCommandDriverSkeleton` regression passed **33/33 tests**. The baseline
  driver test
  first exposed resident tail data incorrectly producing B-valid at edge 266.
  Adding feeder.sv's registered `input_switch_case` corrected the first B edge
  to 301, but the next rebuild showed only 2007/2048 B tokens and 2016/2048
  accepted SA tokens. The remaining cause was an extra skeleton-only
  core-state gate that dropped B while RTL's arbiter remains active through
  `D_OUT`. Removing that gate closed the full edge and token checks. A new
  multi-shape conservation test now covers the current 1/4/8 flow and 1/4/8
  instruction combinations. The first `SauModel` strict-runtime integration
  passed the historical baseline architecture and state comparators after
  relinking `gem5.opt`. The result/write producer increment also passes both
  comparators. The read/array producer increment also passes both comparators.
  The actual driver stage-ledger increment passes the developer's 33/33
  focused checkpoint and, after relinking, the baseline architecture and
  state comparators. Both commands emit the expected observed windows. Strict
  aggregate-consumer cleanup is now relinked and verified: both comparators
  and all observed windows remain unchanged. The constrained non-strict run
  and four DSE monotonicity configurations also pass.
- Static status: `git diff --check` passes. Codex did not run the gem5 build;
  builds are developer-owned per `src/sau/AGENTS.md`.
- Post-link verification used `m5out/sau-rtl-driver-cleanup`,
  `m5out/sau-cleanup-constrained`, and four `m5out/sau-cleanup-dse-*`
  directories. `verify_dse.py` now avoids Python 3.9-only
  `str.removeprefix`, so its documented command runs under the workspace's
  Python 3.8.2; its 2 unit tests, syntax check, and four-stat monotonicity
  verification pass.
- The worktree intentionally contains uncommitted Step 5.5 changes and a
  user-owned modification to `src/sau/AGENTS.md`. Preserve them; do not reset,
  checkout, or overwrite unrelated changes.

Focused rebuild command, if the next increment changes `schedule_state`:

```bash
cd /home/xch/work/sau_n_gem5/sau_origin_feature_sau_command_types_b9fbc18_20260720
scons build/ALL/sau/schedule_state.test.opt \
    --ignore-style --limit-ld-memory-usage -j32
./build/ALL/sau/schedule_state.test.opt
```

Implemented in `schedule_state.{hh,cc}`; items 1–9 are focused-test verified:

1. `RtlSchedulerSkeleton` — start registers, states, transpose/flow/ins
   counters, current D_OUT guards, switch and command completion.
2. `RtlResidentLoadSkeleton` — resident `register_addr` x/y/c requests.
3. `RtlStreamLoadSkeleton` — streamed x/y/flow/ins address and load-done
   control for the validated x=1/y=32 matmul shape.
4. `RtlExecuteUpdateSkeleton` and `RtlSaEnableSkeleton` — accepted SA-enable
   counting, finish/update registers and pre-edge enable sampling.
5. `RtlResultSerializerSkeleton` — macro finish, 32-row stream, output
   transposer and final result valid/last.
6. `RtlOutputWritebackSkeleton` — result accumulation, unload launch,
   256-beat output address stream and native write-finished chain.
7. `RtlSramWriteTransportSkeleton` — registered mem_ctrl/crossbar/TCDM write
   transport with 256-token conservation.
8. `RtlResidentFillSkeleton` — resident mem_ctrl visibility, feeder input-RF
   write valid and padding-shifter/input-SRAM tail drain.
9. `RtlInputFeederSkeleton` — input-RF read counters, feeder A/B-valid and
   input-switch pipelines through the SA-enable observation boundary.
10. `RtlCommandDriverSkeleton` — shared pre-edge composition of the verified
    producers from CSR start through scheduler `commandDone`, including one
    shared resident/stream memory-read visibility path and token accounting.
    The baseline edge/token and multi-shape conservation tests pass in the
    33/33 checkpoint.

Current boundary: strict `SauModel` commands now construct and tick a
command-local driver. Driver core state/input switch replace the old
formula-based state-trace projection, and local completion is additionally
gated by driver `commandDone` plus driver token conservation. Strict result
events and SRAM write requests now consume driver `resultValid` and registered
mem_ctrl write valid/last rather than aggregate result/write delays. Strict
external reads consume the registered shared-SRAM request, and A/B admissions
consume driver valid pulses.
At command completion the driver now appends observed first/last/span rows for
resident read, stream read, A, B, result, and mem_ctrl write plus the actual
command-done edge to the timing ledger. The old derivation rows remain for
provenance comparison. The latest source cleanup also removes the remaining
strict aggregate fill/gap/completion consumers while preserving the original
non-strict DSE schedulers. Developer relinking, current-trace acceptance, and
the CSR-only prediction smoke test now pass; Step 5.5 is complete.

The latest implementation adds the input-RF read/feeder coupling from the
already exported current-FSDB edges:

```text
register_file_rden   266
register_file_rvalid 267
data_A_valid         269
next RF rden         298
data_B_valid         301
input_switch_f=01    302
sa_en_i=1            302
first SA sample      303
```

The input-feeder skeleton models `register_file_in` read counters/valid,
feeder `REGISTER_DELAY=2` plus final A/B valid FF, and feeds the existing
`RtlSaEnableSkeleton` using shared pre-edge snapshots. Its focused tests check
the complete 266/267/269/298/301/302/303 boundary and now pass 30/30.

The new command driver composes every verified producer using one pre-edge
snapshot per tick. Its baseline test checks the full 2..2772 edge chain and
conservation of 256 resident reads, 2048 streamed/RF/A/B/accepted-SA tokens,
256 results, and 256 native/physical writes. The first 32-test rebuild exposed
and localized the missing registered A/B-arbiter gate; the resulting early B
token caused all later result/writeback failures. The corrected driver and
multi-shape extension now pass 33/33; strict runtime integration and the
observed stage ledger are complete. The current handoff is the aggregate
consumer cleanup described at the top of this file.

Relevant raw exports under `/home/xch/work/npu_lpnpu/tmp`:

```text
step5_5_first_command.csv
step5_5_second_command.csv
step5_5_result_serializer.csv
step5_5_output_config.csv
step5_5_output_writeback.csv
step5_5_crossbar_write.csv
step5_5_input_frontend.csv
```

Do not use the old eight fixture traces as golden. They correspond to a
retired RTL implementation and have been removed from quick strict
registration. Fresh current-RTL traces are required only for final
independent acceptance after runtime integration.

Design and implementation references:

- [Design specification](../../docs/superpowers/specs/2026-07-02-sau-cycle-level-model-design.md)
- [Implementation plan](../../docs/superpowers/plans/2026-07-02-sau-cycle-level-model.md)
- [Chinese implementation plan](../../docs/superpowers/plans/2026-07-02-sau-cycle-level-model-zh.md)
- [Task 1 RTL baseline handoff plan](../../docs/superpowers/plans/2026-07-06-sau-task1-rtl-baseline-handoff-zh.md)

## Current State

- Current stage: PLAN3 Steps 1-3 are complete for the current Reuse-A support
  domain. Increment 8 closed the first non-default ABTD boundary milestone;
  increments 9-10 integrated runtime payload resources/statistics and matched
  the deep ATBD operand/transposer payloads in both sequence and exact cycles
  modes. R-none/R-B/R-AB are deferred while the RTL project evolves. Step 4
  is next; the PLAN2 timing baseline below stays authoritative for regression.
- First-milestone record: Tasks 1 through 11 implementation and validation
  complete.
  Final milestone commits `50d42ef51c` and `f303ee71c5` are pushed to
  `sau-origin/feature/sau-command-types`. The standalone model can
  now emit the imported RTL baseline's two-command shape, addresses, row
  count, event counts, fixed read/write accepted cadence, and causal event
  order before strict cycle calibration.
- Active branch: `feature/sau-command-types`
- Worktree: `/home/xch/work/sau_n_gem5/sau_origin_feature_sau_command_types_b9fbc18_20260720`
- Development remote: `sau-origin`
- Latest completed milestone: `SauModel` now schedules external reads,
  RTL-style `register_file_in` reuse for Operand-A, B streaming, array timing,
  output writeback,
  phase progression, command completion, and drain behavior.
- Latest calibration: Task 8.5 adds explicit matmul timing-policy components.
  A can lead B by a configurable 32-token skew, array inputs are emitted in
  32-beat bursts with tile/flow-boundary gaps, results are produced as
  32-result flow bursts after the calibrated fill latency, writes wait until
  the result stream is complete, and command completion waits after the final
  write.
- Latest simulation milestone: Task 10 front-half work adds a reusable
  `--rtl-profile` standalone configuration and command-level address strides
  so gem5 can generate the same two-command profile as
  `tests/gem5/sau/ref/int8_gemm_64x256x256/architecture.csv`.
- Latest calibration milestone: Task 10 now adds `--calibration-memory`, a
  local fixed-cadence memory path for RTL comparison. It bypasses the generic
  `SystemXBar + SimpleMemory` request/retry cadence only when explicitly
  enabled; system/backpressure runs keep using the timing-memory port. The
  calibration trace now passes causal comparison against the imported RTL
  reference.
- Latest strict-calibration implementation: `command_start_cycles` now gates
  command-local feeder issue rather than only delaying the gem5 event, and
  `ArrayPipeline` resets its command-local II epoch between synthetic
  commands. This addresses the observed command-1 three-cycle early preload
  and command-2 inherited-pipeline timing boundary. On 2026-07-10,
  `--rtl-profile --calibration-memory` passed strict comparison against the
  imported RTL reference.
- Latest system-memory validation: the constrained `--rtl-profile` run now
  passes causal comparison. Causal comparison preserves command-local
  dataflow and dependency order while allowing independent timing-memory
  request/response events to interleave in the CSV trace.
- Latest finalization milestone: Task 11 adds complete latency statistics,
  a statistics-window fix for the first synthetic command, DSE monotonicity
  checks for array/FIFO capacity, and `src/sau/README.md`. The final strict,
  constrained causal, and four DSE simulations passed on 2026-07-10.
- Current PLAN2 milestone: Step 1 CSR/decode is verified by the focused
  `csr_config.test` (7/7 passed). Step 2's semantic ATB/reuse-A schedule
  state passed its focused tests and the legacy direct-command strict
  regression on 2026-07-14: both traces have 18,446 data rows and strict
  comparison exits successfully. PLAN2 Step 3 is verified on 2026-07-14:
  `timing_policy.test` passed 3/3, `csr_config.test` passed 7/7, and strict
  comparison of the baseline CSR fixture passed with 18,446 data rows.
  Strict `--rtl-profile CSR_FIXTURE` replays
  `csr_writes.csv`, loads named RTL elaboration parameters from the manifest,
  derives scheduler timing through `timing_policy.{hh,cc}`, emits a
  per-command CSV ledger, and rejects strict timing overrides. The direct
  command controls remain explicitly non-strict DSE controls. Step 5 later
  promoted five coverage fixtures and validated three hold-outs after formula
  freeze; the hold-outs remained excluded from timing adjustment.
- PLAN2 Step 4 instrumentation is built and exercised on 2026-07-15:
  `schedule_state.test` passed 5/5; `sau_state.csv`
  records semantic schedule transitions independently of `architecture.csv`;
  `compare_state_trace.py` normalizes RTL `diagnostic.csv` and pinpoints the
  first state/switch/cause mismatch; `validate_fixture.py` validates the eight
  PLAN2 fixture packages and their SHA256 contracts. Python comparator tests,
  fixture validation, and the baseline architecture strict regression pass.
  The original command-1 mismatches at cycles 258 and 290 are fixed and the
  focused C++ tests plus baseline architecture strict comparison pass. The
  Step 4 is verified on the baseline fixture. The debug projection separates
  runtime execution eligibility from scheduler-observable trace state, aligns
  the named `data_last` window, `TRANSPOSE_CLIP` `SA_SIZE` duration, `D_OUT`
  edge, and final `REGISTER_UNLOAD` switch reset. The latter derives from
  `sa_feeder.result_last/update_finished`, scheduler state edges, and the
  feeder switch pipeline. On 2026-07-15, `timing_policy.test` passed 3/3;
  baseline architecture strict and state strict comparators both passed with
  18,446 architecture data rows and 56 state transitions. Coverage state
  strict comparison remains required before fixture promotion.
- PLAN2 Step 4.5 storage-contract implementation is verified on
  2026-07-15. `RtlStorageTiming` derives the supported 32-byte beat and fixed
  read-visible latency from `SRAM_DATA_WIDTH` and `SRAM_DELAY + 1`; strict
  timing no longer consumes the standalone calibration-latency parameter.
  Strict requests share one issue slot with read-first scheduling and runtime
  checks for single issue, ordered fixed-latency responses, and A-preload
  request ordering. Strict Operand-B requests retain the RTL's no-ready fixed
  cadence and assert the returned-token staging bound derived from the named
  SA row/read-ahead window. Non-strict timing memory instead reserves capacity
  for both returned and in-flight reads using `input_buffer_entries`, allowing
  that path to apply backpressure without changing strict cycles.
  `validate_fixture.py` now checks the same storage contract against every
  architecture trace. It validated 20,352 reads across all eight PLAN2
  packages; Python tests passed 18/18, `timing_policy.test` passed 4/4, and
  `csr_config.test` passed 7/7. The baseline strict simulation completed with
  18,446 architecture data rows and 56 state transitions; both comparators
  returned success. Strict rejected a read-latency override, while a
  non-strict 5-cycle override completed with both observed reads at 5 cycles.
- PLAN2 Step 5 fixed-memory convergence completed on 2026-07-15. The model
  preserves `scheduler.flow_times_i = flow_loop_times` separately from
  `scheduler.ins_times_i = vertical.ins_cycle`, generates nested
  x/y/flow/instruction B addresses, supports asymmetric A/B array-input
  extents, and derives short-D_OUT, result gaps, input-switch reset, early
  `REGISTER_UNLOAD`, writeback, and command-boundary cleanup from named RTL
  structure. Small, K, N, M, and baseline coverage all passed architecture
  and state strict comparison; formulas were then frozen before M96, K128,
  and N128 hold-out validation. All three hold-outs passed architecture and
  state strict comparison as well, for 16 successful final comparators over
  eight fixtures. No fixture name, test ID, command ID, matrix-size branch,
  or hold-out timing override was introduced.
- K128 initially exposed a state-only generalization defect while its
  architecture trace already matched. The old `REUSE_LOAD` duration used
  `inputBeatsPerInstruction - BReadAhead`, which agreed with baseline only
  because `flow_times_i` and `SA_SIZE - BReadAhead` were both eight. Direct
  inspection of `scheduler.sv`'s `flow_times_cnt` and `data_last` guards gives
  the generic expression `inputBeatsPerInstruction - SA_SIZE + flow_times_i`.
  This derivation is now a named `TimingPolicy::flowExecuteCycles` ledger
  term and is covered by a 100-cycle K128-shape unit assertion. After the
  structural correction, all five frozen coverage fixtures and all three
  hold-outs reran without differences.
- `tests/gem5/sau/test_sau.py` now registers all eight packages as RISCV quick
  strict suites. Its custom verifier runs the existing architecture strict
  comparator and RTL-diagnostic semantic-state comparator, rather than
  directly diffing unnormalized CSV files.
- Final Step 5 verification used the developer-built 2026-07-15 binary. Eight
  focused C++ executables passed 58 tests total: address generator 4, A
  register file 6, array-input scheduler 4, command 18, CSR config 7,
  schedule state 5, timing policy 6, and token pipeline 8. Python utility
  tests passed 18/18; all fixture SHA256/schema checks and `git diff --check`
  passed. The final eight gem5 runs all exited through `SAU command complete`,
  and their eight architecture plus eight state strict comparisons returned
  success. The gem5 test framework also executed the small strict suite and
  passed its simulation, exit-regex, and custom strict-verifier checks 3/3.
- PLAN2 now inserts Step 5.5 before timing-memory/DSE. The old eight-fixture
  equality is historical only and is no longer evidence for the current RTL.
  The next implementation must audit the complete CSR-to-command-done RTL
  timing chain and replace strict aggregate end-cycle scheduling with a
  per-tick timing skeleton driven by the RTL counters, valid/last signals,
  register delay chains, and actual handshakes. Existing CSR replay is the
  only required input path; no CPU model, M/K/N-to-CSR generator, or extra
  simulation mode is planned. New golden traces must be recaptured; the old
  traces are neither regression oracles nor completion criteria.
- Step 5.5 Phase A now uses the current files under
  `/home/xch/work/npu_lpnpu` and the passing `yinglong` waveform as the
  authoritative RTL baseline without
  depending on that repository's commit state. The CSR-to-command-finish
  chain, fixed elaboration values, counters, valid/last paths, and output
  write pipeline are recorded in `src/sau/RTL_TIMING_PROVENANCE.md` with file
  hashes. The retired 2026-07-15 audit found a different scheduler D_OUT
  implementation driven by `update_finished/update_finished_q`, while output unload was
  gated by `result_accum_done` and terminates through explicit two-stage data
  plus four-stage finish pipelines. Therefore the Step 5 short-path,
  early-unload, and completion aggregates must not be carried into the
  per-tick skeleton. The current source and 2026-07-22 FSDB instead prove an
  immediate non-final D_OUT guard and contain no `update_finished_q`. The old
  eight fixture exports are retired; fresh packages are required before strict
  acceptance can resume.
- Step 5.5 Phase B adds an isolated `RtlSchedulerSkeleton` to the existing
  `schedule_state` source and test target. It models the CSR start register,
  registered scheduler instruction-valid path, core/instruction states,
  transpose/flow/instruction counters, input switch, current-RTL D_OUT guards,
  and write-finished completion with
  old-state/commit-at-edge semantics. It has no fixture, matrix-shape, golden
  cycle, or aggregate end-cycle input and is not yet connected to `SauModel`,
  so existing runtime output is unchanged. `RtlResidentLoadSkeleton` now also
  reproduces `register_addr.sv`'s x/y/channel loop and registered request
  valid/last signals. Its coupled scheduler test derives the baseline's 256
  request cycles and command-relative cycle-258 TRANSPOSE_LOAD edge directly
  from CSR extents. Phase C now also adds an isolated
  `RtlStreamLoadSkeleton` for `mem_addr.sv`'s streamed x/y/flow/instruction
  counters, registered valid/last, delayed retrigger, and combinational
  `load_done`. All eight current CSR snapshots share the explicitly supported
  x=1/y=32 vertical shape; other shapes are rejected until independently
  validated. Three streamed-load tests cover a 32-read single burst, the
  33-edge two-flow retrigger cadence, and unsupported-shape rejection. The
  developer-built schedule-state target passed all 13 tests on 2026-07-15.
  A fourteenth test couples the scheduler, resident counter, and streamed
  counter using one shared pre-edge snapshot and checks the baseline's first
  instruction state edges at relative cycles 2, 258, 290, 521, and 554. That
  coupled test passed together with the other 13 tests in the developer-built
  binary on 2026-07-15, closing the per-tick start-to-first-D_OUT control chain.
  The next isolated increment adds `RtlExecuteUpdateSkeleton`, which counts
  actual SA enable edges through the RTL's internal-finish and first-macro-row
  finish registers, then reproduces the update FSM distinction between
  non-final execute-done and final result-last. Three new focused tests cover
  enable bubbles and finish latency, final-result waiting, and unsupported
  control rejection. The developer-built target passed all 17 tests on
  2026-07-15. Feeder-derived `sa_en_i` and the result serializer remain
  intentionally outside this isolated producer.
  Coupling is currently paused on a precise observability gap: reconstructing
  `sa_en_i` from exported `input_switch_f` and A/B valid fields yields 246/247
  enable edges at the baseline's first D_OUT entry/exit, while RTL source
  requires 256 enabled counter increments. The 256th reconstructed edge is
  nine cycles after exit. No correction constant has been added. A targeted
  baseline trace of internal feeder enable, calc counter, execute-finish, and
  update signals is required before this producer is connected.
- The targeted 64x256x256 internal trace was captured on 2026-07-22 from the
  passing `/home/xch/work/npu_lpnpu/sim/vcs/build/yinglong/simv` artifact.
  Both commands show the same edge chain: `sa_en_i` becomes visible at
  relative edge 302, the SA counter first consumes it at 303, first `D_OUT`
  enters/exits at 554/555 with post-edge `calc_cnt=245/246`, the 256th accepted
  enable raises `internal_finish_pulse` at 565, and finish/update propagate at
  566/567/568. This resolves the apparent 246/247-versus-256 discrepancy
  without a correction constant; the feeder enable pipeline must preserve
  pre-edge sampling and can overlap scheduler progress. The trace also exposes
  a separate provenance conflict: this compiled RTL has no
  `scheduler_inst.update_finished_q`, uses an immediate non-final,
  non-shift/non-keep D_OUT guard when `flow_times_i != 1`, and differs in five
  audited source hashes from the 2026-07-15 snapshot. The user selected this
  passing snapshot as authoritative. `RtlSchedulerSkeleton` now uses its guard,
  and the new isolated `RtlSaEnableSkeleton` is coupled to the execute skeleton
  in a focused pre-edge/bubble test. Runtime remains unchanged pending compile
  verification and newly captured acceptance packages. Full edge/source
  details are recorded in `RTL_TIMING_PROVENANCE.md`.
- The first developer rebuild of the current-baseline schedule-state target
  exposed only a focused-test bookkeeping bug: the expected post-D_OUT
  `REUSE_LOAD=555` transition overwrote the first `REUSE_LOAD=290` observation.
  After retaining only the first observation, the developer rebuilt and all
  18 schedule-state tests passed on 2026-07-22, including the current-RTL
  554/555 scheduler overlap and feeder-to-execute pre-edge/bubble test.
- The next isolated increment adds `RtlResultSerializerSkeleton` for the fixed
  32x32 SA and 4x4 PE macros. It carries the internal-finish token through the
  eight-column macro delay, 32-row SA_ROW stream, storage/output-start
  registers, 32-beat output transposer, and final valid/last registers. The
  focused expectations reproduce current-FSDB edges 565/574/575/578/610/611/
  612/642/643/644 and exactly 32 result-valid cycles. The final-instruction
  execute test now consumes the serializer's result-last pulse and expects
  update-finished one edge later. The developer rebuilt the source target and
  all 20 focused tests passed on 2026-07-22. Runtime `SauModel` is still
  unchanged.
- The output/writeback trace from the same passing command confirms the RTL
  CSR extents `1x32x1x8` for result accumulation and `8x32x1` for output
  addresses. `RtlOutputWritebackSkeleton` now models the sticky accumulation
  completion, unload launch registers, 256-beat output address generator,
  two-stage valid/last path, and four-stage native write-finished path. The
  focused checks cover standalone drain timing and scheduler completion at
  current-FSDB edges 2507/2512/2767/2771/2772. The developer rebuilt the
  target and all 23 focused tests passed on 2026-07-22.
- `RtlSramWriteTransportSkeleton` now extends the isolated timing chain across
  `mem_ctrl`, ACTIVE `crossbar_mi`, and the shared-memory/TCDM sampling edge.
  The current FSDB proves the 2512/2513/2514/2515 first-edge sequence and the
  2767/2768/2769/2770 final-edge sequence, with no native ready or retry
  signal. Two new tests check 256-token conservation and IDLE-crossbar
  suppression. The developer rebuilt the target and all 25 focused tests
  passed on 2026-07-22.
- The resident-input trace proves that address completion precedes data drain:
  requests occupy edges 2..257, memory-visible data 7..262, feeder input-RF
  writes 8..263, and actual input-RF SRAM writes 9..264. Scheduler enters
  `TRANSPOSE_LOAD` at 258 while the delayed state pipe safely drains six tail
  writes. `RtlResidentFillSkeleton` now models this mem_ctrl/feeder/padding
  valid chain and is coupled to the resident address and scheduler skeletons
  in two new tests. The developer rebuilt the target and all 27 focused tests
  passed on 2026-07-22; runtime `SauModel` remains unchanged.
- `RtlInputFeederSkeleton` now models the current fixed ATB/reuse-A input-RF
  read FSM, feeder control delay, A shift/count path, B valid pipeline and
  final input-switch register. Three focused tests check the current-FSDB
  266/267/269/298/301/302/303 edges, the one-edge SA pre-edge sampling
  boundary and invalid configuration rejection. The developer rebuilt the
  target and all 30 focused tests passed on 2026-07-23.
- `RtlCommandDriverSkeleton` now connects the scheduler, address producers,
  shared memory-read visibility path, input feeder, SA-enable/execute path,
  result serializer, output writeback, and physical write transport with
  shared pre-edge sampling. Two new tests check the baseline edge chain,
  end-to-end token conservation, and inconsistent configuration rejection.
  The first developer rebuild passed 31/32: the end-to-end test caught
  resident tail data leaking into B at edge 266. Adding feeder.sv's registered
  `input_switch_case` corrected that edge, while the next run localized a
  second defect: a skeleton-only delayed core-state gate suppressed 41 B
  tokens across `D_OUT` boundaries and left the final SA calculation at
  224/256. RTL gates B with the persistent A/B arbiter instead, so that extra
  state gate was removed. The developer rebuilt the target and all 32 focused
  tests passed on 2026-07-23.
- The first strict-runtime integration increment adds a command-local
  `RtlCommandDriverSkeleton` to `SauModel`. It advances on the command's
  logical edge zero for both startup and replayed commands, drives semantic
  state/input-switch trace projection, gates completion with `commandDone`,
  and checks resident/stream/SA/result/physical-write conservation before
  teardown. A new focused test covers the current 1/4/8 flow and 1/4/8
  instruction combinations. The developer-built target passes 33/33 and the
  updated `sau_model.o` compiles. After relinking, the historical baseline
  strict simulation completed and both architecture and state comparators
  passed on 2026-07-23.
- The next runtime increment makes strict result production consume driver
  `resultValid` directly and makes write issue consume the registered
  mem_ctrl request valid/last at the observed 2513..2768 edges. The old
  `ResultScheduler` timing overload remains for non-strict DSE; a new
  externally-clocked release test covers its strict index-only use. Static
  checks pass. After developer compilation/relink, the strict baseline
  simulation and both comparators pass.
- The read/array runtime increment exposes the shared-SRAM request from the
  driver's existing pre-edge address snapshot: resident read requests are
  3..258 and streamed requests are 293..2417. The first focused rebuild caught
  and removed one redundant request register which had shifted all four
  boundaries by one edge; the corrected focused target passes 33/33. Strict
  `issueReads()` now uses those pulses while retaining
  `AddressGenerator` for address/index ownership and fixed-latency responses.
  A/B architecture admissions use driver A/B-valid while
  `ArrayInputScheduler` remains the index/conservation owner; its new external
  release API bypasses aggregate cooldown/skew only in strict mode. Non-strict
  behavior is unchanged. The corrected schedule-state target passes 33/33;
  after runtime relink the historical baseline strict simulation and both
  comparators pass.
- `RtlCommandDriverSkeleton` now records actual stage windows from its own
  per-tick signals. `SauModel` appends `actual_*_first_edge`,
  `actual_*_last_edge`, and `actual_*_span` rows for resident/stream reads,
  operands A/B, results, and mem_ctrl writes, followed by
  `actual_command_done_edge`. Baseline unit assertions cover the exact
  3..258, 293..2417, 269..2392, 301..2425, 612..2505, 2513..2768, and 2772
  boundaries. Static checks pass; developer compilation is pending.
- The non-strict direct-command model still assumes the target operator uses
  the RTL `register_file_in` path; strict fixture runs replay the supported
  CSR reuse/control fields instead.
- First milestone scope: direct command injection, int8 GEMM, and 32-byte
  timing-memory beats.
- The memory contract was corrected on 2026-07-03 from a legacy 128-bit
  assumption to the active RTL's 256-bit interface.

## Current RTL Golden Recapture — 2026-07-24

The current `npu_lpnpu` RTL and firmware flow has now produced all five
coverage and three hold-out Int8 GEMM packages requested by
`RTL_GOLDEN_PACKAGE_REQUEST.md`. The source-side package copies are preserved
at:

```text
/home/xch/work/npu_lpnpu/tmp/current_rtl_golden
```

The corresponding FSDB, sampled clock trace, firmware image, simulation log,
matmul comparison, and generated testcase header are preserved per fixture at:

```text
/home/xch/work/npu_lpnpu/tmp/rtl_golden_sources
```

Every RTL run reported `TEST PASSED` and zero matmul mismatches. Every package
passes `validate_fixture.py` and its local `SHA256SUMS`. Replaying each package
directly from the staging directory with the already-linked strict gem5 model
also passes both the full architecture comparator and the normalized state
comparator without a hold-out-specific timing change.

| Role | Fixture | Commands | Flows/command | Architecture rows |
| --- | --- | ---: | ---: | ---: |
| coverage | `int8_gemm_32x32x32_single_flow` | 1 | 1 | 295 |
| coverage | `int8_gemm_64x32x256_k_sweep` | 2 | 1 | 3,278 |
| coverage | `int8_gemm_64x256x32_n_sweep` | 2 | 8 | 3,214 |
| coverage | `int8_gemm_32x256x256_m_sweep` | 1 | 8 | 9,223 |
| coverage | `int8_gemm_64x256x256_baseline` | 2 | 8 | 18,446 |
| hold-out | `int8_gemm_96x256x256_m_holdout` | 3 | 8 | 27,669 |
| hold-out | `int8_gemm_64x128x256_k_holdout` | 2 | 4 | 9,742 |
| hold-out | `int8_gemm_64x256x128_n_holdout` | 2 | 8 | 9,742 |

The eight current packages are now imported under `tests/gem5/sau/ref` and
registered as RISC-V quick strict and timing-memory causal tests. The final
SAU quick run passes 63/63 checks across 23 suites, including strict
architecture/state verification, causal/backpressure verification, legacy
direct-command compatibility, fixed/constrained runs, and DSE simulations.

## Task Progress

| Task | Status | Notes |
| --- | --- | --- |
| 1. Capture deterministic RTL timing reference | Complete | All eight requested current-RTL packages are imported with their source artifacts preserved; package, hash, architecture, and state validation pass. |
| 2. Add the common trace comparator | Complete | Adds `util/sau/compare_trace.py` and unit tests. Strict mode compares every normalized field; causal mode compares event order and metadata while allowing latency shifts with nondecreasing actual cycles. |
| 3. Add command types and admission validation | Complete | Commit `b61ec79f60`; defines stable command/token types and validates the first-milestone contract. |
| 4. Implement deterministic beat generation | Complete / calibrated | Commit `6b3fc7165a`; later calibrated after Task 1 so external reads are A preload once per instruction, then B streaming per flow. |
| 5. Implement token buffers and array timing | Complete / calibrated boundary | Bounded token storage with configurable fill latency, initiation interval, and in-flight capacity. Added the A register-file-in abstraction boundary so A external reads and A array inputs are no longer conflated. |
| 6. Add the SimObject, parameters, trace, and stats skeleton | Complete | Registers `SauModel`, its timing-memory port and parameters, a stable seven-column trace writer, synthetic command startup, and statistics placeholders. |
| 7. Implement the timing memory port | Complete | Sends exact 32-byte read/write packets, retains one rejected packet for retry, tracks accepted outstanding traffic, and queues read responses for the next SAU edge. |
| 8. Integrate scheduling, array timing, writeback, and drain | Complete | `SauModel::tick()` now consumes read responses, feeds `ARegisterFileIn` plus B streaming tokens into the array pipeline, produces output tokens, issues timing writes, advances phases, checks token/request conservation, and drains only after local packet state is clear. |
| 8.5. Calibrate matmul transpose/reuse timing path | Complete | Adds `ArrayInputScheduler` burst/gap timing, `ResultScheduler`, reduced-output command validation, first-array-input anchoring, delayed writeback, and final completion delay. Defaults match the corrected 64x256x256 trace shape: A start 269, B skew 32, input burst 32, tile gap 1, flow gap 3, fill 343, result flow gap 234, writeback delay 8, completion delay 4. |
| 9. Add fixed- and constrained-memory simulations | Complete | Adds `configs/example/sau_timing.py` and `tests/gem5/sau/test_sau.py`. Fixed memory completes at SAU cycle 8477; constrained memory completes at SAU cycle 76617 with retry, outstanding-limit, and input-starvation stalls. |
| 10. Calibrate against the RTL reference | Complete | Fixed-cadence calibration strictly matches all 18,446 RTL rows and seven CSV fields for both commands; the constrained timing-memory profile also passes causal dataflow/dependency validation under retry and backpressure. |
| 11. Final regression, statistics audit, and documentation | Complete | Statistics, README, strict/causal regression, and DSE monotonicity passed. Commit `50d42ef51c` is pushed to `sau-origin/feature/sau-command-types`. |
| PLAN2. CSR-driven Int8 GEMM RTL alignment | Complete; committed and pushed at `458ad7e3cb` | All five current coverage and three hold-out packages pass strict and timing-memory causal verification. DSE monotonicity and legacy direct-command causal compatibility pass; the full 23-suite SAU quick run passes 63/63 checks. |
| PLAN3. CSR-driven functional datapath | Steps 0–3 complete for current Reuse-A support domain; Step 4 next | Step 0 froze the RTL/CSR/golden contract; Step 1 landed the functional memory/payload contract; Step 2 landed full-domain decode, typed resource dispatch, and raw-counter address programs. Step 3 landed input/transpose payload resources, runtime integration, statistics, and generic boundary comparison: ABTD closes the first non-default B -> B^T milestone, while deep ATBD data_A/data_B and T0/T1 inRow/outCol match RTL payloads and exact cycles. R-none/R-B/R-AB remain decoded with existing code preserved, but are deferred while RTL evolves. |

## Historical PLAN2 RTL Fixture Inventory

The previous package versions are retained in Git history as historical
diagnostics, not current golden oracles. They used the fixed PLAN2 control
contract: int8 GEMM,
`trans_mode=01`, `reuse_mode=01`.

| Role | Fixture | M × K × N |
| --- | --- | --- |
| Coverage | `int8_gemm_32x32x32_single_flow` | 32 × 32 × 32 |
| Coverage | `int8_gemm_32x256x256_m_sweep` | 32 × 256 × 256 |
| Coverage | `int8_gemm_64x32x256_k_sweep` | 64 × 32 × 256 |
| Coverage | `int8_gemm_64x256x32_n_sweep` | 64 × 256 × 32 |
| Coverage | `int8_gemm_64x256x256_baseline` | 64 × 256 × 256 |
| Hold-out | `int8_gemm_96x256x256_m_holdout` | 96 × 256 × 256 |
| Hold-out | `int8_gemm_64x128x256_k_holdout` | 64 × 128 × 256 |
| Hold-out | `int8_gemm_64x256x128_n_holdout` | 64 × 256 × 128 |

Package validation checked SHA256 integrity, CSR/snapshot correspondence,
command accept/complete closure, and event-count consistency. These fixtures
provided historical evidence for their retired RTL only; they provide no
timing-acceptance evidence for the current baseline.

## Implemented Components

### Command boundary

- `SauCommand`, stream descriptors, beats, pipeline tokens, phases, and event
  kinds.
- Admission validation for:
  - int8 GEMM and 32-byte beats;
  - aligned stream bases;
  - nonzero beats, strides, loops, and work items;
  - output beats not exceeding work items, allowing GEMM reduction where many
    array-input work tokens produce fewer result/write beats; and
  - checked multiplication before narrowing.

### Address generation

- Cursor-based generator; it does not allocate a vector for all beats.
- Deterministic external SRAM read order for every instruction:

  ```text
  preload all Operand-A beats once -> stream all Operand-B beats per flow
  ```

- Address formula:

  ```text
  base
  + instruction * instructionStrideBytes
  + flow * flowStrideBytes
  + beat * strideBytes
  ```

  For Operand-A external preload, `flow` is treated as zero because A reuse is
  internal to the SAU after it is loaded into the modeled register file.

- `Beat.index` resets for each stream occurrence.
- `Beat.last` marks the final beat of each stream occurrence.
- The accepted command is copied into the generator, so later caller-side
  changes cannot alter an active sequence.

### A register-file-in reuse path

- `ARegisterFileIn` records Operand-A preload beats and reports when a command
  instruction's A data is resident.
- It models RTL reuse at the architecture-relevant timing boundary: in this
  RTL, reuse means the operand is supplied through `register_file_in`.
- It does not model RTL register width, ports, banks, or stored data values.
- It exposes virtual Operand-A array-input beats after preload. These beats use
  address zero because array input trace events are not external SRAM accesses.
- This fixes the old implicit assumption that every A array input required a
  fresh external SRAM read.
- The scheduler now pairs B read responses with reused A tokens at the array
  input boundary.

### Token buffering and array timing

- Capacity-limited FIFO token storage with explicit push availability.
- Configurable array fill latency, initiation interval, and maximum in-flight
  token count.
- No hidden cycle counter; the owner supplies the current SAU cycle.
- Ready tokens retain in-flight capacity until consumed.
- Timing values remain configurable pending RTL calibration.

### SimObject, trace, and statistics

- `SauModel` is a `ClockedObject` with a timing-memory request port and the
  configuration parameters required by the first milestone.
- Startup validates and accepts one synthetic int8 GEMM command, emits stable
  trace events, and schedules cycle ticks.
- `tick()` now integrates memory response consumption, operand readiness,
  array admission, result production, writeback issue, phase progression, and
  command completion.
- The CSV trace has the fixed schema
  `cycle,event,command_id,stream,address,beat,phase`, is disabled by an empty
  path, and flushes on `command_complete`.
- Statistics are registered for commands, phase cycles, traffic, occupancy,
  array utilization, and each planned stall reason.

### Timing-memory port

- Creates real gem5 `ReadReq` and `WriteReq` packets with the SAU requestor ID
  and exact 32-byte payload size.
- Zero-fills write payloads and discards returned read data after preserving
  beat metadata.
- Retains ownership of one rejected packet until `recvReqRetry()` accepts it;
  outstanding counts increase only after acceptance.
- Tracks read and write outstanding requests independently and releases each
  count only on its matching response.
- Read responses are queued by the port and become visible only when
  `SauModel::tick()` runs at the next SAU edge.
- Accepted requests and visible responses update trace and traffic statistics.

### Standalone simulation

- `configs/example/sau_timing.py` instantiates a timing-mode system with a
  1GHz default SAU clock, `SystemXBar(width=32)`, `SimpleMemory`, and one or
  more synthetic `SauModel` commands.
- The CLI exposes memory latency, latency variance, bandwidth, command stream
  shape, buffer sizes, array capacity/timing, outstanding limits, and trace
  path.
- The default command shape matches the corrected baseline size at the model
  boundary: 256 A beats, 256 B beats, 8 flow loops, one instruction, and 256
  output beats.
- Fixed-memory and constrained-memory runs now both exit via
  `SAU command complete` and write a seven-column trace.
- `tests/gem5/sau/test_sau.py` registers RISCV quick tests for the fixed and
  constrained standalone configurations.
- The constrained run uses bandwidth/outstanding pressure but keeps
  `output_buffer_entries` at 256 because the current timing policy delays
  writeback until the complete result stream has been produced.
- Task 10 front-half profile controls add command count, inter-command gap,
  and per-command A/B/output address strides. `--rtl-profile` sets these to
  the imported 64x256x256 RTL baseline:
  two commands, A stride `0x2000`, B stride `0x0`, output stride `0x2000`,
  command gap 353 cycles, RTL base addresses, and 1GiB memory size.
- Task 10 calibration controls add `--calibration-memory` plus
  `--calibration-read-latency-cycles`. In calibration mode, SAU read and write
  accepted events are generated locally at one beat per SAU cycle, and read
  responses become visible after the fixed calibration latency. This mode is
  for RTL profile comparison only; the default standalone path still sends
  real timing packets through `SystemXBar + SimpleMemory`.

### Task 8 scheduler integration

- `SauModel::tick()` now advances the model in the fixed order:

  ```text
  consumeResponses
  -> advanceArray
  -> produceResults
  -> issueWrites
  -> issueReads
  -> updatePhase
  -> accountCycle
  ```

- Operand-A read responses preload `ARegisterFileIn`; Operand-B read responses
  enter the B availability queue.
- Array input admission uses `ArrayInputScheduler` instead of same-cycle A/B
  pairing. Resident A array-input beats may run ahead of B by
  `array_input_skew_cycles` tokens; when both A and B are emitted in the same
  SAU cycle, trace order remains B then A to match the RTL trace.
- Task 8.5 extends that scheduler with the corrected matmul burst shape:
  `array_input_start_delay_cycles=269`,
  `array_input_burst_beats=32`, `array_input_burst_gap_cycles=1`, and
  `array_input_flow_gap_cycles=3`.
- B array-input admission requires one available B beat and drives the
  work/admission count plus `ArrayDrain` boundary. A array-input admission
  requires resident `register_file_in` data and feeds the modeled array
  latency path.
- `ResultScheduler` models the reduced GEMM output stream: the default
  64x256x256 shape has 2048 A/B array-input beats per command but only 256
  result/write beats. Results are emitted as 32-beat flow bursts after
  `array_fill_cycles=343`, with `result_flow_gap_cycles=234` idle cycles
  between bursts.
- Writeback is delayed until all results are produced and then waits
  `writeback_start_delay_cycles=8`; command completion waits
  `completion_delay_cycles=4` after the final accepted write.
- Result write addresses use the synthetic output stream address formula.
- `SauMemoryPort::trySend()` owns a packet once called. If a send is rejected,
  the port retains the single blocked packet for retry, so the scheduler pops
  the corresponding generator/output token immediately and counts acceptance
  only in `requestAccepted()`.
- Phase progression now covers `OperandLoad`, `ArrayActive`, `ArrayDrain`,
  `Writeback`, and `Complete`; `exit_on_done` is effective when command
  completion is reached.
- `drain()` returns `Draining` while the command, blocked packet, or outstanding
  requests are still live, and reports drained only after local packet state is
  clear.

### RTL timing baseline

- The RTL-side Task 1 package was received as
  `/home/xch/workspace/sau_task1_baseline.tar.gz` and refreshed from the
  corrected package unpacked under
  `/home/xch/workspace/sau_task1_baseline_unpack_20260707_real/sau_task1_baseline`.
- Full package hashes in `SHA256SUMS` were checked successfully after mapping
  the original RTL repository paths to the local unpacked package layout.
- The public architecture trace, manifest, summary, diagnostic trace, analysis,
  handoff note, and checksum file were copied into
  `tests/gem5/sau/ref/int8_gemm_64x256x256/`.
- Important measured timing values for this 64x256x256 baseline:
  - command total: 5897 cycles;
  - first read: cycle 3;
  - first array input: cycle 269;
  - first result: cycle 612;
  - last array input: cycle 5550;
  - last result: cycle 5630;
  - first write: cycle 2513;
  - command complete: cycle 5897;
  - array fill latency: 343 cycles;
  - array drain latency: 80 cycles;
  - last result to first write: -3117 cycles;
  - last write to complete: 4 cycles.
- The corrected trace contains two commands. Each command has the same
  diagnostic state profile:

  ```text
  REGISTER_LOAD    256 cycles
  TRANSPOSE_LOAD    32 cycles
  REUSE_LOAD      1854 cycles
  TRANSPOSE_CLIP   231 cycles
  D_OUT             99 cycles
  FIRST_LOAD        33 cycles
  REGISTER_UNLOAD  265 cycles
  ```

- `TRANSPOSE_LOAD` and `TRANSPOSE_CLIP` are therefore part of the active matmul
  timing path, not a low-priority future extension. The public trace also shows
  A and B array-input windows are skewed by about 32 cycles:

  ```text
  command 1: A 269..2392, B 301..2425
  command 2: A 3394..5517, B 3426..5550
  ```

- The original task asked for 32x32x32, but the available RTL testcase is
  64x256x256 and was accepted as the current baseline. These absolute cycle
  counts must not be treated as a 32x32x32 calibration.

### Trace comparator

- `util/sau/compare_trace.py` reads the public seven-column SAU architecture
  trace and normalizes cycles to each trace's first `command_accepted` row.
  Multiple commands are legal and are distinguished by `command_id`.
- `strict` mode compares every normalized field and reports every mismatch.
- `causal` mode compares the event/stream/address/beat/phase sequence and
  requires nondecreasing actual cycles, while ignoring absolute latency
  differences.
- The CLI is:

  ```bash
  python3 util/sau/compare_trace.py --mode strict EXPECTED.csv ACTUAL.csv
  ```

- Exit status is `0` for a match and `1` after printing mismatches or format
  errors.

## Verification

Most recent focused verification:

```bash
python3 -m py_compile configs/example/sau_timing.py \
    tests/gem5/sau/test_sau.py

git diff --check

python3 util/style.py --modifications \
    src/sau/SConscript \
    src/sau/memory_port.hh \
    src/sau/memory_port.cc \
    src/sau/memory_port.test.cc \
    src/sau/sau_model.hh \
    src/sau/sau_model.cc

scons build/RISCV/gem5.opt -j4

./build/RISCV/gem5.opt \
    --outdir=m5out/sau-rtl-match \
    configs/example/sau_timing.py \
    --rtl-profile \
    --trace=m5out/sau-rtl-match/sau.csv

./build/RISCV/gem5.opt \
    --outdir=m5out/sau-rtl-calibration \
    configs/example/sau_timing.py \
    --rtl-profile \
    --calibration-memory \
    --trace=m5out/sau-rtl-calibration/sau.csv

python3 util/sau/compare_trace.py --mode causal \
    tests/gem5/sau/ref/int8_gemm_64x256x256/architecture.csv \
    m5out/sau-rtl-calibration/sau.csv

./build/RISCV/gem5.opt \
    --outdir=m5out/sau-fixed \
    configs/example/sau_timing.py \
    --memory-latency=3ns \
    --memory-latency-var=0ns \
    --memory-bandwidth=256GiB/s \
    --trace=m5out/sau-fixed/sau.csv

./build/RISCV/gem5.opt \
    --outdir=m5out/sau-constrained \
    configs/example/sau_timing.py \
    --memory-latency=20ns \
    --memory-latency-var=5ns \
    --memory-bandwidth=1GiB/s \
    --trace=m5out/sau-constrained/sau.csv \
    --max-outstanding-reads=2 \
    --max-outstanding-writes=2

cd tests
./main.py run --skip-build gem5/sau
cd ..

./build/RISCV/sau/address_generator.test.opt
./build/RISCV/sau/a_register_file.test.opt
./build/RISCV/sau/command.test.opt
./build/RISCV/sau/memory_port.test.opt
./build/RISCV/sau/token_pipeline.test.opt
./build/RISCV/sau/trace_writer.test.opt

python3 -m unittest util.sau.compare_trace_test -v

python3 util/sau/compare_trace.py --mode strict \
    tests/gem5/sau/ref/int8_gemm_64x256x256/architecture.csv \
    tests/gem5/sau/ref/int8_gemm_64x256x256/architecture.csv
```

Results:

- `configs/example/sau_timing.py` and `tests/gem5/sau/test_sau.py`
  `py_compile`: passed after the Task 10 profile controls were added.
- `git diff --check`: passed.
- `build/RISCV/gem5.opt`: rebuilt successfully after the Task 10 profile
  controls, multi-command scheduling changes, and calibration-memory mode.
- `--rtl-profile` standalone run completed with `SAU command complete`, wrote
  `m5out/sau-rtl-match/sau.csv`, and generated 18446 data rows plus header,
  matching the imported RTL architecture trace row count.
- `m5out/sau-rtl-match/sau.csv` now matches the RTL profile structurally:
  commands 1 and 2 are both present, each command has 9223 rows, per-command
  event counts match, and A/B/output read/write address ranges match the RTL
  baseline.
- Causal comparison against
  `tests/gem5/sau/ref/int8_gemm_64x256x256/architecture.csv` now passes for
  `m5out/sau-rtl-calibration/sau.csv`. The original row 6 mismatch between
  `read_accepted` and `read_response_visible` ordering was fixed on
  2026-07-09 by issuing reads before taking visible responses in
  `SauModel::tick()`. A later row 510 mismatch was fixed by adding
  `b_read_start_ahead_beats=24` to `--rtl-profile`, so Operand-B external
  reads now wait for all Operand-A preload responses and the initial A
  array-input lead window. `b_stride_bytes=0x100` plus
  `b_flow_stride=0x20` now matches the RTL Operand-B read address order.
  `--calibration-memory` fixes the row 541 B read accepted cadence mismatch:
  B reads are accepted at cycles 293, 294, 295... in the calibration trace.
  The row 562 same-cycle B/A array-input mismatch was fixed by letting
  Operand-A enter the array pipeline as an additional same-cycle input that
  does not consume the pipeline initiation interval. Later B-read burst-gap
  and result-ordering mismatches were fixed by applying the 32-beat
  tile/flow gaps to calibration B reads and letting calibration result trace
  emission follow `ResultScheduler` directly.
- On 2026-07-10, after rebuilding with `scons --ignore-style`, the RTL
  calibration run completed with `SAU command complete`. Both strict and
  causal comparison against `architecture.csv` passed. The reference and
  generated traces each contain 18,446 data rows; command 1 first read and
  completion are 3 and 2772, while command 2 first read and completion are
  3128 and 5897.
- On 2026-07-10, the constrained real timing-memory RTL profile completed
  with `SAU command complete` and passed the updated causal comparator. It
  retained all 18,446 event rows and recorded `stallRequestRetry=5114`,
  `stallOutstandingReadLimit=19199`, `stallOutstandingWriteLimit=2652`, and
  `stallInputStarvation=132505`.
- Fixed standalone run completed with `SAU command complete`, wrote
  `m5out/sau-fixed/sau.csv`, and completed at SAU cycle 8477.
- Constrained standalone run completed with `SAU command complete`, wrote
  `m5out/sau-constrained/sau.csv`, and completed at SAU cycle 76617.
- Constrained standalone stats showed real pressure:
  `stallRequestRetry=2559`, `stallOutstandingReadLimit=9125`,
  `stallOutstandingWriteLimit=127`, and `stallInputStarvation=71833`.
- `cd tests && ./main.py run --skip-build gem5/sau`: 4/4 checks passed for
  the RISCV fixed and constrained quick tests.
- After the 2026-07-09 read-ordering change, `scons build/RISCV/gem5.opt -j4`
  rebuilt successfully, `--rtl-profile` completed with
  `SAU command complete`, and `cd tests && ./main.py run --skip-build
  gem5/sau` still passed 4/4.
- After the 2026-07-09 B-start and B-address-order changes,
  `scons build/RISCV/gem5.opt -j4` rebuilt successfully, `--rtl-profile`
  completed with `SAU command complete`, the first 12 Operand-B read addresses
  matched the RTL sequence, and `cd tests && ./main.py run --skip-build
  gem5/sau` passed 4/4.
- After the 2026-07-09 calibration-memory change,
  `scons build/RISCV/gem5.opt -j4` rebuilt successfully,
  `--rtl-profile --calibration-memory` completed with `SAU command complete`,
  the generated trace kept the same 18447 total lines as the RTL reference,
  and causal comparison progressed from row 541 to row 562.
- After the 2026-07-09 same-cycle A/B input, B-read burst-gap, and
  calibration result-schedule changes, `scons build/RISCV/gem5.opt -j4`
  rebuilt successfully, `--rtl-profile --calibration-memory` completed with
  `SAU command complete`, causal comparison passed, strict comparison failed
  only on cycles, `./build/RISCV/sau/token_pipeline.test.opt` passed 6/6, and
  `cd tests && ./main.py run --skip-build gem5/sau` passed 4/4.
- SAU trace comparator tests: 8/8 passed.
- SAU trace comparator strict and causal self-comparison against the imported
  RTL reference trace passed.
- SAU trace comparator strict mode reported the expected cycle mismatch for a
  temporary trace with one `read_accepted` row shifted by one cycle; causal
  mode accepted the same temporary trace because event order and metadata were
  unchanged.
- `py_compile` for `util/sau/compare_trace.py` and
  `util/sau/compare_trace_test.py`: passed.
- A register-file-in tests: 4/4 passed, covering preload completion, A reuse
  count greater than external A reads, virtual A array-input beats, and
  rejection of non-A loads.
- RTL baseline package `SHA256SUMS`: all listed files passed.
- RTL baseline validator CLI regenerated `summary.json`; the regenerated file
  was byte-identical to the package `summary.json`.
- RTL baseline validator unit tests: 27/27 passed when run from a temporary
  `/tmp` package layout matching the original RTL repository import path.
- gem5 style check: passed.
- Address generator tests: 3/3 passed.
- Command validation tests: 14/14 passed, including rejection of the legacy
  16-byte beat size.
- Token buffer and array pipeline tests: 5/5 passed.
- Trace writer tests: 2/2 passed.
- Timing-memory port tests: 3/3 passed, covering exact packet metadata,
  request rejection/retry, outstanding accounting, response ownership, and
  zero-filled writes.
- Latest developer-built focused SAU tests: 33/33 passed.
- `build/RISCV/gem5.opt`: built successfully.
- After the strict-calibration boundary fix, `scons
  build/RISCV/sau/sau_model.o build/RISCV/sau/token_pipeline.test.opt -j4`
  compiled the affected model object and test target; the token-pipeline
  suite passed 7/7, including the new command-local epoch reset test. A full
  gem5 relink was not run because this worktree's build requests interactive
  installation of Git hooks, which was not authorized.
- `build/RISCV/params/SauModel.hh` was generated in this worktree, confirming
  SimObject parameter registration.
- Build warnings about unavailable Capstone and HDF5 are unrelated to the SAU
  unit tests.

## Known Gaps and Risks

- The imported RTL reference is for 64x256x256, not 32x32x32. It is usable for
  schema, event ordering, and the currently accepted baseline case, but its
  absolute cycle counts must not be generalized to other dimensions without
  additional RTL traces.
- The corrected package `SHA256SUMS` records paths from the remote RTL
  repository layout, not the local unpack directory. Local verification requires
  remapping `sim/vcs/build/sau_regress/baseline/int8_gemm_32x32/` to the package
  root and `testcase/sau_regress_matmul/INT8_SAU_MATMUL_TEST_ID_0/` to
  `testcase_INT8_SAU_MATMUL_TEST_ID_0/`.
- The address generator assumes the command has already passed admission
  validation.
- Task 8 did not add a direct `SauModel` unit test because constructing the
  SimObject plus timing-memory topology in a focused C++ test would duplicate a
  large part of the upcoming standalone gem5 configuration. End-to-end
  scheduler validation is now covered by the Task 9 standalone runs.
- Task 8.5 represents the corrected matmul trace shape with configurable
  timing-policy knobs, but it is still an architectural cycle-level model; it
  does not reproduce RTL internal state encodings, register ports, transpose
  datapath storage, or arithmetic values.
- Causal and strict comparison of the Task 10
  `--rtl-profile --calibration-memory` trace against
  `tests/gem5/sau/ref/int8_gemm_64x256x256/architecture.csv` now passes. The
  imported two-command 64x256x256 baseline is cycle-aligned.
- The causal comparator now validates command-local event lanes and explicit
  dataflow dependencies rather than requiring one fixed global CSV event
  interleaving. This keeps the fixed-memory strict contract unchanged while
  making constrained timing-memory validation meaningful.
- The first observed Task 10 mismatch was a sequence mismatch after profile
  alignment, not a matrix-size or arithmetic mismatch. RTL records Operand-A
  beat 4 `read_accepted` before Operand-A beat 0 `read_response_visible`;
  old gem5 emitted visible read responses before issuing new reads in
  `SauModel::tick()`, so the same logical events appeared in a different CSV
  order. This local ordering issue is now fixed.
- The current first mismatch is the next timing boundary: gem5 starts
  Operand-B external reads before the final Operand-A preload responses have
  become visible. RTL drains the Operand-A visible responses first, enters
  `array_active`, and only starts Operand-B external reads after the
  Operand-A array-input lead window has begun. This boundary has now been
  encoded for `--rtl-profile` with `b_read_start_ahead_beats=24`.
- The row 541 B read cadence mismatch is fixed only in calibration mode.
  `--rtl-profile --calibration-memory` bypasses `SystemXBar + SimpleMemory`
  so RTL comparison can use the imported fixed SRAM cadence. The generic
  timing-memory path intentionally remains different and should be used for
  system/backpressure experiments, not strict RTL alignment.
- Same-cycle A/B array-input ordering now matches RTL in calibration mode.
  Operand-B consumes the modeled work-admission initiation slot; Operand-A can
  enter as an additional same-cycle pipeline input without advancing the next
  initiation slot. This is covered by the new token-pipeline unit test.
- Strict RTL alignment is now verified for the imported two-command
  64x256x256 baseline. The calibrated `command_start_cycles=3` and
  command-local `ArrayPipeline` reset are baseline-specific timing-policy
  values and must be revalidated with additional RTL traces before being
  generalized to other matrix shapes.
- DSE configurations with an output FIFO smaller than a command's complete
  result stream use streaming writeback so the FIFO can drain. This behavior
  is deliberately outside the calibrated strict profile and outside the
  current causal comparator's all-results-before-writeback dependency.
- `pre-commit` and `clang-format` are not installed in the current
  environment. `git diff --check` passed; no formatting dependency was
  installed.
- Current tests establish deterministic component behavior, trace comparison
  behavior, compile-time integration of the scheduler, standalone completion
  under fixed/constrained memory, and strict RTL cycle alignment for five
  coverage plus three independent M/K/N hold-out shapes. This supports the
  tested CSR/control domain but does not claim every unobserved RTL-accepted
  configuration.

## Next Steps

### PLAN.md checklist reconciliation — 2026-07-24

The original `PLAN.md` checklist has been reconciled against this status
record, the current source/tests, and milestone commits. Tasks 2 through 11
are now marked complete, including the previously stale Task 8.5 standalone
comparison/commit, Task 10 causal-profile checkpoint, and Task 11 milestone
commit. Task 12 steps 1 through 4 are marked complete because PLAN2 delivered
the CSR field model, config/command boundary, baseline decode, and focused
tests.

Unchecked items are intentional: the original Task 1 `+sau_trace` testbench
instrumentation and `timing_trace` Make target were superseded by the
FSDB/golden-package capture path and are not present in the current RTL
repository; historical red-test observations cannot be reconstructed from
the final tree; live CPU/CSR bus integration remains deferred. These do not
invalidate the completed timing milestone.

### PLAN3 functional-data stage — 2026-07-24

Requirements for the next stage are captured in `PLAN3.md`. The stage adds a
JSON-selected CSR and memory-image workload, real read/write payloads, and an
RTL-structured int8 GEMM datapath through input storage, transpose/reuse,
32x32 PE accumulation, result serialization, output accumulation/saturation,
and persistent memory writeback.

Unlike PLAN2's accepted timing domain, the PLAN3 target is not fixed to
`trans_mode=01, reuse_mode=01`: all four RTL GEMM transpose modes and the
three scheduler-defined reuse modes must be selected by CSR. Other CSR fields
that affect int8 GEMM are likewise functional controls rather than fixture
constants. In particular, the full 5-bit `cutbit=0..31` domain must drive the
RTL-equivalent signed arithmetic shift and int8 saturation; the current
fixture's `cutbit=8` is only one validation point. CPU integration, int16,
convolution, standalone transpose, and matrix addition remain out of scope.
Step 0 completed on 2026-07-24 and Step 1 on 2026-07-26; see the dated
sections below.

PLAN3 was reviewed and revised before implementation. The authoritative
datapath is now the currently instantiated
`SA_CORE -> sa_element/feeder + register_file_in ->
sa_execute/sa_feeder -> transposer_tiny -> SA_ENGINE -> SA_ROW ->
SA_PE_array -> SA_PE -> register_file_out` hierarchy; retained legacy
`trans2sa_top/SA_TOP/SA_row_unit/SA_pe` sources are explicitly excluded.
The plan now models the current PE's 24-bit saturating accumulator rather than
wraparound, separates the output register file's 16-bit arithmetic boundary,
and requires a pre-implementation Step 0 to freeze the CSR legal-domain table,
write payloads, byte order, and final-memory golden. Strict fixed-SRAM and
timing-memory runs each have one explicit data authority. New transpose/reuse
and flow modes must first extend the resource model and pass the selected
per-tick boundary oracle checks before their payload behavior can be promoted.

A second modeling-level review clarified that PLAN3 is not a line-by-line RTL
translation. The target is a CSR-driven, resource-constrained, data-bit-exact
cycle model. It must preserve finite capacity, port contention, throughput,
latency, backpressure, retry/outstanding behavior, state lifetime, transaction
ordering, payload, and strict boundary timing, while allowing RTL-internal
wires and architecturally invisible registers to be combined. Step 0 now
freezes a resource abstraction contract and state-lifetime table. The existing
`RtlCommandDriverSkeleton` is a strict regression oracle for boundary events,
not a second functional execution state machine; accepted transactions advance
control metadata and payload together in the new resource model.

The follow-up convergence pass makes raw CSR writes the only executable
configuration semantics: CSV references and optional inline JSON records both
normalize to one `SauCsrWrite` replay/decode path, while `csr_snapshot.json` is
cross-check data only. New transpose/reuse/flow modes compare the resource
model directly against RTL boundary traces through a generic comparator;
`RtlCommandDriverSkeleton` remains an `01/01` regression and is not expanded
into a second multi-mode controller. PLAN3 also requires sparse memory backing
for the 1-GiB address range, explicit 256/8/24/16-bit data conversion
boundaries, and RTL-derived mode interaction equivalence classes.

The clarified implementation target is broader than adding one transpose
special case. PLAN3 now brings the complete legal int8-GEMM CSR domain into
the typed resource-dispatch framework before datapath bring-up. The existing
`01/01` fixture remains a regression and the first full end-to-end golden, but
is no longer an implementation gate. Resources are implemented and validated
in RTL order: input/register storage, transpose/reuse, systolic-array fixed
point behavior, serializer/output storage, then multi-command integration.
The first non-default milestone is `trans_mode=2 (ABTD)`: real B payload must
pass through the finite transposer resource and emerge in RTL-equivalent
transposed order, with bank occupancy, stalls, latency, and cycles accounted
even before downstream end-to-end validation is complete. RTL legality and
model validation maturity are tracked separately.

The latest review makes path coverage derive from the RTL datapath rather than
from brute-force CSR enumeration. Step 0 now records each elaborated RTL
guard/mux/case path, the CSR fields selecting it, selected finite resources,
port/capacity effects, and observable boundaries. Every legal CSR combination
must map to a known path, while tests cover each structural path, field
boundary, key interaction, and independent legal hold-out instead of all
numeric Cartesian products. Architecturally visible bank/port conflicts remain
in scope; only bitcell/analog implementation details are excluded.

PLAN3 also explicitly tests a legal but unintended configuration: identical
memory and CSR except `trans_mode=1` versus `trans_mode=2`. gem5 must not infer
or correct user intent; each case independently matches the result, write
payload, boundary cycles, and command-done cycle of RTL using the same CSR.
Strict fixed-SRAM requires exact total cycles under the same memory
request-accept/response schedule. Timing-memory may add cycles, but every
difference must be attributable to observable memory latency, retry,
queue/outstanding stalls, or propagated backpressure.

The final pre-implementation clarification closes five execution ambiguities
without changing PLAN3 scope. Sequential workloads now reject a start that
arrives before the active command completes and its writes are visible,
at preflight when determinable or at the conflicting cycle under dynamic
timing-memory stalls.
Dedicated busy/start tests may replay overlapping raw writes, but gem5 must
match RTL accepted/ignored behavior and never queue an unaccepted start for
later execution. `rtl_legal_unimplemented` is distinct from RTL-illegal:
complete workloads fail before producing results or trusted performance, while
module bring-up may report only the boundary maturity it has actually reached.

Each workload/golden is bound to the RTL commit and function/timing-affecting
elaboration parameters, including `ROW_NUM/COL_NUM/OUTPUTDW/SRAM_DELAY` and
clock period, and is rejected on mismatch. A datapath path may be implemented
only after its own RTL boundary golden is available; unrelated future paths do
not block progress. Timing-memory retry/late-response behavior is resource
local rather than a global freeze: blocked packets remain stable, address
counters advance only on acceptance, independent buffered work continues, and
dependent stalls propagate through real backpressure. Diagnostic reasons may
overlap, but total stall cycles use one frozen primary-cause priority.

The frozen PLAN3 JSON example was corrected to the current fixture contract:
`SRAM_DELAY=3`. The workload schema now also carries an explicit
`"manifest": "manifest.json"` path so the required RTL commit/elaboration
preflight has a defined golden-manifest source.

### PLAN3 Step 0 complete — 2026-07-24

The pre-implementation RTL/data contract is frozen in `PLAN3_STEP0.md`.
It records the active hierarchy and elaboration parameters, finite-resource
contract, state lifetime, compositional transpose/reuse/flow path table, CSR
raw support domain, stall attribution order, and golden readiness. In
particular, a real VCS run establishes that raw `reuse_mode=11` is executable:
both reuse bits are asserted, the command completes, and all 1024 result bytes
match the software reference.

Four self-checking packages were added under
`tests/gem5/sau/functional_ref`: ATBD/reuse-A at cutbit 8, the same path at
cutbit 1, an ABTD B/transposer boundary fixture, and the reuse=11 probe. Each
package contains the pre-simulation memory image, NPI-extracted boundary
changes, testbench compare, address-ordered final output bytes, simulation log,
RTL/simulator manifest, and SHA-256 inventory. The two cutbit cases and
reuse=11 are 1024/1024 end-to-end matches. The ABTD image intentionally retains
ATBD software layout, so its 1005 mismatches are recorded and explicitly not
used as a mathematical oracle; its B/transposer and output boundaries are the
golden. `util/sau/build_plan3_step0_package.py` reproduces the package layout.
No gem5 model behavior was changed.

### Timing-memory causal checkpoint — 2026-07-24

The first post-Step-5.5 item is complete. The standalone
configuration now separates CSR fixture replay from strict fixed-SRAM timing:
`--rtl-profile FIXTURE --timing-memory` replays the fixture-derived commands
and `TimingPolicy` through `SystemXBar + SimpleMemory`. The eight supported
fixtures are registered as causal timing-memory suites. Their verifier checks
the architecture trace with the causal comparator, exact command/read/write
token counts against the RTL package, configured outstanding limits, and the
presence of real retry/outstanding/starvation pressure.

Static verification passed:

```text
python3 -m py_compile configs/example/sau_timing.py tests/gem5/sau/test_sau.py
python3 -m unittest util.sau.compare_trace_test \
    util.sau.verify_dse_test util.sau.validate_fixture_test -v
python3 util/style.py --modifications \
    src/sau/sau_model.hh src/sau/sau_model.cc
git diff --check
```

The Python suites passed 14/14. Per `src/sau/AGENTS.md`, Codex did not compile
gem5; the developer incrementally rebuilt `build/RISCV/gem5.opt`.

The smallest 32x32x32 run completed with one command, 64 reads, 32 writes,
maximum outstanding read/write counts of 2, and nonzero retry, outstanding
limit, and input-starvation stalls. All eight supported fixtures then passed
causal comparison and the conservation/pressure verifier. The K-sweep exposed
that a data event can observe a different phase snapshot when B backpressure
changes event interleaving; the causal comparator now preserves the exact
phase-transition and command-boundary phase sequence while allowing that
legitimate data-event snapshot change. Positive and negative comparator tests
cover this distinction.

The initial complete SAU quick run passed **60/60 checks across 22 suites**:
eight strict fixture suites, eight causal timing-memory suites,
fixed/constrained direct-command runs, and four DSE simulations.
Comparator/DSE/fixture Python unit tests passed 16/16.

The formal post-Step-5.5 DSE verifier also passes. Increasing array capacity
from 1 to 16 reduced `commandCycles` from 174 to 137 and removed 62
array-capacity stall cycles. Increasing output FIFO entries from 1 to 8 did
not increase the 668 command cycles; both configurations retained the expected
output-full pressure.

The historical two-command 64x256x256 direct-command parameters were recovered
from milestone commit `50d42ef51c` and rerun through calibration memory. The
trace contains the same 18,446 data rows as the current CSR baseline, with two
completed commands, 4,608 reads, and 512 writes. Causal comparison passes.
Strict comparison is intentionally not its acceptance criterion: the aggregate
direct-command DSE scheduler finishes 34 cycles later than the current per-tick
RTL driver and has a different independent event interleaving. A permanent
quick suite now checks its causal lanes and exact command/read/write counts.
After adding that suite, the final complete SAU quick run passed **63/63
checks across 23 suites**.

1. Review the pending source, test, comparator, and documentation changes.
2. Commit and push only after developer approval.

### PLAN3 Step 1 increment 1 — 2026-07-26

The first Step 1 source increment adds the functional memory and payload
infrastructure without changing any runtime scheduling behavior:

- `data_beat.hh` defines the frozen fixed-point views `MemoryBeat256`,
  `OperandVector32x8`, `AccumulatorVector32x24`, `OutputVector32x16`, and
  `WriteBeat256`, plus every conversion boundary from the Step 0 contract:
  24-bit sign extension, the SA_PE saturating 24-bit accumulate, the
  `sat_truncate_func` arithmetic shift by raw `cutbit=0..31` with int8
  saturation, the output-RF 16-bit two's-complement wrap add, and the
  `sat_signed8` writeout clamp.
- The beat byte order was re-proven directly from the
  `int8_gemm_32x32x32_atbd_cutbit8` package before implementation: the
  first captured `sau_sram_rdata` at `0x29120000` equals `initial_memory.hex`
  lines {1,0} concatenated, and the first output write beat at `0x29120c00`
  equals `final_output_memory.hex` bytes 0..31 in ascending address order.
  Beat byte k is external address X + k and RTL bit slice [8k+7:8k]; hex
  image lines are one little-endian word each (16 bytes for
  `initial_memory.hex` at the image base, one byte for
  `final_output_memory.hex`).
- `functional_memory.{hh,cc}` implements the strict-run data authority:
  byte-addressable sparse 4-KiB-page backing over a bounded range (a 1 GiB
  declaration allocates nothing; holes read the contract fill value without
  allocating), the little-endian word-per-line hex loader with explicit
  rejection of `$readmemh` directives/comments and malformed or
  out-of-range lines, and unified range dump/compare reporting the first
  differing address with its expected/actual byte and 256-bit beat/lane.
- `SauMemoryPort` now surfaces real read-response payloads through
  `SauMemoryResponse {beat, data}` and accepts an optional real write
  payload in `trySend()`. A null payload keeps the legacy zero-filled
  timing-only write contract, and a rejected packet retains its address,
  beat metadata, and payload unchanged until retry delivery.
  `SauModel::takeVisibleReadResponses()` unwraps the beat metadata and
  intentionally drops the payload until the functional datapath consumes
  it in later Step 3+ increments; no other runtime path changed.
- New focused tests: `data_beat.test.cc` (7 tests: lane mapping, sign
  extension, 24-bit saturation, cutbit shift/saturation extremes, 16-bit
  wrap, int8 clamp, write-beat assembly) and `functional_memory.test.cc`
  (9 tests: hole fill without allocation, sparse page accounting,
  cross-page beats, range rejection, both package hex formats, malformed
  images, dump/compare beat/lane reporting). `memory_port.test.cc` extends
  to 5 tests, adding real read payload surfacing, real write payload, and
  blocked-write payload survival across retry.

Static verification on 2026-07-26: `g++ -std=c++17 -fsyntax-only` passes
for `data_beat.hh`, `functional_memory.cc`, and both new test files;
`util/style.py` reports no issues in the new/changed regions (the three
remaining sau_model.cc long-line reports are pre-existing lines outside
this change); `git diff --check` passes. Per `src/sau/AGENTS.md`, gem5
compilation is developer-owned and pending:

```bash
scons build/ALL/sau/data_beat.test.opt \
    build/ALL/sau/functional_memory.test.opt \
    build/ALL/sau/memory_port.test.opt \
    --ignore-style --limit-ld-memory-usage -j32
./build/ALL/sau/data_beat.test.opt
./build/ALL/sau/functional_memory.test.opt
./build/ALL/sau/memory_port.test.opt

scons build/RISCV/gem5.opt --ignore-style --limit-ld-memory-usage -j32
cd tests && ./main.py run --skip-build gem5/sau
```

The quick suite must stay at 63/63: the memory-port change preserves
zero-filled writes and beat-only response consumption for every existing
run. Remaining Step 1 items for the next increments, in order: wire a
`FunctionalMemory` image source into strict fixed-SRAM read payloads and
RTL-edge write commit; timing-memory image preload via functional packets
plus the write-visibility barrier; stall primary-cause attribution per the
Step 0 priority; and the workload-level dump/compare entry point.

The developer built and ran this increment on 2026-07-26: the three
focused targets and the full quick suite passed.

### PLAN3 Step 1 increment 2 — 2026-07-26

The second increment wires the single-data-authority contract into
`SauModel` without touching any scheduling behavior:

- New optional SimObject parameters declare the functional memory
  contract: `functional_memory_base/size/fill`, an RTL hex
  `memory_image_file/base/word_bytes`, and a
  `final_memory_dump_file/base/size` range. `configs/example/sau_timing.py`
  exposes them as `--memory-image*`, `--functional-memory-*`, and
  `--final-memory-dump*`. They are data-contract options, valid together
  with strict `--rtl-profile` runs, and everything stays disabled when
  `functional_memory_size` is zero.
- Strict/calibration runs keep the loaded `FunctionalMemory` as their
  only data authority. Every accepted output write commits its beat to it
  at the local accepted edge (payload still all-zero until the output
  datapath lands in Step 5), and the final dump reads from it.
- Timing-memory runs stage the image only until `startup()`, preload the
  downstream memory through new idle-only
  `SauMemoryPort::writeFunctional/readFunctional` helpers (4-KiB
  chunks), then release the staging copy so the downstream memory is the
  run's only authority. The final dump uses functional readback, which is
  safe because `commandLocallyComplete()` already requires zero
  outstanding writes — that same condition is the write-visibility
  barrier before any next command.
- Both authorities dump through one writer,
  `FunctionalMemory::writeByteHexFile()`: one byte per line, ascending
  addresses — exactly the golden `final_output_memory.hex` format.
- `util/sau/compare_memory.py` is the workload-level comparator for both
  run types: little-endian word-per-line inputs of any width, first
  differing absolute address with expected/actual bytes and 256-bit
  beat/lane, plus the total mismatch count.

Verification already done on 2026-07-26: standalone
`data_beat`/`functional_memory` focused tests pass 17/17 (new dump
round-trip test included); `compare_memory` unit tests pass 9/9 and were
sanity-checked against the real cutbit-8 and cutbit-1 goldens
(self-compare clean; cross-compare reports 1024 differing bytes from
beat 0 lane 0); `py_compile`, `-fsyntax-only`, style on changed regions,
and `git diff --check` pass. Developer compilation is pending:

```bash
scons build/ALL/sau/functional_memory.test.opt \
    build/ALL/sau/memory_port.test.opt \
    --ignore-style --limit-ld-memory-usage -j32
./build/ALL/sau/functional_memory.test.opt
./build/ALL/sau/memory_port.test.opt

scons build/RISCV/gem5.opt --ignore-style --limit-ld-memory-usage -j32
cd tests && ./main.py run --skip-build gem5/sau && cd ..
```

The `ref/int8_gemm_32x32x32_single_flow` strict fixture and the
`functional_ref/int8_gemm_32x32x32_atbd_cutbit8` package share identical
`csr_writes.csv`, so the data plumbing has a direct smoke check. Both of
the following must produce byte-identical all-zero dumps (writeback data
is still zero) over the 1024-byte output range, proving image load,
preload, authority separation, commit, and dump on both paths:

```bash
./build/RISCV/gem5.opt --outdir=m5out/sau-func-strict \
    configs/example/sau_timing.py \
    --rtl-profile tests/gem5/sau/ref/int8_gemm_32x32x32_single_flow \
    --memory-image tests/gem5/sau/functional_ref/int8_gemm_32x32x32_atbd_cutbit8/initial_memory.hex \
    --memory-image-base 0x29120000 \
    --functional-memory-base 0x29120000 --functional-memory-size 0x40000 \
    --final-memory-dump m5out/sau-func-strict/final_output.hex \
    --final-memory-dump-base 0x29120c00 --final-memory-dump-size 0x400 \
    --trace=m5out/sau-func-strict/sau.csv

./build/RISCV/gem5.opt --outdir=m5out/sau-func-timing \
    configs/example/sau_timing.py \
    --rtl-profile tests/gem5/sau/ref/int8_gemm_32x32x32_single_flow \
    --timing-memory \
    --memory-image tests/gem5/sau/functional_ref/int8_gemm_32x32x32_atbd_cutbit8/initial_memory.hex \
    --memory-image-base 0x29120000 \
    --functional-memory-base 0x29120000 --functional-memory-size 0x40000 \
    --final-memory-dump m5out/sau-func-timing/final_output.hex \
    --final-memory-dump-base 0x29120c00 --final-memory-dump-size 0x400 \
    --trace=m5out/sau-func-timing/sau.csv

python3 util/sau/compare_memory.py --base 0x29120c00 \
    m5out/sau-func-strict/final_output.hex \
    m5out/sau-func-timing/final_output.hex

python3 -m unittest util.sau.compare_memory_test -v
```

A dump range over untouched operand data (for example
`--final-memory-dump-base 0x29120000 --final-memory-dump-size 0x400`)
must instead reproduce the initial image bytes on both paths. Comparing
either output-range dump against the package's `final_output_memory.hex`
must still fail — writeback data stays zero until Steps 4/5, and no
end-to-end data claim is made by this increment.

Increment 2 verification completed on 2026-07-26. The developer rebuilt
the two focused targets and relinked `gem5.opt`; both focused suites
pass. All smoke checks then passed: the strict and timing-memory
output-range dumps are byte-identical and all zero; both A-region dumps
reproduce the initial image bytes exactly; comparing the output dump
against the golden `final_output_memory.hex` fails as required (998
differing bytes — 26 golden result bytes are naturally zero); and the
strict run with the image enabled still passes full strict architecture
comparison against the RTL trace, proving the data plumbing changed no
timing. The complete SAU quick suite passes 63/63 across 23 suites.

### PLAN3 Step 1 increment 3 — 2026-07-26

The third increment adds single-attribution stall accounting per the
frozen Step 0 priority without changing any existing statistic:

- Every existing per-resource `stall*` scalar keeps its exact semantics
  and sites; those remain diagnostic event counts and may legitimately
  count several resources in one cycle.
- Each blocking site now also records a raw cause bit for the cycle:
  memory retry (including every cycle a rejected packet stays blocked,
  which the per-event scalar never counted), response starvation versus
  input backpressure (split by whether the missing operand's read is
  still in flight), outstanding-limit, array/result capacity, and
  output/writeback capacity.
- `accountCycle()` attributes each cycle with at least one raw cause to
  exactly one bucket of the new `primaryStallCycles` vector, choosing
  the highest-priority recorded cause in the frozen order: memory retry
  -> response starvation -> outstanding limit -> input backpressure ->
  array backpressure -> output backpressure. The vector total is
  therefore bounded by `commandCycles` and never double-counts a cycle.

Static checks pass (`git diff --check`, style, added-line lengths).
Developer compilation is pending; only `sau_model.{hh,cc}` changed:

```bash
scons build/RISCV/gem5.opt --ignore-style --limit-ld-memory-usage -j32
```

Post-link verification completed on 2026-07-26 after the developer
relink. The constrained direct-command profile attributes 76,240 of
76,320 command cycles (memory_retry 65,263, response_starvation 8,572,
outstanding_limit 2,405) — retry dominates because every cycle a
rejected packet stays blocked now counts, which the 2,558-event legacy
scalar never did. The output-buffer-1 DSE run attributes 629 of 668
cycles with a populated output_backpressure bucket, and most
output-full cycles correctly attribute to the higher-priority
outstanding-write limit that causes them; its 668 command cycles equal
the historical record exactly, confirming unchanged timing. The
64x256x256 strict baseline passes both the architecture and state
comparators, and the full quick suite passes 63/63.

PLAN3 Step 1 checklist state: the type/conversion, loader/sparse
memory, blocked-packet, single-authority/barrier, stall-attribution,
and dump/compare items are checked complete. Three items remain
partially open by design until their consumers land: strict read
payloads are captured from `FunctionalMemory` only when the Step 3
input datapath consumes them; write packets carry a real (non-zero)
payload only when the Step 5 output datapath produces one (the port
mechanism for both is already in place and unit-tested); and the
independent-resource-progress bullet keeps its existing behavior but
still needs a focused delayed-response test.

### PLAN3 Step 1 increment 4 — 2026-07-26

The fourth increment closes the remaining test-only gap in the Step 1
independent-resource-progress bullet. It changes no runtime source —
only the two focused test files — so `gem5.opt` and the 63/63 quick
suite are unaffected by construction:

- `memory_port.test.cc` adds
  `DelayedReadResponseDoesNotBlockIndependentTraffic`: while a read
  response stays pending, the port must keep `canIssue()` true (only a
  rejected packet occupies the request path), accept and complete an
  independent write, and accept a further read behind the delayed
  response. Read/write outstanding counts are held and released
  independently, the write response is never surfaced as visible data,
  and the delayed response finally arrives with its beat metadata and
  full 32-byte pattern payload intact, ordered before the later read.
  The test-local `TestMemory` gains a `respondBack()` helper so the
  write can respond while the earlier read request stays queued;
  `respond()`/`respondBack()` share one `respondPacket()` body.
- `token_pipeline.test.cc` adds
  `InFlightTokensMatureDuringInputStarvation`: with fill latency 4 and
  three tokens accepted at cycles 0..2, an input-starvation window from
  cycle 3 onward must not freeze in-flight work — each token matures at
  its original ready cycle (4/5/6), the pipeline drains to zero
  in-flight during the starvation window, and a token accepted after
  the gap (cycle 9) is due exactly one fill latency later (13) with no
  residual acceptance penalty.

Together these encode the component contracts behind the runtime rule
that a delayed response or full outstanding window freezes only the
resources that actually depend on it: `SauMemoryPort` never blocks
independent traffic behind a pending response, and `ArrayPipeline`
keeps draining buffered work while admission is starved.

Static verification on 2026-07-26: `util/style.py` reports no issues,
`git diff --check` passes, and `g++ -std=c++17 -fsyntax-only` passes
for both changed test files (using `-I build/ALL -I src -I ext
-I ext/googletest/googletest/include`). Developer compilation is
pending; no `Source()` file changed, so only the two focused targets
need rebuilding:

```bash
scons build/ALL/sau/memory_port.test.opt \
    build/ALL/sau/token_pipeline.test.opt \
    --ignore-style --limit-ld-memory-usage -j32
./build/ALL/sau/memory_port.test.opt
./build/ALL/sau/token_pipeline.test.opt
```

Expected results: `memory_port.test` grows from 6 to 7 tests and
`token_pipeline.test` from 8 to 9. Once both pass, the PLAN3 Step 1
independent-resource-progress checkbox can be checked; the two
remaining unchecked Step 1 items stay deferred to their Step 3/Step 5
consumers by design.

Increment 4 verification completed on 2026-07-26: the developer
rebuilt both focused targets and both suites passed — `memory_port.test`
7/7 and `token_pipeline.test` 9/9. The PLAN3 Step 1
independent-resource-progress checkbox is now checked. Step 1 is
thereby closed except for the two consumer-deferred items (strict read
payload consumption in Step 3; real write payloads in Step 5). The
next PLAN3 work item is Step 2, the full-legal-domain Int8 GEMM CSR
decode and typed resource-dispatch framework.

### PLAN3 Step 2 increment 1 — 2026-07-26

The first Step 2 increment replaces the fixture-value legality gate in
CSR decode with the frozen Step 0 support domain, preserves the
complete raw control state in the command, and adds the validation
maturity record plus the runtime fail-fast:

- `types.hh` gains the typed raw encodings `SauTransMode`
  (ABD/ATBD/ABTD/ABDT), `SauReuseMode` (None/A/B/AB),
  `SauSaFlowMode` (CNORMAL/CTRANS/RETAIN/TRETAIN), `SauPeWorkMode`,
  and the `ValidationMaturity` ladder
  (Decoded/RtlLegalUnimplemented/ResourceTimed/DataFunctional/
  EndToEndValidated). The four csr.sv counter-group structs moved from
  `csr_config.hh` into `types.hh` unchanged, and a new
  `SauControlFields` block — every raw mode/flag/cutbit/flow field,
  all four addresses, and all four counter groups with typed
  accessors — is embedded in `SauCommand` as `command.control`.
  Synthetic direct commands keep the neutral default.
- `SauCsrConfig::decode()` no longer rejects on
  `trans_mode/reuse_mode != 01/01`. Decode-level rejection now covers
  exactly the Step 0 out-of-stage operator switches — `pe_work_mode !=
  MATMUL`, `shift_flag=1`, `conv_kernal != 0`, `stride_flag=1`, and the
  depthwise `register_mode=10` — and the thrown message lists the
  authoritative reason plus the complete raw control summary. Every
  other raw combination decodes losslessly. The decoded result carries
  a maturity: the validated `trans=01/reuse=01/sa_flow=00` path (with
  `register_mode` 00/01/11 sharing the RTL non-depthwise guard) that
  also passes command validation is `ResourceTimed` and derives its
  timing policy; every other legal configuration — including raw wrap
  shapes such as `flow_loop_times=0` — is `RtlLegalUnimplemented` with
  a missing-path reason, never mislabeled illegal.
- `loadCsrFixture()` derives the RTL timing policy only for
  `ResourceTimed` commands, and the `SauModel` constructor fail-fasts
  with `rtl_legal_unimplemented` plus the full reason before any
  result or statistic when a replayed fixture command is legal but
  unimplemented. The strict `RtlCommandDriverSkeleton` keeps its own
  01/01 shape gate unchanged as the regression oracle.
- `csr_config.test.cc` drops the old 01/01 rejection test and adds
  four: out-of-stage operator rejection with raw-control message
  checks, lossless decode of all 4x4x4 trans/reuse/flow combinations
  with exact maturity classification, `cutbit=0..31` full-domain
  preservation at unchanged timing maturity, and the
  `flow_loop_times=0` legal-but-unimplemented wrap boundary. The
  small-fixture replay test now also checks `command.control` and its
  typed accessors.

Static verification on 2026-07-26: `g++ -std=c++17 -fsyntax-only`
passes for every changed file and every `types.hh` consumer swept
(`command.cc`, `timing_policy.cc`, `schedule_state.cc`,
`address_generator.cc`, `a_register_file.cc`, `sau_model.cc` against
the current `build/RISCV` params header); `util/style.py` reports no
issues in new or changed regions (the three remaining `sau_model.cc`
long-line reports are the documented pre-existing lines); a
pre-existing long line touched in `csr_fixture.cc` was wrapped;
`git diff --check` passes. Developer compilation is pending:

```bash
scons build/ALL/sau/csr_config.test.opt \
    --ignore-style --limit-ld-memory-usage -j32
./build/ALL/sau/csr_config.test.opt

scons build/RISCV/gem5.opt --ignore-style --limit-ld-memory-usage -j32
cd tests && ./main.py run --skip-build gem5/sau && cd ..
```

Expected results: `csr_config.test` goes from 7 to 10 tests. The full
quick suite must stay at 63/63 — every registered fixture is
`trans=01/reuse=01/sa_flow=00` and therefore still decodes to
`ResourceTimed` with an unchanged timing policy. Remaining Step 2
items for later increments: the shared typed resource configs for each
resource, raw-counter address generation, and the path-table mapping
checks from the Step 2 acceptance list.

The developer rebuilt this increment on 2026-07-26 and reported the
build passing. The 10-test focused suite was additionally compiled and
run standalone against the increment sources and passes 10/10.

### PLAN3 Step 2 increment 2 — 2026-07-26

The second Step 2 increment adds the shared typed resource-dispatch
module and binds every legal configuration to its frozen Step 0 path
row:

- New `resource_config.{hh,cc}` defines one typed configuration struct
  per Step 0 resource — controller/scheduler, streamed and resident
  address programs, input RF/padding/feeder, transposer/reuse banks,
  systolic array, output RF, and writeback — plus
  `deriveResourceConfigs(SauControlFields)`. Each struct carries
  exactly the CSR fields the RTL wires to that resource (for example
  `cutbit` reaches only the array shift/saturation boundary,
  `sa_flow_mode[1]` reaches the bank-retain, PE-keep, and output-RF
  accumulate flags, and the output register-side versus mem-side
  counters split between the output-RF and writeback configs). The
  dispatch is total over the decoded domain and contains no fixture,
  matrix-size, or 01/01 branch.
- `selectRtlPaths()` maps any raw control block onto the frozen
  PLAN3_STEP0 path table rows (`T-ABD..T-ABDT`, `R-none..R-AB`,
  `F-normal..F-tretain`). `SauCsrConfig::decode()` now includes the
  selected path row in every `rtl_legal_unimplemented` reason, so the
  runtime fail-fast names the exact missing structural path.
- New focused suite `resource_config.test` (3 tests): per-resource
  field dispatch against the small-fixture control state; a
  change-isolation test proving each legal field change reaches only
  its owning resource config (cutbit, trans_mode including the
  ABD direct-result row, reuse bits including 11, sa_flow bits,
  vertical counters, padding, and the split output counters); and the
  4x4x4 path-selection sweep cross-checked against the
  operand-transposer ownership flags. `csr_config.test` adds the
  path-row assertion to its mode-combination sweep and registers
  `resource_config.cc` as a dependency.

Static verification on 2026-07-26: syntax-only compilation, style on
all new/changed files, and `git diff --check` pass. Both focused
suites were also compiled and run standalone against the increment
sources: `resource_config.test` passes 3/3 and `csr_config.test`
passes 10/10. Developer compilation is pending; `csr_config.cc` and
the new `Source("resource_config.cc")` require a `gem5.opt` relink:

```bash
scons build/ALL/sau/resource_config.test.opt \
    build/ALL/sau/csr_config.test.opt \
    --ignore-style --limit-ld-memory-usage -j32
./build/ALL/sau/resource_config.test.opt
./build/ALL/sau/csr_config.test.opt

scons build/RISCV/gem5.opt --ignore-style --limit-ld-memory-usage -j32
cd tests && ./main.py run --skip-build gem5/sau && cd ..
```

The quick suite must stay at 63/63; no runtime scheduling path
consumes the new resource configs yet. Remaining Step 2 items:
raw-counter address generation and wiring the typed resource configs
into the module boundaries as their consumers land in Steps 3-5.

The developer rebuilt this increment on 2026-07-26 and reported the
build passing.

### PLAN3 Step 2 increment 3 — 2026-07-26

The third Step 2 increment implements raw-counter address programs
copied from the frozen RTL address generators. The evidence base is the
local authoritative checkout `/home/xch/workspace/npu_lpnpu`: the
SHA-256 of `hardware/src/sa_element/mem_addr.sv` and
`register_addr.sv` there match the frozen hashes in
`RTL_TIMING_PROVENANCE.md` exactly, so their source semantics are the
Step 0 contract, not a guess.

- New `address_program.{hh,cc}`:
  - `RtlStreamAddressProgram` reproduces `mem_addr.sv`: the two-stage
    step registers (`x_step*32`, `y_step*32`, `flow_step*y_step*32`
    through the 16-bit product register, and the fixed one-beat
    instruction step for `conv_kernal=0` — exactly the composite
    strides the eight strict fixtures validated), the x→y walk with
    row-start tracking, flow/instruction retriggers accumulating from
    `flow_start/ins_start`, `vertical_cnt_last` on entering each flow's
    final row, 32-bit wrap arithmetic, and the IDLE zero-count guard
    (any zero x/y/flow/instruction count ends immediately with no
    address — the raw zero boundary is "none", not "one"). Two
    source-faithful shape quirks are encoded and tested: `y_cycle==1`
    ends after the x walk without ever advancing flow/instruction, and
    `x_burst>1` emits only the first beat of each flow's final row
    because the source leaves RUNNING when entering it.
  - `RtlResidentAddressProgram` reproduces `register_addr.sv` (which
    also serves the output-RF unload instance): the x/y/channel loop
    with `y_step*32` and `c_step*y_step*32` steps, the zero-count
    guard, and the complete padding contract — padded x positions and
    fully padded y rows freeze the address (a padded row keeps
    `row_start` and returns to the row start) while the write pointer
    advances on every beat.
- The resource configs gained the fields the RTL actually wires to
  these modules: `conv_kernal` on the stream program and the
  pad/valid-window fields on the resident program (register_addr.sv
  consumes them directly); the resource-config change-isolation test
  now checks padding reaches both the input resource and the resident
  address program.
- New focused suite `address_program.test` (8 tests): composite-stride
  formula equality on the validated x=1 shape, both zero-count guards,
  the two source-derived shape quirks, the linear 256-beat baseline
  resident load (`x_burst=8, y_step=8, y_cycle=32`), a hand-computed
  24-beat padding sequence over two channels, and the raw-control →
  resource-config → program chain.

Static verification on 2026-07-26: syntax-only compilation, style, and
`git diff --check` pass. Standalone compilation and execution:
`address_program.test` passes 8/8 and the updated
`resource_config.test` passes 3/3. Runtime address ownership is
unchanged — `AddressGenerator` still owns runtime addresses until the
Step 3+ datapath consumes these programs. Developer compilation is
pending; the new `Source("address_program.cc")` requires a `gem5.opt`
relink:

```bash
scons build/ALL/sau/address_program.test.opt \
    build/ALL/sau/resource_config.test.opt \
    --ignore-style --limit-ld-memory-usage -j32
./build/ALL/sau/address_program.test.opt
./build/ALL/sau/resource_config.test.opt

scons build/RISCV/gem5.opt --ignore-style --limit-ld-memory-usage -j32
cd tests && ./main.py run --skip-build gem5/sau && cd ..
```

The quick suite must stay at 63/63. With this increment the Step 2
checklist items for full-domain decode, raw command fields, typed
resource dispatch, raw-counter address generation, maturity recording,
skeleton preservation, and Step 0 field classification all have
implementations; checking them off in `PLAN3.md` waits on this
increment's developer build.

Increment 3 verification completed on 2026-07-26: the developer
rebuilt the focused targets and relinked `gem5.opt`; all builds
passed. Six of the seven PLAN3 Step 2 checklist items are now checked:
full-domain decode, raw/type-safe command fields, typed resource
dispatch, raw-counter address programs, the validation-maturity
record, and the Step 0 field classification. The remaining item —
comparing new modes against RTL golden through a generic
boundary-trace comparator instead of a second skeleton state machine —
stays open by design until Step 3 runs the first non-default mode
(ABTD) against its Step 0 boundary golden; the skeleton itself remains
unexpanded as required. The next work item is PLAN3 Step 3:
configuration-driven input, transpose, and reuse resources, with the
`trans_mode=2 (ABTD)` B -> B^T transposer path as the first
non-default milestone against the captured
`functional_ref` ABTD boundary package.

### PLAN3 Step 3 opening survey and increment 1 — 2026-07-26

Step 3 (configuration-driven input/transpose/reuse resources) started
in a VCS-less environment. The opening survey established what evidence
is available:

- The four Step 0 functional golden packages under
  `tests/gem5/sau/functional_ref` are intact. The ABTD boundary package
  (`trans_mode=2`) carries a 634-row NPI signal trace covering SRAM
  read payloads, input-RF data in/out, A/B operand payloads,
  transposer bank 0/1 inRow/outCol with the full
  ready/rden/valid/last handshake, results, and the write chain — the
  golden boundary for the B -> B^T milestone. No new VCS run is needed
  for the golden-backed paths (ATBD end-to-end, ABTD boundary,
  reuse-A, reuse=11); the Step 0 rows still marked `pending` (T-ABD,
  T-ABDT, R-none, R-B, F-trans/retain/tretain) keep waiting for their
  boundary captures.
- RTL provenance state: the local `/home/xch/workspace/npu_lpnpu`
  checkout is `b7feb0d` (2026-07-15) — older than the frozen golden
  commit `d894466f15ea`, which is absent from local history and
  currently unreachable over the network. File-level comparison against
  the frozen `RTL_TIMING_PROVENANCE.md` hashes shows
  `register_file_in.sv`, `padding_shifter.sv`, `feeder.sv`,
  `mem_addr.sv`, and `register_addr.sv` are byte-identical to the
  frozen contract and therefore usable as authority. `sa_feeder.sv`
  differs (local is the older 07-15 version; frozen is `851007b4…`),
  and `transposer_tiny.v`/`transposer_tiny_pe.v` have no frozen hash
  at all. **The transposer/sa_feeder payload increment is blocked until
  the frozen-state copies of `sa_execute/sa_feeder.sv`,
  `transposer_tiny.v`, and `transposer_tiny_pe.v` (plus `SA_ROW.sv`,
  `SA_PE_array.sv`, `SA_PE.sv`, `SA_pkg.sv` for Step 4) are copied
  from the workstation `d894466` worktree**; `sa_feeder.sv` will be
  verified against its frozen hash on arrival and the others frozen
  into the provenance record then.
- `feeder.sv` source fact useful for later increments:
  `conv_reuse_flag` is constant 1, so operand A always flows through
  the register-file readout + `shift_register` path while operand B
  flows through the delayed `data_i` pipeline to the A/B arbiter; the
  B-side payload needs no `shift_register` source, but the A-side
  payload increment does.

Increment 1 implements the input-side payload resources whose sources
are hash-verified:

- New `input_datapath.{hh,cc}`:
  - `StreamPaddingShifter` (`padding_shifter.sv`): the one-stage byte
    barrel shifter — output byte j takes input byte j-p, the vacated
    low bytes take zero at start-of-packet or the previous beat's tail.
  - `InputRegisterFile` (`register_file_in.sv` storage): 256 entries of
    one 32-byte beat plus the avail-label bitset; clear resets labels
    only (reset storage is not a functional initializer, and the RTL
    read port has no label gate — both from the frozen source and the
    Step 0 state-lifetime table).
  - `InputReadPointerProgram` (`register_file_in.sv` read FSM): the
    x/y/flow/ins counter program over the raw CSR input group with the
    shared single-adder base tracking, 8-bit pointer wrap, and the
    6-bit "burst-1" end compares under which a raw zero burst means 64
    iterations — deliberately different from `mem_addr.sv`'s
    immediate-done zero guard, both source-derived.
- New focused suite `input_datapath.test` (7 tests): zero-padding
  passthrough, padding insert/carry across beats, storage/label
  behavior, the small-fixture 0..31 walk, the baseline 8-instruction
  256-entry walk, 8-bit pointer wrap, and the zero-burst=64 boundary.

Static verification on 2026-07-26: syntax-only compile, style, and
`git diff --check` pass; the standalone build runs `input_datapath.test`
at 7/7. Developer compilation is pending; the new
`Source("input_datapath.cc")` requires a `gem5.opt` relink:

```bash
scons build/ALL/sau/input_datapath.test.opt \
    --ignore-style --limit-ld-memory-usage -j32
./build/ALL/sau/input_datapath.test.opt

scons build/RISCV/gem5.opt --ignore-style --limit-ld-memory-usage -j32
cd tests && ./main.py run --skip-build gem5/sau && cd ..
```

The quick suite must stay at 63/63; nothing consumes the new module at
runtime yet. Next Step 3 increments, in order: the feeder B-side
payload pipeline and A/B arbiter payload muxing (needs the
`shift_register` source read for the A side), then the transposer bank
resource once the frozen `sa_execute` sources arrive, then the
generic boundary-trace comparator against the ABTD package.

Step 3 increment 1 verification completed on 2026-07-26: the developer
rebuilt the focused target and relinked `gem5.opt`; all builds passed.

### PLAN3 Step 3 increment 2 — 2026-07-26

The second increment adds the feeder operand-B payload chain and the
input write-path composition, both from the hash-verified `feeder.sv`,
`register_file_in.sv`, and `padding_shifter.sv` sources:

- `FeederBPipeline` models the int8-GEMM streamed-operand chain
  `data_i -> data_i_d -> data_i_d2 -> data_i_case0_reg -> data_B_o`
  with the register-file write payload tapped at `data_i_d`. Because
  the source's `conv_reuse_flag` is constant 1, operand A never takes
  this chain (it flows through the register-file readout plus
  `shift_register`, which waits for the frozen `sa_execute` sources);
  only the B side is modeled here. The A/B arbiter enable is an input
  whose timing stays owned by the strict input-feeder skeleton, and a
  disabled edge registers a zero beat exactly like the RTL mux.
- `InputWritePath` composes the write side: a pad-flagged position
  writes zero into the `StreamPaddingShifter`, and the shifted beat is
  stored at the delay-aligned pointer with its avail label.
- `input_datapath.test` grows from 7 to 11 tests: the four-register
  B-chain delay (input visible on the fourth cycle) with the one-stage
  write tap, the disabled-edge zero beat with the chain still moving,
  raw-beat storage at zero padding, and the pad-flagged zero write
  whose tail carries into the following beats through the shifter.

Static verification on 2026-07-26: syntax-only compile, style, and
`git diff --check` pass; the standalone build runs `input_datapath.test`
at 11/11. Developer compilation is pending; `input_datapath.cc` is a
`Source()` file, so `gem5.opt` needs a relink:

```bash
scons build/ALL/sau/input_datapath.test.opt \
    --ignore-style --limit-ld-memory-usage -j32
./build/ALL/sau/input_datapath.test.opt

scons build/RISCV/gem5.opt --ignore-style --limit-ld-memory-usage -j32
cd tests && ./main.py run --skip-build gem5/sau && cd ..
```

The quick suite must stay at 63/63. The remaining Step 3 increments —
the A-side `shift_register` payload, the transposer bank resource, and
the sa_feeder-side integration — are blocked on the frozen `d894466`
copies of `hardware/src/sa_execute/{sa_feeder.sv, transposer_tiny.v,
transposer_tiny_pe.v, shift_register.sv}` (plus `SA_ROW.sv`,
`SA_PE_array.sv`, `SA_PE.sv`, `SA_pkg.sv` for Step 4) from the
workstation worktree. The generic boundary-trace comparator against
the ABTD package can proceed next without them.

Step 3 increment 2 verification completed on 2026-07-26: the developer
rebuilt the focused target and relinked `gem5.opt`; all builds passed.

### PLAN3 Step 3 increment 3 — 2026-07-26

The third increment adds the generic boundary-trace comparator that new
modes must validate through instead of a second command-driver state
machine (the mechanism behind the last open PLAN3 Step 2 checklist
item):

- `util/sau/compare_boundary.py` compares a model boundary trace
  (`signal,cycle,value`, first row per signal = initial state at its
  cycle) against the NPI value-change export shipped in the
  `functional_ref` packages (`signal,time,value`). Golden times anchor
  to the first occurrence of a chosen signal value (default: the
  `start` rise) and convert to cycles with the RTL clock; every golden
  change must land exactly on a clock edge. Values canonicalize from
  binary or 0x-hex, x/z values stay verbatim, pre-anchor changes fold
  into the cycle-0 state, and consecutive duplicates collapse on both
  sides per value-change semantics. `sequence` mode compares each
  signal's ordered values (payload maturity); `cycles` mode also
  requires exact relative cycles (boundary timing). `--signals`
  restricts and requires named signals; `--inspect` dumps the anchored
  golden sequences for bring-up. A successful comparison prints
  nothing.
- `util/sau/compare_boundary_test.py` passes 9/9: mixed-base matching,
  value-mismatch reporting with index and both cycles, a cycle shift
  caught only by `cycles` mode, pre-anchor folding, duplicate
  collapsing, missing-required-signal errors, off-edge golden times,
  decreasing model cycles, and change-count differences.
- Real-golden validation: `--inspect` loads the complete 633-row ABTD
  `boundary.csv` with every change landing on a 1667 ps clock edge
  relative to the start rise. A conversion of that golden into the
  model schema round-trips through `--mode cycles` with zero
  differences across all 45 shared signals, and a filtered comparison
  over `data_B`, `u_trans2sa_top.trans0_inRow/outCol`, and
  `sau_sram_rdata` — the B -> B^T milestone boundary — also passes.

No C++ or build change is involved; verification is
`python3 -m unittest util.sau.compare_boundary_test` plus the
documented real-golden commands, all already run. The PLAN3 Step 2
comparator checkbox stays open until the first model-produced ABTD
trace passes through this tool, which lands with the transposer
increments once the frozen `sa_execute` sources arrive.

### PLAN3 Step 3 increment 4 — 2026-07-26

The developer's `git pull` of `/home/xch/workspace/npu_lpnpu` unblocked
the payload sources. The frozen commit `d894466` is still absent (it
was workstation-local), but four fetched commits simultaneously match
every file hash already frozen in `RTL_TIMING_PROVENANCE.md`, and the
seven previously unfrozen `sa_execute` files are byte-identical across
all four — their content at the golden state is therefore unambiguous.
`e722852bd9ab` is recorded as the canonical extraction reference and
the seven SHA-256 hashes are frozen in a new provenance section. The
current fetched HEAD `f4cbb25` has evolved past the contract in seven
files (including `sa_feeder.sv`, `SA_ENGINE.sv`, `shift_register.sv`,
and `SA_PE_array.sv`); those worktree files are explicitly not
authority.

The increment then implements the transposer bank resource from the
frozen `transposer_tiny.v`:

- New `transposer.{hh,cc}`: `TransposerTinyBank` holds 32 rows of one
  32-byte beat. Input acceptance stores at `cnt_in`; the final row
  drops the input ready and raises the output ready. Output
  consumption returns the transposed column — output lane a takes row
  (31-a), byte (column index) — or the stored row in passthrough mode;
  draining the final output restores the input ready, with `reuse_en`
  keeping the output side ready for replay and non-reuse closing it. A
  not-ready write still stores but latches the error flag until the
  drain, and `clear()` zeroes the storage itself, all as the source
  does. Registered outCol/valid/last delays, the same-cycle input
  bypass, and the prefetch-ahead index remain cycle-level driver
  concerns.
- The row-reversed lane mapping was proven against real RTL data
  before implementation: in the ABTD golden, the 32 accepted
  `trans0_inRow` beats (cycles 43 and 78..108) and the first
  prefetched `trans0_outCol` at cycle 109 satisfy
  `outCol lane a = row[31-a].byte[0]` exactly; the check script and
  the derived constants are embedded in the focused test.
- New focused suite `transposer.test` (6 tests): full 32x32 synthetic
  transpose with the reversed lane mapping and non-reuse close, the
  ABTD golden first-column vector, passthrough row order, reuse
  replay across two passes, overflow-error latch/clear, and
  clear-resets-storage.

Static verification on 2026-07-26: syntax-only compile, style, and
`git diff --check` pass; the standalone build runs `transposer.test`
at 6/6. Developer compilation is pending; the new
`Source("transposer.cc")` requires a `gem5.opt` relink:

```bash
scons build/ALL/sau/transposer.test.opt \
    --ignore-style --limit-ld-memory-usage -j32
./build/ALL/sau/transposer.test.opt

scons build/RISCV/gem5.opt --ignore-style --limit-ld-memory-usage -j32
cd tests && ./main.py run --skip-build gem5/sau && cd ..
```

The quick suite must stay at 63/63. Remaining Step 3 increments: the
`shift_register` A-side payload (frozen source now available), the
sa_feeder-side T0/T1/T2 bank ownership and input-switch integration,
and the model-produced ABTD boundary trace through
`compare_boundary.py` — the final validation that also closes the last
PLAN3 Step 2 checkbox.

Step 3 increment 4 verification completed on 2026-07-26: the developer
rebuilt the focused target and relinked `gem5.opt`; all builds passed.

### PLAN3 Step 3 increment 5 — 2026-07-26

The fifth increment adds the operand-A payload chain from the frozen
`shift_register.sv` and closes the resident A-side payload path:

- For the int8 GEMM stage (`conv_kernal<=1`), `shift_register.sv` is
  in its bypass mode: `kernal_cnt` stays cleared, `shift_data_result`
  equals `data_i`, and idle cycles load zero into the shift stage. The
  A payload chain is therefore register-file readout -> one shift
  stage -> the constant `conv_reuse` data_A mux -> registered
  `data_A_o`. `FeederAPipeline` in `input_datapath.{hh,cc}` models
  exactly that: a two-register passthrough whose disabled edges insert
  zero bubbles.
- The payload contract was checked against the ABTD golden before
  implementation: the `data_A` value-change sequence equals the
  `core_register_data_out` readout sequence in order (the apparent
  38-cycle first-burst offset is the REGISTER_LOAD write-phase echo on
  the RF read port plus value-change compression, not a payload
  transform). Exact cycle alignment stays owned by the strict
  input-feeder skeleton, which already pins the 266/267/269 edges.
- `input_datapath.test` grows from 11 to 14 tests: the two-stage
  delay, idle-cycle zero bubbles, and a full A-side composition —
  32 beats through `InputWritePath` into the register file, read back
  in `InputReadPointerProgram` order through `FeederAPipeline`,
  arriving unchanged and in sequence.

Static verification on 2026-07-26: syntax-only compile, style, and
`git diff --check` pass; the standalone build runs `input_datapath.test`
at 14/14. Developer compilation is pending (`input_datapath.cc` is a
`Source()` file):

```bash
scons build/ALL/sau/input_datapath.test.opt \
    --ignore-style --limit-ld-memory-usage -j32
./build/ALL/sau/input_datapath.test.opt

scons build/RISCV/gem5.opt --ignore-style --limit-ld-memory-usage -j32
cd tests && ./main.py run --skip-build gem5/sau && cd ..
```

The quick suite must stay at 63/63. Remaining Step 3 increments: the
sa_feeder-side T0/T1/T2 bank ownership and input-switch integration
(frozen `sa_feeder.sv` available), then the model-produced ABTD
boundary trace through `compare_boundary.py`.

Step 3 increment 5 verification completed on 2026-07-26: the developer
rebuilt the focused target and relinked `gem5.opt`; all builds passed.

### PLAN3 Step 3 increment 6 — 2026-07-26

The sixth increment adds the sa_feeder-side transposer arbiter from
the frozen `sa_feeder.sv`:

- `TransposerArbiter` in `transposer.{hh,cc}` owns the T0/T1 operand
  banks. Frozen-source facts it encodes: the loading operand is A for
  ATBD and B for ABTD; ABD without the out-of-stage conv path is the
  pure-output mode whose input steering is disabled; an accepted row
  steers to T0 first and to T1 only when T0 cannot take input, with
  `trans_loaded_bank_q` remembering the last loaded bank; an
  output-phase start latches the loaded bank as the output bank and
  otherwise the selection follows whichever bank is exclusively ready
  (the ping-pong); the banks elaborate with `reuse_en` tied low —
  sa_flow retention works by skipping the command-start clear, which
  also resets the selection state; T2 belongs to the result serializer
  and stays outside. `saOperandRouting()` mirrors the SA data arbiter:
  input_switch 01 feeds the left port from the banks (ATBD) with B
  direct above, input_switch 10 feeds the above port from the banks
  (ABTD) with A direct left.
- Call-order contract: within one modeled cycle, input acceptances are
  presented before output consumptions because the RTL steering
  samples the registered pre-edge bank ready — a same-cycle drain
  completion must not redirect that cycle's input row. The ping-pong
  test initially exposed exactly that artifact and now encodes the
  contract.
- `transposer.test` grows from 6 to 11 tests: ABTD loads B and feeds
  the above port with the transposed columns, ATBD feeds the left
  port, ABD accepts no operand rows, a two-flow ping-pong across both
  banks with payload checks on each, and retain-versus-clear at
  command start. The configs come through
  `deriveResourceConfigs()`, exercising the Step 2 dispatch chain.

Static verification on 2026-07-26: syntax-only compile, style, and
`git diff --check` pass; the standalone build runs `transposer.test`
at 11/11. Developer compilation is pending; `transposer.test` gains a
`resource_config.cc` dependency in `SConscript`:

```bash
scons build/ALL/sau/transposer.test.opt \
    --ignore-style --limit-ld-memory-usage -j32
./build/ALL/sau/transposer.test.opt

scons build/RISCV/gem5.opt --ignore-style --limit-ld-memory-usage -j32
cd tests && ./main.py run --skip-build gem5/sau && cd ..
```

The quick suite must stay at 63/63. The remaining Step 3 increment is
the model-produced ABTD boundary trace through `compare_boundary.py`:
compose the memory image, `FeederBPipeline`, and `TransposerArbiter`
into the B -> B^T sequence, emit the model-schema trace, and compare
the `data_B`/`trans0_inRow`/`trans0_outCol` payload sequences against
the golden — the `data_functional` acceptance for the first
non-default mode, which also closes the last PLAN3 Step 2 checkbox.

Step 3 increment 6 verification completed on 2026-07-26: the developer
rebuilt the focused target and relinked `gem5.opt`; all builds passed.

### PLAN3 Step 3 increment 7 — 2026-07-26

The seventh increment runs the first model-versus-golden boundary
comparison for a non-default mode and lands three pieces:

- `compare_boundary.py` gains `--qualify DATA=VALID` (wire-style
  golden signals drop to zero outside their valid windows; the
  qualified sequence keeps only accepted payload values) and
  `--allow-actual-extra` (a truncated golden capture window compares
  as a matched prefix). `compare_boundary_test` grows from 9 to 12.
- New `boundary_trace.{hh,cc}`: `BoundaryTraceWriter` emits the model
  trace schema, and `generateAbtdBoundaryTrace()` composes the package
  memory image, the resident and streamed raw-counter address
  programs, `FeederBPipeline`, and `TransposerArbiter` into the
  idealized B -> B^T sequence, emitting `sau_sram_rdata`, `data_B`,
  `trans0_inRow`, and `trans0_outCol`. `boundary_trace.test` (2 tests)
  checks the writer format and the generated trace shape, skipping
  when the package is not visible from the working directory.
- The comparison run (documented below) produced one full pass and one
  precisely diagnosed divergence:
  - `sau_sram_rdata` PASSES in strict sequence terms: all 64 external
    read payloads (32 resident + 32 streamed) match the golden exactly
    — the image loader, byte order, and both raw-counter address
    programs are data-correct end to end.
  - `data_B`/`trans0_inRow`/`trans0_outCol` diverge, and the
    comparator pinpointed real raw-configuration RTL behavior: the
    golden T0 bank receives `[A31, B0, A1..A30]`, not `[B0..B31]`.
    The load enables derive from B-valid pulses, but the loaded data
    follows `data_i = input_switch[1] ? data_B : data_A`
    (`sa_feeder.sv`, annotated `//todo: maybe bug` at that mux), and
    the scheduler drives the A-side input-switch sequence under raw
    ABTD+reuse-A. Script-verified corroboration: golden accepted
    `data_B[1..31]` equals the model B beats `[0..30]` exactly, and
    the leaked first acceptance equals model A beat 31. This is the
    microscopic mechanism behind the Step 0 conclusion that the ABTD
    package is a boundary oracle with 1005/1024 software mismatches,
    not a math oracle.

Per the PLAN3 fidelity principle, gem5 must reproduce this raw
behavior rather than the idealized transpose. Reaching the full ABTD
boundary match therefore needs the ABTD control chain — the scheduler
`input_switch` timeline and feeder arbiter enables for `trans_mode=2`,
derived from the frozen `scheduler.sv`/`feeder.sv` sources — which is
the next increment. The PLAN3 Step 2 comparator checkbox stays open
until that match lands; the comparator mechanism itself is now proven
in real use.

Verification already run on 2026-07-26: `compare_boundary_test` 12/12,
`boundary_trace.test` standalone 2/2, style and `git diff --check`
clean. Developer compilation is pending; `boundary_trace.cc` is a new
`Source()` file:

```bash
scons build/ALL/sau/boundary_trace.test.opt \
    --ignore-style --limit-ld-memory-usage -j32
./build/ALL/sau/boundary_trace.test.opt

scons build/RISCV/gem5.opt --ignore-style --limit-ld-memory-usage -j32
cd tests && ./main.py run --skip-build gem5/sau && cd ..

python3 -m unittest util.sau.compare_boundary_test
```

Step 3 increment 7 verification completed on 2026-07-26: the developer
rebuilt the focused targets and relinked `gem5.opt`; all builds and
suites passed.

The comparison recipe (already exercised standalone; the generator
binary is the focused test's `generateAbtdBoundaryTrace`):

```bash
python3 util/sau/compare_boundary.py --mode sequence \
    --allow-actual-extra \
    --qualify data_B=data_B_valid,u_trans2sa_top.trans0_inRow=u_trans2sa_top.trans0_inRow_en \
    --signals sau_sram_rdata,data_B,u_trans2sa_top.trans0_inRow,u_trans2sa_top.trans0_outCol \
    tests/gem5/sau/functional_ref/int8_gemm_32x32x32_abtd_boundary/boundary.csv \
    <model trace csv>
```

### PLAN3 Step 3 increment 8 — 2026-07-26

The eighth increment derives the ABTD control chain from the frozen
`scheduler.sv`/`feeder.sv`/`sa_feeder.sv` sources (extracted from
`e722852bd9ab`; all three SHA-256 hashes match the frozen provenance
record) and lands the full model-versus-golden ABTD boundary match —
the `data_functional` acceptance for the first non-default mode and
the close of the last PLAN3 Step 2 checklist item.

Source-derived mechanism, each element confirmed against the golden:

- `scheduler.sv` preloads `input_switch=2'b11` in IDLE when
  `trans_mode==2'b10`, and the REUSE_LOAD branch forces `2'b01`
  unconditionally because `conv_reuse_flag` is constant 1 — the
  raw-config quirk that overrides the commented ABT-reuse-A switch
  table. TRANSPOSE_LOAD holds for `SA_SIZE=32` edges
  (`transload_state_cnt`).
- `feeder.sv`'s B gate is `!input_switch_case & !NO_INPUT_state`.
  Under ABTD the switch bit 0 is 1 in both 11 and 01, so
  `input_switch_case` stays 0 and the B gate is open the whole
  command — unlike the 01/01 path where case=1 suppresses the
  resident tail. The out-state machine leaves NO_INPUT at the resident
  `data_last_i`; the 2-cycle-delayed `NO_INPUT_state` and the
  3-cycle-delayed `EN_i_d_o[2]` overlap for exactly one cycle, and
  `data_i_case0_reg` holds the last resident beat there: one early
  accepted `data_B` pulse carrying A31 at the resident tail + 4.
- The scheduler switch reaches the sa_feeder ports 11 cycles later
  (feeder `STATE_DELAY=7` + `input_switch_d` + two out-pipe stages +
  the `input_switch_o` register). The first streamed beat surfaces at
  REUSE_LOAD entry + 11 (request offset 3 + mem visibility 4 + feeder
  chain 4) while the forced 01 arrives at entry + 12, so exactly one
  B beat (B0) enters the bank; from the next cycle the sa_feeder mux
  `data_i = input_switch[1] ? data_B_i : data_A_i` follows the
  replayed data_A stream. Bank rows: `[A31, B0, A1..A30]`; the 33rd
  enable steers A31 to T1. B31 never enters a bank.
- `conv_reuse_flag==1` keeps data_A on the register-file readout path;
  reuse-A replays the 32-beat readout back to back
  (`shift_almost_last` retrigger), giving 64 continuous data_A beats.

Implementation: `generateAbtdBoundaryTrace()` is now a per-cycle
register model — the feeder out-state machine, EN/state/switch delay
taps, `FeederBPipeline` data chain, the sa_feeder mux register, and
the T0/T1 arbiter — with every cycle anchor a named structural
constant (none fitted to the golden). The leak pulse and the mixed
bank load emerge from the register mechanics. `BoundaryTraceWriter`
gains explicit-cycle emits, and the trace now carries real model
cycles plus the `data_A` payload stream. The output bank latches from
the pre-edge loaded bank when T0 fills, before the overflow row
reaches T1.

Verification already run on 2026-07-26 (standalone builds):
`boundary_trace.test` passes 2/2 with the new 228-row shape, and the
generated trace passes the golden comparison in BOTH modes — sequence
(payload) and cycles (every accepted edge at the exact golden cycle:
rdata 6..37/72..103, data_A 45..108, data_B 42 + 77..108, trans0_inRow
43 + 78..108, outCol prefetch 109):

```bash
python3 util/sau/compare_boundary.py --mode cycles \
    --allow-actual-extra \
    --qualify data_A=data_A_valid,data_B=data_B_valid,u_trans2sa_top.trans0_inRow=u_trans2sa_top.trans0_inRow_en \
    --signals sau_sram_rdata,data_A,data_B,u_trans2sa_top.trans0_inRow,u_trans2sa_top.trans0_outCol \
    tests/gem5/sau/functional_ref/int8_gemm_32x32x32_abtd_boundary/boundary.csv \
    <model trace csv>
```

`--mode sequence` with the same arguments also passes. Style and
`git diff --check` are clean. The last PLAN3 Step 2 checklist item is
checked: the first non-default mode is validated through the generic
boundary comparator, with no second command-driver state machine.

Developer compilation is pending; `boundary_trace.cc` is a `Source()`
file, so `gem5.opt` needs a relink:

```bash
scons build/ALL/sau/boundary_trace.test.opt \
    --ignore-style --limit-ld-memory-usage -j32
./build/ALL/sau/boundary_trace.test.opt

scons build/RISCV/gem5.opt --ignore-style --limit-ld-memory-usage -j32
cd tests && ./main.py run --skip-build gem5/sau && cd ..
```

The quick suite must stay at 63/63; nothing consumes the generator at
runtime. Remaining Step 3 work: runtime integration of the payload
resources into `SauModel` (with the transposer/reuse statistics), and
the VCS-blocked R-none/R-B boundary captures.

Step 3 increment 8 verification completed on 2026-07-26: the developer
rebuilt the focused target and relinked `gem5.opt`; all builds passed.

### PLAN3 Step 3 increment 9 — 2026-07-26

The ninth increment starts the Step 3 runtime integration: strict
fixture runs with a functional memory authority now move real 256-bit
payloads through the input/transposer resources along the strict
driver's own edges, own the transposer/reuse statistics, and can emit
a runtime boundary trace for the generic comparator.

- New `payload_datapath.{hh,cc}`: `StrictPayloadDatapath` composes the
  Step 3 payload resources under driver control. A mem_ctrl-visible
  resident beat enters `InputRegisterFile` through `InputWritePath`; a
  streamed beat queues for the operand-B path; a register-file
  read-valid edge advances `InputReadPointerProgram` (replaying it
  when exhausted — the reuse readout) and queues the readout payload;
  an operand edge pops its queue and, when the transpose/reuse config
  loads that operand, the row enters the T0/T1 `TransposerArbiter`
  with RTL steering/ping-pong; an accepted SA-enable edge consumes one
  bank column. Pulse/payload mismatches (empty queue, no free bank, no
  drainable column) are counted, not fatal — bring-up divergence
  evidence, mirroring the RTL's own error-latch behavior.
  `payload_datapath.test` (3 tests) covers the ATBD small-fixture
  payload flow (RF storage, 64-row reuse readout into both banks, the
  transposed lane mapping of the drained column, first-in/first-out
  edges, occupancy/busy), operand-B staying outside the ATBD banks,
  and the divergence counters.
- `SauModel` integration (strict + `functionalMemory` only): the
  command driver's registered pulses (`registerFileReadValid`,
  `dataAValid`, `dataBValid`, `saEnable`) drive the payload datapath
  each edge via a new driver-edge counter that matches the skeleton's
  command-relative edge numbering; strict visible read responses fetch
  the real payload from the `FunctionalMemory` authority (the PLAN3
  Step 1 consumer-deferred item) and feed `onMemoryDataVisible`. New
  statistics: `payloadReadBeats`, transposer input rows / output
  columns / input stalls / output stalls / payload underflows / busy
  cycles / max bank occupancy, and per-command first-row-in to
  first-column-out latency. Runs without an image are bit-for-bit
  unchanged — the payload path is simply absent.
- New `boundary_trace_file` param (`--boundary-trace`, requires
  `--rtl-profile` and `--memory-image`): the first strict command
  emits `sau_sram_rdata` (request + SRAM_DELAY) and
  `core_register_data_out` (the mem_ctrl-visible feeder input tap, one
  edge later) with real payloads at driver edges.
- Verification anchor: the Step 0 ATBD end-to-end package
  `int8_gemm_32x32x32_atbd_cutbit8` carries byte-identical
  `csr_writes.csv` to the strict quick fixture
  `int8_gemm_32x32x32_single_flow` and matching elaboration
  parameters, so the strict replay entry is the timing package while
  the functional package supplies the image and the golden
  `boundary.csv`. Golden labeling proves `sau_sram_rdata` carries
  A0..A31 at cycles 6..37 and B0..B31 at 72..103, and
  `core_register_data_out` the same beats one cycle later — matching
  the strict driver's request edges 3..34/69..100 plus the storage
  contract. (`core_register_data_in` carries result payloads and waits
  for Steps 4/5.)

Static verification on 2026-07-26: `payload_datapath.test` standalone
3/3, `sau_model.cc` syntax-checks against a param-patched header,
`py_compile` on `sau_timing.py`/`Sau.py`, style, and
`git diff --check` all pass. Developer compilation is pending; the new
`Source("payload_datapath.cc")` and the new parameter require a
`gem5.opt` relink:

```bash
scons build/ALL/sau/payload_datapath.test.opt \
    --ignore-style --limit-ld-memory-usage -j32
./build/ALL/sau/payload_datapath.test.opt

scons build/RISCV/gem5.opt --ignore-style --limit-ld-memory-usage -j32

./build/RISCV/gem5.opt --outdir=m5out/sau-atbd-payload \
    configs/example/sau_timing.py \
    --rtl-profile tests/gem5/sau/ref/int8_gemm_32x32x32_single_flow \
    --memory-image tests/gem5/sau/functional_ref/int8_gemm_32x32x32_atbd_cutbit8/initial_memory.hex \
    --memory-image-base 0x29120000 \
    --functional-memory-base 0x29120000 \
    --functional-memory-size 0x40000 \
    --boundary-trace m5out/sau-atbd-payload/boundary.csv \
    --trace=m5out/sau-atbd-payload/sau.csv

python3 util/sau/compare_boundary.py --mode sequence \
    --allow-actual-extra \
    --qualify core_register_data_out=core_register_data_out_valid \
    --signals sau_sram_rdata,core_register_data_out \
    tests/gem5/sau/functional_ref/int8_gemm_32x32x32_atbd_cutbit8/boundary.csv \
    m5out/sau-atbd-payload/boundary.csv

# Informational: the driver-edge emission should also align exactly.
python3 util/sau/compare_boundary.py --mode cycles \
    --allow-actual-extra \
    --qualify core_register_data_out=core_register_data_out_valid \
    --signals sau_sram_rdata,core_register_data_out \
    tests/gem5/sau/functional_ref/int8_gemm_32x32x32_atbd_cutbit8/boundary.csv \
    m5out/sau-atbd-payload/boundary.csv

cd tests && ./main.py run --skip-build gem5/sau && cd ..
```

The sequence-mode comparison and the 63/63 quick suite are the
acceptance; the cycles-mode run is expected to pass from the driver
edge numbering and is diagnostic if it does not. The transposer
statistics land in `m5out/sau-atbd-payload/stats.txt`
(`transposer*`, `payloadReadBeats`). Remaining Step 3 work after this
increment: extending the runtime payload validation as deeper golden
signals become available, and the VCS-blocked R-none/R-B captures.

Increment 9 developer verification completed on 2026-07-27. The first
runtime run completed normally and preserved the existing 63/63 quick
regression, but its new boundary comparison exposed two trace-only defects:
`BoundaryTraceWriter` did not flush before gem5 exited, leaving 18 payload
pairs buffered, and the exported shared-SRAM/mem_ctrl payload cycles were one
edge late because response consumption observed the post-tick driver counter.
The follow-up adds an explicit writer `flush()` at command completion and
defines the mem_ctrl-visible payload edge as `rtlDriverEdge - 1`, with
shared-SRAM rdata one edge earlier.

After the developer rebuilt `boundary_trace.test.opt` and `gem5.opt`,
`boundary_trace.test` passed 3/3, including the new flush-before-destruction
test. The strict ATBD payload run exited through `SAU command complete`;
`sau_sram_rdata` and `core_register_data_out` then passed both sequence and
exact-cycles comparison against the frozen functional package. The complete
SAU quick run passed 63/63 checks across 23 suites. Increment 9 runtime
integration is therefore verified; remaining Step 3 work is deeper golden
signals and the VCS-blocked R-none/R-B paths.

### PLAN3 Step 3 increment 10 — 2026-07-27

The tenth increment deepens the strict runtime payload boundary from the
shared-SRAM/input-RF taps into the operand and T0/T1 transposer interfaces.
The original cutbit-8 ATBD FSDB named by the frozen functional package remains
available locally. A developer-run `npi_fsdb_probe` export captured 239 rows
for `data_A/data_B`, their valid signals, and both banks' inRow enable,
outCol, ready, rden, valid, and last boundaries. The export is preserved at:

```text
/home/xch/work/npu_lpnpu/tmp/plan3_step3_atbd_deep_boundary.csv
SHA-256 88e63775eccc9965dcf1b20dcaf31191ae3170b3c177ffb929b9dc0577379abd
```

Anchored to the accepted start, the frozen RTL observes data_A at edges
45..108, data_B at 77..108, T0 input rows at 46..77, T1 input rows at
78..109, T0 outCol prefetch/stream at 78..109, and the first non-consumed T1
outCol prefetch at 110.

`StrictPayloadDatapath` now exposes per-edge payload boundary transactions.
Operand payloads retain their driver edge; the transposer input boundary is
one registered edge later, matching `sa_feeder -> transposer_tiny`. Output
transactions identify the selected bank. When the final T0 column is
consumed, the runtime trace peeks at T1's first column on the following edge
without advancing its output counter. `TransposerTinyBank/Arbiter` therefore
gain const peek operations, distinct from consuming reads. `SauModel` emits
the first command's data_A/data_B and T0/T1 inRow/outCol values through the
existing boundary trace without changing the public architecture trace.

The payload focused test now checks operand edges and payloads, registered
bank-input edges and bank selection, all 32 T0 output columns, and the
non-consuming T1 prefetch. Syntax-only compilation of the transposer,
payload-datapath, test, and SauModel sources passes; style and
`git diff --check` also pass.

After the developer rebuilt the focused tests and `gem5.opt`,
`transposer.test` passed 11/11 and `payload_datapath.test` passed 3/3. The
first runtime comparison proved that all six payload sequences matched, while
also exposing that event-style data_A/data_B/inRow rows must not carry an
initial zero and that the runtime driver's post-tick edge is one later than
the RTL boundary-cycle coordinate. The boundary exporter now omits those
event initializers and maps payload events to `driverEdge - 1`, without
changing datapath state or statistics.

The rebuilt strict cutbit-8 ATBD run exited through `SAU command complete`.
`data_A`, `data_B`, T0/T1 inRow, and T0/T1 outCol then passed both sequence
and exact-cycles comparison against the deep RTL export. Runtime statistics
reported 64 transposer input rows, 32 consumed output columns, zero input
stalls, zero output stalls, zero payload underflows, and 32 cycles from first
row to first column. The complete SAU quick regression passed 63/63 checks
across 23 suites. Increment 10 is therefore verified.

### PLAN3 Step 3 scope closure — 2026-07-27

The user explicitly froze the current reuse support domain to
`reuse_mode=01` (Reuse-A) because the RTL project and its other reuse
interfaces are still evolving. `reuse_mode=00/10/11` continue to decode
losslessly; their existing interfaces, implementation fragments, tests, and
historical evidence are preserved. They are classified as deferred and must
not silently fall back to Reuse-A or be reported as validated.

Under this revised scope, R-none/R-B/R-AB golden capture and implementation
are not Step 3 blockers. The completed input/register-file, feeder,
transposer, Reuse-A runtime payload integration, statistics, ABTD module
boundary milestone, and deep ATBD payload/cycle comparison satisfy the
current Step 3 contract. PLAN3 Step 3 is closed; Step 4 (systolic-array and
fixed-point computation resources) is next.
