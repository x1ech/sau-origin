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

## Run and compare a strict RTL CSR fixture

The fixed-cadence calibration path uses a local deterministic read-response
schedule. It is the only profile intended for strict cycle comparison.

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

The expected result is exit cause `SAU command complete` and no output from
the comparator. Strict mode replays `csr_writes.csv`, loads named RTL
elaboration parameters from `manifest.json`, and writes
`sau_timing_ledger.csv` alongside the trace. It rejects all timing overrides.

Strict mode models only the RTL-visible SRAM contract: one shared 256-bit
(32-byte) read/write request port, at most one request per SAU cycle, read
priority, ordered responses, and fixed read visibility at
`accepted_cycle + SRAM_DELAY + 1`. Operand-A is externally preloaded before
Operand-B requests and becomes resident in `ARegisterFileIn` before array
execution. SRAM banks, crossbar contention, retry, and variable latency are
intentionally reserved for non-strict system-memory runs.

The verified strict control domain is int8 GEMM with `trans_mode=01` and
`reuse_mode=01`. Five coverage shapes (32x32x32, 64x32x256, 64x256x32,
32x256x256, and 64x256x256) were used to derive and freeze the structural
rules. Three independently accepted hold-outs (96x256x256, 64x128x256, and
64x256x128) then passed without fixture-specific timing adjustment. This is
evidence for M/K/N generalization inside the tested control domain, not a
claim that every unobserved RTL configuration is supported.

All eight packages are registered as RISC-V quick tests. Each test runs both
the normalized architecture strict comparator and the RTL-diagnostic
semantic-state comparator:

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
