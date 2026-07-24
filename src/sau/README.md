# SAU Cycle-Level Timing Model

`SauModel` is a standalone gem5 timing model for the SAU's first milestone.
It models synthetic int8-GEMM command admission, 32-byte timing-memory
traffic, Operand-A resident reuse, Operand-B streaming, array timing, output
writeback, and command drain. It is a performance model, not a
register-accurate replacement for the RTL.

## Scope and limitation

The model emits timing events and makes write requests, but it does not
perform GEMM arithmetic. **First-stage writeback data is always zero; output
contents are not claimed to be functionally correct.** Use the trace and
statistics for timing/DSE work, not for numerical result validation.

## Build

From the gem5 worktree, build the RISC-V optimized binary incrementally:

```bash
scons --ignore-style build/RISCV/gem5.opt -j4
```

## Current RTL CSR fixture replay

The eight profiles under `tests/gem5/sau/ref` were recaptured from the current
`npu_lpnpu` RTL on 2026-07-24. Their source artifacts, simulator identity, and
hashes are recorded in each package manifest. They are the current strict
architecture and semantic-state acceptance fixtures.

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

The validated control domain is int8 GEMM with `trans_mode=01` and
`reuse_mode=01`. Five coverage shapes (32x32x32, 64x32x256, 64x256x32,
32x256x256, and 64x256x256) were used to derive and freeze the structural
rules. Three independently accepted hold-outs (96x256x256, 64x128x256, and
64x256x128) then passed without fixture-specific timing adjustment.

All eight packages are registered as RISC-V quick strict and timing-memory
causal tests. Together with the permanent legacy direct-command regression,
fixed/constrained runs, and DSE simulations, the full quick set contains 23
suites and currently passes 63/63 checks:

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

## Current RTL per-tick work

Step 5.5 replaced the retired aggregate strict timing formulas with
source-driven per-edge components. The scheduler, resident/stream address
generators, resident fill, SA-enable/execute path, result serializer, output
writeback, and native SRAM transport are integrated into strict
`SauModel` execution through the command-local driver in
`schedule_state.{hh,cc}`.

Implementation history: the developer-built focused checkpoint reached 30/30
passing tests, including
the input-RF readout and feeder A/B-valid skeleton. A combined
`RtlCommandDriverSkeleton` and two tests are now in the source tree and pass
static checks. The first rebuild passed 31/32 and exposed a missing registered
feeder A/B-arbiter gate. That fix corrected the first B edge; a second run
then exposed an extra skeleton-only core-state gate dropping B tokens across
`D_OUT`. Removing the extra gate produced a 32/32 passing focused checkpoint.
The first strict-runtime increment now ticks a command-local driver, uses its
core state/input switch for state-trace projection, and requires its
`commandDone` plus token conservation before completion. A multi-shape driver
test now passes in the 33/33 focused checkpoint, and the updated
`sau_model.o` compiles. After relinking, the historical baseline architecture
and state comparators both pass. The next source increment drives strict
results and registered mem_ctrl write requests from the command driver while
leaving non-strict DSE scheduling unchanged; the historical baseline
architecture/state comparators pass after that increment. The next source
increment drives strict shared-SRAM reads and A/B admissions from the driver,
while retaining existing address/index/data owners and leaving non-strict DSE
unchanged; the historical baseline comparators pass after relinking. The next
source increment appends actual driver-observed stage first/last/span and
command-done rows to `sau_timing_ledger.csv`. The developer's focused
checkpoint passes 33/33, and after relinking both historical baseline
comparators pass. Both commands emit resident 3..258, stream 293..2417,
A 269..2392, B 301..2425, result 612..2505, write 2513..2768, and
command-done 2772. The current source removes the remaining strict aggregate
fill/gap/completion gates; non-strict DSE retains the original timed
schedulers. After relinking, strict architecture/state comparison, stage
ledger inspection, constrained non-strict execution, and the four-way DSE
monotonicity check all pass.
The 2026-07-24 current-RTL recapture passes all eight architecture and state
strict comparisons. All eight timing-memory causal/backpressure suites and the
legacy direct-command causal regression also pass. The full SAU quick run
passes 63/63 checks across 23 suites.

## Statistics

`m5out/.../stats.txt` reports `system.sau.*`, including:

- command counts; total `commandCycles`; and operand-load, array-active,
  array-drain, and writeback phase cycles;
- accepted read/write requests and bytes;
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
