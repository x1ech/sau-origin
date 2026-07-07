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

- Current stage: Tasks 1 through 8 complete, with a Task 4.5 calibration patch
  for RTL-observed A preload/B streaming behavior.
- Active branch: `feature/sau-command-types`
- Worktree: `/home/xch/workspace/gem5/.worktrees/sau-command-types`
- Development remote: `sau-origin`
- Latest completed milestone: `SauModel` now schedules external reads,
  RTL-style `register_file_in` reuse for Operand-A, B streaming, array timing,
  output writeback,
  phase progression, command completion, and drain behavior.
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
| 8.5. Calibrate matmul transpose/reuse timing path | Pending | Corrected RTL baseline shows high-frequency `TRANSPOSE_LOAD` and `TRANSPOSE_CLIP` states; current scheduler still lacks this timing policy and same-cycle A/B input pairing is too simple. |
| 9. Add fixed- and constrained-memory simulations | Pending after Task 8.5 | Covers deterministic latency, retry, and backpressure after the corrected matmul transpose/reuse timing path is represented. |
| 10. Calibrate against the RTL reference | Pending Task 9 traces | No cycle-accuracy claim can be made until gem5 traces are generated and compared against the imported RTL reference. |
| 11. Final regression, statistics audit, and documentation | Pending | Final milestone validation and handoff. |

## Implemented Components

### Command boundary

- `SauCommand`, stream descriptors, beats, pipeline tokens, phases, and event
  kinds.
- Admission validation for:
  - int8 GEMM and 32-byte beats;
  - aligned stream bases;
  - nonzero beats, strides, loops, and work items;
  - consistent work-item and output-beat counts; and
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
- Array input admission requires resident A for the current instruction, one
  available B beat, array initiation-interval/capacity availability, and output
  buffer headroom.
- Array input trace events are currently emitted as paired B then A events.
  This is now known to be an oversimplification for the corrected RTL baseline:
  A and B array-input windows are skewed by about 32 cycles due to the
  matmul transpose/reuse path.
- Results from `ArrayPipeline` become output tokens; writes use the synthetic
  output stream address formula.
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
python3 util/style.py --modifications \
    src/sau/SConscript \
    src/sau/memory_port.hh \
    src/sau/memory_port.cc \
    src/sau/memory_port.test.cc \
    src/sau/sau_model.hh \
    src/sau/sau_model.cc

scons build/RISCV/gem5.opt -j4

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
  scheduler validation should be covered after Task 8.5 and Task 9.
- The Task 8 scheduler currently admits A/B array inputs as paired events. This
  is insufficient for the corrected RTL baseline because the matmul
  `TRANSPOSE_LOAD/CLIP` path creates staggered A/B array-input windows.
- Current tests establish deterministic component behavior, trace comparison
  behavior, and compile-time integration of the scheduler. They do not yet
  establish calibrated RTL cycle-level timing accuracy.

## Next Steps

1. Complete Task 8.5: calibrate and model the matmul
   `TRANSPOSE_LOAD/TRANSPOSE_CLIP` timing path.
2. Add fixed- and constrained-memory simulations in Task 9.
3. Use `util/sau/compare_trace.py` to compare Task 9 generated gem5 traces
   with the imported RTL baseline.
