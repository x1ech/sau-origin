# SAU Cycle-Level Timing and Functional Datapath Model

`SauModel` is a standalone gem5 model of the Systolic Array Unit. The
completed PLAN2 baseline models CSR-driven int8-GEMM command admission,
32-byte timing-memory traffic, Reuse-A scheduling, strict RTL-visible timing,
backpressure, writeback, command drain, and DSE controls. Active PLAN3 work
extends that baseline with real payloads and finite datapath resources.

This is an architecture-relevant cycle model, not a register-accurate RTL
replacement.

## Current status and documentation

- [`PLAN.md`](PLAN.md) is the short plan entry.
- [`STATUS.md`](STATUS.md) is the current handoff snapshot.
- [`PLAN3.md`](PLAN3.md) is the frozen active functional-datapath plan.
- [`PLAN3_STEP0.md`](PLAN3_STEP0.md) and
  [`RTL_TIMING_PROVENANCE.md`](RTL_TIMING_PROVENANCE.md) define the active
  RTL/resource contract and authoritative sources.
- [`docs/workflows/run-yinglong-sau-rtl.md`](docs/workflows/run-yinglong-sau-rtl.md)
  records the repeatable case-generation, firmware-build, Yinglong SAU RTL
  simulation, and FSDB inspection workflow.

Historical plans and status logs are indexed under `docs/plans/` and
`docs/status/`; they are not read by default.
The reproducible Step 5 host-performance baseline is recorded in
[`docs/reports/plan3-step5-performance-baseline.md`](docs/reports/plan3-step5-performance-baseline.md).

## Scope and current limitation

PLAN3 Steps 0–5 are complete for the currently frozen Reuse-A domain
(`reuse_mode=01`). The model now carries real 256-bit memory payloads through
the input/register resources and finite transposer, and carries signed-int8
operands through a finite 32x32 systolic array with signed 24-bit saturating
PE accumulation and CSR-driven `cutbit=0..31`. The supported ATBD/Reuse-A
runtime array inputs and registered result rows match the frozen RTL in
payload, sequence, and exact cycles.

PLAN3 Step 5 is complete in that domain. The output data resources implement a finite
32-row T2 result serializer, flow-mode-1 transpose ordering, raw nested output-RF
addressing, two SRAM halves, normal/retain arithmetic, same-address
forwarding, registered unload, external `register_addr` sequencing, and
signed-int8 256-bit payload assembly. Increment 2 focused build/test passes
11/11.

Increment 3 connects array rows, driver result-valid edges, output-RF updates,
and registered unload payloads to the strict runtime. Increment 4 consumes
that native FIFO at strict writeback, checks token/address/last agreement,
and submits the real bytes to strict `FunctionalMemory`; timing-memory/DSE
interfaces retain their prior behavior. The cutbit-8 and cutbit-1 strict
checkpoints both match their frozen RTL final memory for all 1024 bytes.
Increment 5 enables strict Flow1 plus multi-flow/multi-instruction array
tiles. The two-command 64x160x64 fixture matches all 4096 final bytes and both
690-cycle RTL command extents. Strict Flow2 is also end-to-end validated by
the K512 `[flow2, flow0]` fixture: command extents are 569/685 cycles with a
93-cycle gap, only the final command emits 32 result/write beats, and all
1024 final bytes match. CSR-driven resident Operand-A reads use the shared
`register_addr.sv` x/y/channel address program, including non-contiguous
`yStep` layouts. The K768 `[flow2, flow2, flow0]` fixture further proves two
consecutive retain boundaries. The quick regression passes 72/72 checks
across 26 suites.
Broader mode coverage is not complete,
so PLAN3 does not yet claim full-domain end-to-end correctness. The legacy
direct-command/DSE path and completed PLAN2 regressions remain timing-model
interfaces; do not interpret their placeholder output bytes as PLAN3
functional results.

