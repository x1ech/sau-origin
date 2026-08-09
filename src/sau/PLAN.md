# SAU Plan Index

Last updated: 2026-08-09

## Active Plan

- [`PLAN3.md`](PLAN3.md)：CSR 驱动的 Int8 GEMM 功能与数据通路 RTL 对齐。
- Current stage: Steps 0–5 complete; Step 6 increment 13 complete in the frozen
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
  a second functional model. Increment 5 promotes the existing ABTD Step-0
  boundary package from a count smoke test to the shared comparator's exact
  payload/cycle regression; the rebuilt focused test passes. The ATBD and
  ABTD packages now also have an executable pair contract proving identical
  input memory/configuration except `trans_mode=1→2` and distinct RTL final
  memory. At increment 5, full ABTD runtime was blocked on moving the proven
  `input_switch`/feeder mux behavior into the generic payload path: the
  current RTL's Reuse-A quirk gates transposer loading with B-valid while
  selecting resident A payload. Increment 6 wires the driver's delayed
  `outputInputSwitch()` into a registered generic payload mux and preserves
  the next-edge bank-load timing; the full payload focused set passes 5/5.
  Increment 7 applies frozen `sa_feeder.sv`'s raw switch-01 array routing:
  the mixed transposer column is activation and registered B is weight. Its
  extended focused test passes with the full payload set at 5/5. Increment 8
  adds reproducible ABTD CSR replay, explicitly inferring fixed firmware write
  order because `csr_we/csr_addr` were omitted. Paired ATBD proves sampled
  `csr_wdata` is setup and acceptance is the next posedge. Increment 9 matches
  result 157–188, write 196–227, done 231; schedule passes 38/38. Increment 10
  adds the missing feeder output-state gate so ABTD B-valid is one resident
  tail plus 32 streamed rows, not 64; schedule passes 38/38. Increment 11
  maps early B to resident tail, consumes 32 streamed rows, and rejects final
  overflow; payload passes 5/5. Increment 12 follows frozen `sa_feeder.sv`:
  pre-ready SA uses zero activation and the final uses the real column; payload
  passes 5/5. Increment 13 opens ABTD/R-A/Flow0; its linked run reaches all 32
  RTL result/write edges, matches cycle 231 and all 1024 RTL bytes, and keeps
  the relinked ATBD strict traces passing through an ABTD-only admission guard.
  Prediction/admission/quick regression pass; Reuse-A supports Flow0/1/2.
  K512 `[flow2, flow0]` and K768 `[flow2, flow2, flow0]` match 1024 bytes and RTL
  command/gap timing; only their final commands emit 32 result/write beats.
  This validates accumulator state across one and two consecutive retain
  boundaries. Resident Operand-A requests use `register_addr.sv`'s raw
  x/y/channel address program. Flow1 remains byte exact for all 4096 bytes
  and both 690-cycle command extents. The quick regression passes 78/78
  checks across 28 suites. The trace-disabled payload performance baseline is
  recorded in `docs/reports/plan3-step5-performance-baseline.md`; measured
  medians are 0.16–0.17 seconds and 65–69 MiB RSS across the frozen small,
  baseline, and K768 runs. Real-payload read/write stats are checked against
  architecture-trace events; the post-build quick regression passes 78/78.

Read PLAN3's status/current Step first; read other sections only for needed
scope, dependencies, contracts, or acceptance criteria.

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

Historical plans are not read by default; use a specific archive only for
scope, rationale, verification evidence, or regression tracing.
