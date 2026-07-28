# SAU Plan Index

Last updated: 2026-07-28

## Active Plan

- [`PLAN3.md`](PLAN3.md)：CSR 驱动的 Int8 GEMM 功能与数据通路 RTL 对齐。
- Current stage: Steps 0–5 complete; Step 6 increment 4 active in the frozen
  Reuse-A domain. Flow3 remains explicit fail-fast.
- Current checkpoint: strict raw fixtures predict each RTL
  command-done edge during construction and reject an overlapping next start
  before command statistics or trace events. Timing-memory raw replay rejects
  a busy start on its arrival edge; explicit `sequential` replay submits the
  next command only after completion/write visibility. A Yinglong firmware
  probe that omitted the first completion poll still produced starts only
  110/105 cycles after prior done edges: the integration crossbar serializes
  CPU CSR access while SAU owns the bus. Thus overlapping raw fixtures are
  invalid at the software-visible boundary; no claim is made about forced
  internal `SA_CORE` starts or the non-authoritative UVM environment. A four-command
  `[flow2, flow2, flow0, flow0]` chain proves command 4 reads the exact
  32-beat range written by command 3 after the final producer write, with
  1024/1024 RTL-matching final bytes. Optional `memory_dependencies` is
  verifier metadata over the existing trace and final-memory comparator, not
  a second functional model. Focused prediction, raw/sequential policy runs,
  and quick regression pass. Step 5 supports
  strict Flow0, Flow1, and Flow2 on
  the frozen Reuse-A path. K512 `[flow2, flow0]` and K768
  `[flow2, flow2, flow0]` both match all 1024 final bytes and exact RTL
  command/gap timing; only their final commands emit 32 result/write beats.
  This validates accumulator state across one and two consecutive retain
  boundaries. Resident Operand-A requests use `register_addr.sv`'s raw
  x/y/channel address program. Flow1 remains byte exact for all 4096 bytes
  and both 690-cycle command extents. The quick regression passes 75/75
  checks across 27 suites. The trace-disabled payload performance baseline is
  recorded in `docs/reports/plan3-step5-performance-baseline.md`; measured
  medians are 0.16–0.17 seconds and 65–69 MiB RSS across the frozen small,
  baseline, and K768 runs. Real-payload read/write stats are checked against
  architecture-trace events; the post-build quick regression passes 75/75.

Read the PLAN3 status and current Step first. Read other sections only when
the task needs their scope, dependencies, contracts, or acceptance criteria.

## Active Contracts

- [`PLAN3_STEP0.md`](PLAN3_STEP0.md)：PLAN3 pre-implementation RTL、CSR、
  resource 和 golden 冻结合同。
- [`RTL_TIMING_PROVENANCE.md`](RTL_TIMING_PROVENANCE.md)：当前权威 RTL
  来源、elaborated hierarchy 和文件 Hash。

These contracts remain active and are not historical archives.

## Historical Plans

- [Initial timing-model plan](docs/plans/archive/initial-timing-model.md)
- [PLAN2 CSR/timing-alignment plan](docs/plans/archive/plan2-csr-timing-alignment.md)
- [Plan archive index](docs/plans/INDEX.md)

Historical plans are complete or superseded and are not read by default.
Read a specific archive only when tracing scope, design rationale, verification
evidence, or a regression.