The 64x160x64 Flow1 RTL fixture is packaged under
`tests/gem5/sau/functional_ref` and registered as a byte-exact quick functional
test. Flow1 reproduces the observed RTL tile-local clockwise rotation; it does
not impose a mathematical whole-matrix transpose.

The 32x512x32 Flow2 fixture in the same directory is a permanent byte-exact
and strict-timing regression. Its verifier checks final memory, both command
extents, the inter-command gap, and per-command result/write counts.
The 32x768x32 fixture extends that contract to `[flow2, flow2, flow0]`,
proving accumulator state survives two consecutive retain boundaries; its
three command extents are 569/569/685 cycles and all 1024 bytes match.

`reuse_mode=00/10/11` remains decoded but is deferred while the RTL evolves.
Complete workloads selecting those paths must fail explicitly rather than
silently falling back to Reuse-A.

## Build

Builds are developer-owned because of the environment's resource limits.
Codex should provide the smallest relevant command and wait for the reported
result. From the gem5 worktree, the current optimized build form is:

```bash
scons build/RISCV/gem5.opt \
    --ignore-style --limit-ld-memory-usage -j32
```

## PLAN2 RTL CSR fixture replay

The eight profiles under `tests/gem5/sau/ref` were recaptured from the current
`npu_lpnpu` RTL on 2026-07-24. Their source artifacts, simulator identity, and
hashes are recorded in each package manifest. They are the current strict
architecture and semantic-state acceptance fixtures for the completed PLAN2
timing/control baseline. They do not by themselves prove PLAN3 final payload
or final-memory correctness.

```bash
./build/RISCV/gem5.opt \
    --outdir=m5out/sau-rtl-strict \
    configs/example/sau_timing.py \
    --rtl-profile tests/gem5/sau/ref/int8_gemm_64x256x256_baseline \
    --trace=m5out/sau-rtl-strict/sau.csv

python3 util/sau/compare_trace.py --mode strict \
    tests/gem5/sau/ref/int8_gemm_64x256x256_baseline/architecture.csv \
    m5out/sau-rtl-strict/sau.csv
```

This produces exit cause `SAU command complete`; a successful comparator emits
no output. The profile path replays `csr_writes.csv`, loads named RTL
elaboration parameters from `manifest.json`, and writes
`sau_timing_ledger.csv` alongside the trace. It rejects all timing overrides.

Strict mode models only the RTL-visible SRAM contract: one shared 256-bit
(32-byte) read/write request port, at most one request per SAU cycle, read
priority, ordered responses, and fixed read visibility at
`accepted_cycle + SRAM_DELAY + 1`. Operand-A is externally preloaded before
Operand-B requests and becomes resident in `ARegisterFileIn` before array
execution. SRAM banks, crossbar contention, retry, and variable latency are
intentionally reserved for non-strict system-memory runs.

The validated PLAN2 timing/control domain is int8 GEMM with `trans_mode=01`
and `reuse_mode=01`. Five coverage shapes (32x32x32, 64x32x256, 64x256x32,
32x256x256, and 64x256x256) were used to derive and freeze the structural
rules. Three independently accepted hold-outs (96x256x256, 64x128x256, and
64x256x128) then passed without fixture-specific timing adjustment.

All eight packages are registered as RISC-V quick strict and timing-memory
causal tests. Together with the Flow1 and Flow2 functional fixtures, permanent legacy
direct-command regression, fixed/constrained runs, and DSE simulations, the
full quick set contains 26 suites and currently passes 72/72 checks:

```bash
cd tests
./main.py run --skip-build gem5/sau
```

## Inspect semantic schedule states

Strict fixture runs also write `sau_state.csv` without changing the public
seven-column architecture trace. It records semantic-state transitions,
their mapped RTL state family, the modeled input switch, and the transition
cause. Compare it with the RTL diagnostic projection when debugging a timing
divergence:

```bash
python3 util/sau/compare_state_trace.py --rtl-diagnostic \
    tests/gem5/sau/ref/int8_gemm_64x256x256_baseline/diagnostic.csv \
    m5out/sau-rtl-strict/sau_state.csv

python3 util/sau/validate_fixture.py tests/gem5/sau/ref
```

