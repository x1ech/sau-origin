# SAU Cycle-Level Behavioral Model Status

Last updated: 2026-07-07

## Goal

Build a cycle-level behavioral model of the Systolic Array Unit (SAU) in
gem5 for system-level performance analysis and design-space exploration.
The model targets architecture-relevant cycle timing rather than RTL
register-level equivalence, and the first milestone does not perform
arithmetic computation.

Design and implementation references:

- [Design specification](../../docs/superpowers/specs/2026-07-02-sau-cycle-level-model-design.md)
- [Implementation plan](../../docs/superpowers/plans/2026-07-02-sau-cycle-level-model.md)
- [Chinese implementation plan](../../docs/superpowers/plans/2026-07-02-sau-cycle-level-model-zh.md)
- [Task 1 RTL baseline handoff plan](../../docs/superpowers/plans/2026-07-06-sau-task1-rtl-baseline-handoff-zh.md)

## Current State

- Current stage: Tasks 1 through 9 complete, with Task 4.5 and Task 8.5
  calibration patches for RTL-observed A preload/B streaming, skewed/bursty
  A/B array input, reduced result production, delayed writeback, and command
  completion timing.
- Active branch: `feature/sau-command-types`
- Worktree: `/home/xch/workspace/gem5/.worktrees/sau-command-types`
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
- Latest simulation milestone: Task 9 adds a standalone
  `configs/example/sau_timing.py` topology with `SauModel`, `SystemXBar`, and
  `SimpleMemory`, plus quick tests for fixed and constrained timing-memory
  configurations.
- The current direct-command model assumes the target operator uses the RTL
  `register_file_in` path; CSR decode of reuse/control fields is not yet
  modeled.
- First milestone scope: direct command injection, int8 GEMM, and 32-byte
  timing-memory beats.
- The memory contract was corrected on 2026-07-03 from a legacy 128-bit
  assumption to the active RTL's 256-bit interface.

## Task Progress

| Task | Status | Notes |
| --- | --- | --- |
| 1. Capture deterministic RTL timing reference | Complete / refreshed | Re-imported and validated the corrected RTL-side package from `/home/xch/workspace/sau_task1_baseline.tar.gz` on 2026-07-07. The testcase is `INT8_SAU_MATMUL_TEST_ID_0` with matrix 64x256x256, not the originally requested 32x32x32. Public reference files live in `tests/gem5/sau/ref/int8_gemm_64x256x256/`. |
| 2. Add the common trace comparator | Complete | Adds `util/sau/compare_trace.py` and unit tests. Strict mode compares every normalized field; causal mode compares event order and metadata while allowing latency shifts with nondecreasing actual cycles. |
| 3. Add command types and admission validation | Complete | Commit `b61ec79f60`; defines stable command/token types and validates the first-milestone contract. |
| 4. Implement deterministic beat generation | Complete / calibrated | Commit `6b3fc7165a`; later calibrated after Task 1 so external reads are A preload once per instruction, then B streaming per flow. |
| 5. Implement token buffers and array timing | Complete / calibrated boundary | Bounded token storage with configurable fill latency, initiation interval, and in-flight capacity. Added the A register-file-in abstraction boundary so A external reads and A array inputs are no longer conflated. |
| 6. Add the SimObject, parameters, trace, and stats skeleton | Complete | Registers `SauModel`, its timing-memory port and parameters, a stable seven-column trace writer, synthetic command startup, and statistics placeholders. |
| 7. Implement the timing memory port | Complete | Sends exact 32-byte read/write packets, retains one rejected packet for retry, tracks accepted outstanding traffic, and queues read responses for the next SAU edge. |
| 8. Integrate scheduling, array timing, writeback, and drain | Complete | `SauModel::tick()` now consumes read responses, feeds `ARegisterFileIn` plus B streaming tokens into the array pipeline, produces output tokens, issues timing writes, advances phases, checks token/request conservation, and drains only after local packet state is clear. |
| 8.5. Calibrate matmul transpose/reuse timing path | Complete | Adds `ArrayInputScheduler` burst/gap timing, `ResultScheduler`, reduced-output command validation, first-array-input anchoring, delayed writeback, and final completion delay. Defaults match the corrected 64x256x256 trace shape: A start 269, B skew 32, input burst 32, tile gap 1, flow gap 3, fill 343, result flow gap 234, writeback delay 8, completion delay 4. |
| 9. Add fixed- and constrained-memory simulations | Complete | Adds `configs/example/sau_timing.py` and `tests/gem5/sau/test_sau.py`. Fixed memory completes at SAU cycle 8477; constrained memory completes at SAU cycle 76617 with retry, outstanding-limit, and input-starvation stalls. |
| 10. Calibrate against the RTL reference | Pending trace-profile alignment | Task 9 now generates gem5 traces, but causal comparison still fails because the standalone synthetic profile emits one command with synthetic addresses while the imported RTL reference has two commands and RTL baseline addresses. |
| 11. Final regression, statistics audit, and documentation | Pending | Final milestone validation and handoff. |

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
  1GHz default SAU clock, `SystemXBar(width=32)`, `SimpleMemory`, and one
  synthetic `SauModel` command.
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
  `py_compile`: passed.
- `git diff --check`: passed.
- `build/RISCV/gem5.opt`: rebuilt successfully after the Task 9 scheduler
  conservation fix.
- Fixed standalone run completed with `SAU command complete`, wrote
  `m5out/sau-fixed/sau.csv`, and completed at SAU cycle 8477.
- Constrained standalone run completed with `SAU command complete`, wrote
  `m5out/sau-constrained/sau.csv`, and completed at SAU cycle 76617.
- Constrained standalone stats showed real pressure:
  `stallRequestRetry=2559`, `stallOutstandingReadLimit=9125`,
  `stallOutstandingWriteLimit=127`, and `stallInputStarvation=71833`.
- `cd tests && ./main.py run --skip-build gem5/sau`: 4/4 checks passed for
  the RISCV fixed and constrained quick tests.
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
- All focused SAU tests: 27/27 passed.
- `build/RISCV/gem5.opt`: built successfully.
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
- Causal comparison of the Task 9 generated traces against
  `tests/gem5/sau/ref/int8_gemm_64x256x256/architecture.csv` currently fails.
  The immediate mismatches are expected at this stage: the RTL reference has
  two commands and RTL baseline addresses, while the standalone gem5 config
  currently emits one synthetic command with synthetic addresses. Aligning that
  profile is the first part of Task 10.
- `system.sau.commandsAccepted` currently dumps as 0 while
  `commandsCompleted` dumps as 1 in the standalone stats because the synthetic
  command is accepted during startup before the stats dump window records it.
  Task 11 should audit and fix or document this statistics boundary.
- Current tests establish deterministic component behavior, trace comparison
  behavior, compile-time integration of the scheduler, and standalone
  completion under fixed/constrained memory. They do not yet establish
  calibrated RTL cycle-level timing accuracy.

## Next Steps

1. Align the standalone synthetic profile with the imported two-command RTL
   reference shape and addresses.
2. Use `util/sau/compare_trace.py` in causal mode, then strict mode, as part
   of Task 10 calibration.
