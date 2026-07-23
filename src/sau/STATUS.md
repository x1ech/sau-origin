# SAU Cycle-Level Behavioral Model Status

Last updated: 2026-07-23

## Goal

Build a cycle-level behavioral model of the Systolic Array Unit (SAU) in
gem5 for system-level performance analysis and design-space exploration.
The model targets architecture-relevant cycle timing rather than RTL
register-level equivalence, and the first milestone does not perform
arithmetic computation.

## Session Handoff Checkpoint — 2026-07-23

Read this section first when resuming Step 5.5 in a new session.

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
non-strict DSE schedulers. It awaits developer relinking and regression.
Step 5.5 still needs new-current-trace acceptance.

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

- Current stage: Tasks 1 through 11 implementation and validation complete.
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

## Task Progress

| Task | Status | Notes |
| --- | --- | --- |
| 1. Capture deterministic RTL timing reference | Superseded / recapture required | The 2026-07-07 packages are retained as historical diagnostics but are no longer golden. The current 64x256x256 `yinglong` run establishes internal edge provenance; new acceptance packages still need capture. |
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
| PLAN2. CSR-driven Int8 GEMM RTL alignment | Step 5.5 in progress / baseline reset | Current-RTL scheduler and feeder/execute isolated skeletons are updated. The old five coverage and three hold-out results are historical; all eight profiles have been removed from quick strict registration until current-baseline packages are captured. |

## Retired PLAN2 RTL Fixture Inventory

These packages are historical diagnostics, not current golden oracles. They
used the fixed PLAN2 control contract: int8 GEMM,
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

1. When new current-baseline packages are available, use them for independent
   architecture/state strict acceptance; do not compare against the old eight
   traces as golden.
2. After Step 5.5 and new-trace acceptance, resume timing-memory causal,
   backpressure, DSE, and legacy direct-command regression.