The state comparator is intentionally stricter than the architecture trace:
it identifies the first differing semantic state, input switch, or transition
cause. A mismatch is diagnostic evidence, not a reason to add a profile delay.

## Run the constrained system-memory profile

Without `--rtl-profile`, SAU is an explicitly non-strict direct-command/DSE
run. It sends requests through `SystemXBar` and `SimpleMemory`, so retry,
latency variance, bandwidth, and outstanding limits can affect timing.

To replay a supported CSR fixture through that timing-memory path, combine
`--rtl-profile FIXTURE` with `--timing-memory`. Causal comparison preserves
each command-local event/address/beat lane, the phase-transition sequence, and
explicit request/response/token dependencies. A data event may observe a
different instantaneous phase when backpressure changes otherwise-independent
event interleaving.

The quick suite also retains the original two-command 64x256x256
direct-command profile as `sau-legacy-direct-command`. It must remain causally
equivalent to the current baseline package, including exact event, address,
beat, and token counts. It is intentionally not a strict-cycle oracle:
post-Step-5.5 strict timing is owned by CSR replay and the per-tick RTL command
driver, while direct-command timing remains the aggregate non-strict DSE path.

```bash
./build/RISCV/gem5.opt \
    --outdir=m5out/sau-constrained \
    configs/example/sau_timing.py \
    --memory-latency=20ns \
    --memory-latency-var=5ns \
    --memory-bandwidth=1GiB/s \
    --max-outstanding-reads=2 \
    --max-outstanding-writes=2 \
    --trace=m5out/sau-constrained/sau.csv
```

## Important timing parameters

`configs/example/sau_timing.py` exposes the model parameters as command-line
options. The main DSE controls are:

- `--read-issue-width`, `--write-issue-width`,
  `--max-outstanding-reads`, and `--max-outstanding-writes`: memory-side
  issue and response pressure.
- `--input-buffer-entries`, `--output-buffer-entries`: B-token and result
  FIFO capacities.
- `--array-fill-cycles`, `--array-ii-cycles`, and `--array-capacity`:
  array latency, initiation interval, and in-flight work capacity.
- `--array-input-*`, `--result-flow-gap-cycles`,
  `--writeback-start-delay-cycles`, `--completion-delay-cycles`, and
  `--command-start-cycles`: calibrated command-local scheduling boundaries.

The default 256-slot output FIFO holds the complete 256-result calibrated
stream, so writeback waits for its final result as required by the RTL trace.
For DSE, a FIFO smaller than the command's total output count begins draining
after tokens are present; otherwise the old all-results-before-writeback rule
would deadlock. This streaming-FIFO behavior is intentionally not suitable
for strict RTL comparison or the current causal comparator's
result-before-writeback dependency.

All timing knobs in this section are DSE controls. Their use prints
`non-strict direct-command/DSE`; they must not be combined with
`--rtl-profile CSR_FIXTURE`.

## Functional datapath architecture

The active PLAN3 resource path is:

```text
CSR start
  -> raw address programs and memory requests
  -> FunctionalMemory / timing-memory payload response
  -> register_file_in, feeder, reuse, and finite transposer
  -> finite 32x32 signed-int8 systolic array
  -> registered result-row boundary
  -> register_file_out and result serializer
  -> real 256-bit memory writeback                  [Step 5 complete]
```

The implementation advances finite resources through accepted transactions
and explicit per-cycle state. It must not replace the path with a direct
`C=A*B` calculation. Strict fixed-SRAM runs and timing-memory runs each have
one explicit data authority; the timing-memory path may add explainable
latency, retry, queue, outstanding, and backpressure cycles.

The completed PLAN2 scheduler, resident/stream address generators, state
projection, native SRAM transport, and timing ledger remain the regression
baseline. PLAN3 reuses those accepted boundaries while replacing placeholder
data behavior with typed payload resources. Refer to `STATUS.md` for the
current Step 6 scope checkpoint rather than storing increment-by-increment
history in this README.

