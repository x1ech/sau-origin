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

- Current stage: Tasks 1 through 7 complete, with a Task 4.5 calibration patch
  for RTL-observed A preload/B streaming behavior; Task 8 is next.
- Active branch: `feature/sau-command-types`
- Worktree: `/home/xch/workspace/gem5/.worktrees/sau-command-types`
- Development remote: `sau-origin`
- Latest completed milestone: the external read model now follows the
  RTL-observed A preload plus B streaming behavior, and an abstract
  `ARegisterFileIn` captures A-side reuse before Task 8 scheduling.
- First milestone scope: direct command injection, int8 GEMM, and 32-byte
  timing-memory beats.
- The memory contract was corrected on 2026-07-03 from a legacy 128-bit
  assumption to the active RTL's 256-bit interface.

## Task Progress

| Task | Status | Notes |
| --- | --- | --- |
| 1. Capture deterministic RTL timing reference | Complete | Imported and validated the RTL-side package from `/home/xch/workspace/sau_task1_baseline.tar.gz`. The available testcase is `INT8_SAU_MATMUL_TEST_ID_0` with matrix 64x256x256, not the originally requested 32x32x32. Public reference files live in `tests/gem5/sau/ref/int8_gemm_64x256x256/`. |
| 2. Add the common trace comparator | Complete | Adds `util/sau/compare_trace.py` and unit tests. Strict mode compares every normalized field; causal mode compares event order and metadata while allowing latency shifts with nondecreasing actual cycles. |
| 3. Add command types and admission validation | Complete | Commit `b61ec79f60`; defines stable command/token types and validates the first-milestone contract. |
| 4. Implement deterministic beat generation | Complete / calibrated | Commit `6b3fc7165a`; later calibrated after Task 1 so external reads are A preload once per instruction, then B streaming per flow. |
| 5. Implement token buffers and array timing | Complete / calibrated boundary | Bounded token storage with configurable fill latency, initiation interval, and in-flight capacity. Added the A register-file-in abstraction boundary so A external reads and A array inputs are no longer conflated. |
| 6. Add the SimObject, parameters, trace, and stats skeleton | Complete | Registers `SauModel`, its timing-memory port and parameters, a stable seven-column trace writer, synthetic command startup, and statistics placeholders. |
| 7. Implement the timing memory port | Complete | Sends exact 32-byte read/write packets, retains one rejected packet for retry, tracks accepted outstanding traffic, and queues read responses for the next SAU edge. |
| 8. Integrate scheduling, array timing, writeback, and drain | Pending | Main end-to-end behavioral model integration. |
| 9. Add fixed- and constrained-memory simulations | Pending | Covers deterministic latency, retry, and backpressure. |
| 10. Calibrate against the RTL reference | Blocked by model integration | No cycle-accuracy claim can be made until gem5 traces are generated and compared against the imported RTL reference. |
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

### A register-file-in abstraction

- `ARegisterFileIn` records Operand-A preload beats and reports when a command
  instruction's A data is resident.
- It models the architecture-relevant reuse boundary only; it does not model
  RTL register width, ports, or stored data values.
- It exposes virtual Operand-A array-input beats after preload. These beats use
  address zero because array input trace events are not external SRAM accesses.
- This fixes the old implicit assumption that every A array input required a
  fresh external SRAM read.
- Task 8 must wire this into the scheduler so B read responses can pair with
  reused A tokens at the array input boundary.

### Token buffering and array timing

- Capacity-limited FIFO token storage with explicit push availability.
- Configurable array fill latency, initiation interval, and maximum in-flight
  token count.
- No hidden cycle counter; the owner supplies the current SAU cycle.
- Ready tokens retain in-flight capacity until consumed.
- Timing values remain configurable pending RTL calibration.

### SimObject, trace, and statistics skeleton

- `SauModel` is a `ClockedObject` with a timing-memory request port and the
  configuration parameters required by the first milestone.
- Startup validates and accepts one synthetic int8 GEMM command, emits stable
  trace events, and schedules cycle ticks.
- The skeleton intentionally remains in `OperandLoad`; memory traffic and
  phase progression belong to Tasks 7 and 8.
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

### RTL timing baseline

- The RTL-side Task 1 package was received as
  `/home/xch/workspace/sau_task1_baseline.tar.gz` and unpacked under
  `/home/xch/workspace/sau_task1_baseline_unpack/sau_task1_baseline`.
- Full package hashes in `SHA256SUMS` were checked successfully from the package
  root.
- The public architecture trace, manifest, summary, diagnostic trace, analysis,
  handoff note, and checksum file were copied into
  `tests/gem5/sau/ref/int8_gemm_64x256x256/`.
- Important measured timing values for this 64x256x256 baseline:
  - command total: 2036 cycles;
  - first read: cycle 3;
  - first array input: cycle 77;
  - first result: cycle 324;
  - last array input: cycle 1689;
  - last result: cycle 1769;
  - first write: cycle 1777;
  - command complete: cycle 2036;
  - array fill latency: 247 cycles;
  - array drain latency: 80 cycles;
  - last result to first write: 8 cycles;
  - last write to complete: 4 cycles.
- The original task asked for 32x32x32, but the available RTL testcase is
  64x256x256 and was accepted as the current baseline. These absolute cycle
  counts must not be treated as a 32x32x32 calibration.

### Trace comparator

- `util/sau/compare_trace.py` reads the public seven-column SAU architecture
  trace and normalizes cycles to each trace's `command_accepted` row.
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

- SAU trace comparator tests: 7/7 passed.
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
- The handoff document and `manifest.json` disagree on the tracked working-tree
  diff SHA-256 (`0424c4...` vs `33f816...`). The package contents themselves
  passed `SHA256SUMS`, so this is recorded as a provenance metadata risk rather
  than a trace-integrity failure.
- The timing-memory port is implemented, but the scheduler does not issue
  command beats through it until Task 8.
- `ARegisterFileIn` is implemented as a reusable component but is not yet
  connected to `SauModel::tick()`; Task 8 must consume it.
- Outstanding-limit checks exist at the model boundary; Task 8 must prevent
  issue before a configured limit is exceeded and account the resulting stall.
- The skeleton does not yet integrate address generation, token buffering,
  array timing, phase progression, writeback, or command completion.
- `exit_on_done` becomes effective only after command completion is implemented.
- The address generator assumes the command has already passed admission
  validation.
- Current tests establish deterministic component behavior and trace comparison
  behavior, not calibrated RTL cycle-level timing accuracy.

## Next Steps

1. Implement Task 8 scheduler, array pipeline, writeback, phase progression,
   and drain behavior.
2. Add fixed- and constrained-memory simulations in Task 9.
3. Use `util/sau/compare_trace.py` to compare generated gem5 traces with the
   imported RTL baseline once Task 8 can emit end-to-end traces.
