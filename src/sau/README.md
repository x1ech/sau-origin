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

## Run and compare the calibrated RTL profile

The fixed-cadence calibration path uses a local deterministic read-response
schedule. It is the only profile intended for strict cycle comparison.

```bash
./build/RISCV/gem5.opt \
    --outdir=m5out/sau-rtl-strict \
    configs/example/sau_timing.py \
    --rtl-profile \
    --calibration-memory \
    --trace=m5out/sau-rtl-strict/sau.csv

python3 util/sau/compare_trace.py --mode strict \
    tests/gem5/sau/ref/int8_gemm_64x256x256/architecture.csv \
    m5out/sau-rtl-strict/sau.csv
```

The expected result is exit cause `SAU command complete` and no output from
the comparator. The imported profile has two commands and 18,446 trace rows.

## Run the constrained system-memory profile

Without `--calibration-memory`, SAU sends requests through `SystemXBar` and
`SimpleMemory`, so retry, latency variance, bandwidth, and outstanding limits
can affect timing. Its trace must use causal comparison: independent request
and response events may interleave differently from RTL while command-local
dataflow remains checked.

```bash
./build/RISCV/gem5.opt \
    --outdir=m5out/sau-constrained \
    configs/example/sau_timing.py \
    --rtl-profile \
    --memory-latency=20ns \
    --memory-latency-var=5ns \
    --memory-bandwidth=1GiB/s \
    --max-outstanding-reads=2 \
    --max-outstanding-writes=2 \
    --trace=m5out/sau-constrained/sau.csv

python3 util/sau/compare_trace.py --mode causal \
    tests/gem5/sau/ref/int8_gemm_64x256x256/architecture.csv \
    m5out/sau-constrained/sau.csv
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