## Statistics

`m5out/.../stats.txt` reports `system.sau.*`, including:

- command counts; total `commandCycles`; and operand-load, array-active,
  array-drain, and writeback phase cycles;
- accepted read/write requests and bytes;
- strict real-payload beats as `payloadReadBeats` and `payloadWriteBeats`;
  functional regressions cross-check them against visible read responses and
  accepted writes in the architecture trace;
- time-average and maximum outstanding read/write counts and input/output
  FIFO occupancies;
- array activity and `arrayUtilization`;
- retry, outstanding-limit, input-starvation, output-FIFO-full, and array
  capacity stalls; and
- `firstReadOffset`, `firstArrayInputOffset`, `firstResultOffset`,
  `lastResultOffset`, `lastWriteOffset`, and `completeOffset`. These are
  per-command vectors indexed from zero and measured from that command's
  `command_accepted` event.

## Task-11 DSE monotonicity check

Run the four small configurations below, then compare their `stats.txt`
files. The `array_capacity=1` run must record array-capacity stalls; the
one-entry output FIFO must record output-full stalls. Increasing either
resource must not increase `commandCycles`.

```bash
./build/RISCV/gem5.opt --outdir=m5out/sau-dse-cap1 \
    configs/example/sau_timing.py --a-beats=1 --b-beats=1 \
    --output-beats=16 --flow-loops=16 --array-fill-cycles=2 \
    --array-input-start-delay-cycles=0 --array-input-burst-beats=1 \
    --array-input-burst-gap-cycles=0 --array-input-flow-gap-cycles=0 \
    --result-flow-gap-cycles=0 --writeback-start-delay-cycles=0 \
    --array-capacity=1 --output-buffer-entries=16

./build/RISCV/gem5.opt --outdir=m5out/sau-dse-cap16 \
    configs/example/sau_timing.py --a-beats=1 --b-beats=1 \
    --output-beats=16 --flow-loops=16 --array-fill-cycles=2 \
    --array-input-start-delay-cycles=0 --array-input-burst-beats=1 \
    --array-input-burst-gap-cycles=0 --array-input-flow-gap-cycles=0 \
    --result-flow-gap-cycles=0 --writeback-start-delay-cycles=0 \
    --array-capacity=16 --output-buffer-entries=16

./build/RISCV/gem5.opt --outdir=m5out/sau-dse-out1 \
    configs/example/sau_timing.py --a-beats=1 --b-beats=1 \
    --output-beats=16 --flow-loops=16 --array-fill-cycles=2 \
    --array-input-start-delay-cycles=0 --array-input-burst-beats=1 \
    --array-input-burst-gap-cycles=0 --array-input-flow-gap-cycles=0 \
    --result-flow-gap-cycles=0 --writeback-start-delay-cycles=0 \
    --array-capacity=16 --output-buffer-entries=1 \
    --memory-latency=20ns --max-outstanding-writes=1

./build/RISCV/gem5.opt --outdir=m5out/sau-dse-out8 \
    configs/example/sau_timing.py --a-beats=1 --b-beats=1 \
    --output-beats=16 --flow-loops=16 --array-fill-cycles=2 \
    --array-input-start-delay-cycles=0 --array-input-burst-beats=1 \
    --array-input-burst-gap-cycles=0 --array-input-flow-gap-cycles=0 \
    --result-flow-gap-cycles=0 --writeback-start-delay-cycles=0 \
    --array-capacity=16 --output-buffer-entries=8 \
    --memory-latency=20ns --max-outstanding-writes=1

python3 util/sau/verify_dse.py \
    --array-capacity-1=m5out/sau-dse-cap1/stats.txt \
    --array-capacity-16=m5out/sau-dse-cap16/stats.txt \
    --output-buffer-1=m5out/sau-dse-out1/stats.txt \
    --output-buffer-8=m5out/sau-dse-out8/stats.txt
```
