# SAU Cycle-Level Behavioral Model Status

Last updated: 2026-07-03

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

## Current State

- Current stage: Tasks 3 through 5 complete; Task 6 is next.
- Active branch: `feature/sau-command-types`
- Worktree: `/home/xch/workspace/gem5/.worktrees/sau-command-types`
- Development remote: `sau-origin`
- Latest completed milestone: configurable token buffers and array timing.
- First milestone scope: direct command injection, int8 GEMM, and 32-byte
  timing-memory beats.
- The memory contract was corrected on 2026-07-03 from a legacy 128-bit
  assumption to the active RTL's 256-bit interface.

## Task Progress

| Task | Status | Notes |
| --- | --- | --- |
| 1. Capture deterministic RTL timing reference | Deferred | Requires an RTL simulator such as VCS or a compatible alternative. |
| 2. Add the common trace comparator | Deferred | Can be implemented independently, but meaningful differential comparison also needs Task 1 traces. |
| 3. Add command types and admission validation | Complete | Commit `b61ec79f60`; defines stable command/token types and validates the first-milestone contract. |
| 4. Implement deterministic beat generation | Complete | Commit `6b3fc7165a`; cursor-based B-then-A generation with flow and instruction strides. |
| 5. Implement token buffers and array timing | Complete | Bounded token storage with configurable fill latency, initiation interval, and in-flight capacity. |
| 6. Add the SimObject, parameters, trace, and stats skeleton | Pending | Depends on the core command/address/token components. |
| 7. Implement the timing memory port | Pending | Must model timing requests, responses, retry, and outstanding limits. |
| 8. Integrate scheduling, array timing, writeback, and drain | Pending | Main end-to-end behavioral model integration. |
| 9. Add fixed- and constrained-memory simulations | Pending | Covers deterministic latency, retry, and backpressure. |
| 10. Calibrate against the RTL reference | Blocked by Tasks 1 and 2 | No cycle-accuracy claim can be made until RTL traces are available and compared. |
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
- Deterministic order for every instruction and flow:

  ```text
  all Operand-B beats -> all Operand-A beats
  ```

- Address formula:

  ```text
  base
  + instruction * instructionStrideBytes
  + flow * flowStrideBytes
  + beat * strideBytes
  ```

- `Beat.index` resets for each stream occurrence.
- `Beat.last` marks the final beat of each stream occurrence.
- The accepted command is copied into the generator, so later caller-side
  changes cannot alter an active sequence.

### Token buffering and array timing

- Capacity-limited FIFO token storage with explicit push availability.
- Configurable array fill latency, initiation interval, and maximum in-flight
  token count.
- No hidden cycle counter; the owner supplies the current SAU cycle.
- Ready tokens retain in-flight capacity until consumed.
- Timing values remain configurable pending RTL calibration.

## Verification

Most recent focused verification:

```bash
python3 util/style.py --modifications \
    src/sau/SConscript \
    src/sau/token_pipeline.hh \
    src/sau/token_pipeline.cc \
    src/sau/token_pipeline.test.cc

scons build/ALL/sau/address_generator.test.opt \
      build/ALL/sau/command.test.opt \
      build/ALL/sau/token_pipeline.test.opt -j4

./build/ALL/sau/address_generator.test.opt
./build/ALL/sau/command.test.opt
./build/ALL/sau/token_pipeline.test.opt
```

Results:

- gem5 style check: passed.
- Address generator tests: 3/3 passed.
- Command validation tests: 14/14 passed, including rejection of the legacy
  16-byte beat size.
- Token buffer and array pipeline tests: 5/5 passed.
- Build warnings about unavailable Capstone and HDF5 are unrelated to the SAU
  unit tests.

## Known Gaps and Risks

- No RTL reference trace has been captured yet.
- No common trace comparator is implemented yet.
- The current model has no memory port, retry/backpressure integration,
  scheduler, writeback, statistics, or end-to-end SimObject.
- The address generator assumes the command has already passed admission
  validation.
- Current tests establish deterministic component behavior, not calibrated
  RTL cycle-level timing accuracy.

## Next Steps

1. Implement Task 6: the SimObject, parameter, trace, and statistics skeleton.
2. Continue with timing-memory integration in Tasks 7-9.
3. Return to Tasks 1 and 2 before Task 10 calibration.
