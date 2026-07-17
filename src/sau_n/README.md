# Im2Col Reference RTL Cycle Model

`Im2ColTiming` is a standalone gem5 `ClockedObject` that reproduces the
per-cycle behavior of
`rtl/gemmini_im2col_chw_gather_readable.sv`. It models configuration-derived
CHW address generation, 16-bank SRAM arbitration, combinational SRAM
responses, the six-state controller, the four-entry FIFO, downstream
handshakes, and the registered `done` pulse.

This model is an executable timing reference for the readable RTL. It is
**not** a model of a complete Gemmini implementation or NPU. It does not model
DMA, DRAM, instruction decode, a systolic array, MAC operations, weights, or
output writeback.

## Validation status

The model passed strict per-cycle validation against RTL simulated with VCS
T-2022.06. Seven golden runs cover `W=1/5/16/17/20`, packing and splitting,
nonzero scratchpad base, `N>1`, `C>1`, padding, stride, dilation, tail lanes,
same-bank different-row serialization, all-padding vectors, FIFO full with a
simultaneous pop, and post-`done` drain.

Across 1,026 cycles, all 47 trace fields matched between RTL and gem5:

- controller state, `busy`, and registered `done`;
- FIFO count and read/write pointers;
- all 16 SRAM request valid/address fields;
- all 16 combinational response valid/data fields; and
- `feed_valid`, `feed_ready`, `feed_data`, and `feed_mask`.

The validated RTL traces and provenance manifests are under
`tests/gem5/im2col/ref/`. The reference DUT SHA256 is:

```text
1e7e085e53f2fa63a21336d407805ec6db354ffa16de5d6901061b9e6b0c680c
```

## Fixed hardware and timing contract

The first version fixes the following parameters:

| Parameter | Value |
|---|---:|
| Clock | 100 MHz |
| Block size / lanes | 16 |
| SRAM banks | 16 |
| SRAM rows per bank | 4096 |
| Element width | 8 bits |
| FIFO depth | 4 |
| SRAM response | combinational |

One trace cycle is the stable interval between two adjacent positive clock
edges. Cycle 0 begins after `start` is accepted and contains
`ISSUE/busy=1/done=0`. The model reads old registers, computes combinational
signals, records the observation, calculates next registers, and then commits
all register updates together.

The final controller sequence is:

```text
DONE/busy=1/done=0 -> IDLE/busy=0/done=1
```

`rtl_done_cycle` is the cycle containing the registered `done` pulse. It does
not imply that the FIFO is empty. Simulation continues until the first
post-`done` cycle in which the FIFO is empty:

```text
post_done_drain_cycles = drained_cycle - rtl_done_cycle
total_done_cycles = rtl_done_cycle + 1
total_drained_cycles = drained_cycle + 1
```

## Build

The model is registered in the RISC-V gem5 binary. In the memory-constrained
development environment, build it incrementally with four jobs:

```bash
scons build/RISCV/gem5.opt -j4 \
    --ignore-style --limit-ld-memory-usage
```

## Fixture format

A fixture is a strict JSON object. Unknown fields, duplicate fields, missing
required fields, and non-integer numeric values are rejected. A typical
fixture is:

```json
{
  "schema_version": 1,
  "name": "w5_pack3_pad1_stride1",
  "n": 1,
  "c": 2,
  "h": 4,
  "w": 5,
  "out_h": 4,
  "out_w": 5,
  "kernel_h": 3,
  "kernel_w": 3,
  "stride_h": 1,
  "stride_w": 1,
  "dilation_h": 1,
  "dilation_w": 1,
  "pad_top": 1,
  "pad_left": 1,
  "spad_base": 0,
  "input_generator": "tb_act_value_v1"
}
```

The supported fields are:

| Field | Requirement |
|---|---|
| `schema_version` | required, exactly `1` |
| `name` | required, nonempty string |
| `n/c/h/w` | required, each in `1..65535` |
| `out_h/out_w` | both present or both omitted, each in `1..65535` |
| `kernel_h/kernel_w` | required, each in `1..15`, product at most 16 |
| `stride_h/stride_w` | required, each in `1..15` |
| `dilation_h/dilation_w` | required, each in `1..15` |
| `pad_top/pad_left` | required, each in `0..65535` |
| `spad_base` | required, bank-local row base in `0..4095` |
| `input_generator` | required, exactly `tb_act_value_v1` |
| `cfg_dw_mode` | optional, must resolve to `0` |
| `cfg_kernel_pattern` | optional, must resolve to `0xffff` |

Runtime ready parameters do not belong in a fixture. Passing
`ready_period` or `ready_high_cycles` as JSON fields is an error.

### Output dimensions

If `out_h` and `out_w` are omitted, the loader computes them using symmetric
top/left padding:

```text
effective_kernel_h = dilation_h * (kernel_h - 1) + 1
effective_kernel_w = dilation_w * (kernel_w - 1) + 1
out_h = floor((H + 2 * pad_top  - effective_kernel_h) / stride_h) + 1
out_w = floor((W + 2 * pad_left - effective_kernel_w) / stride_w) + 1
```

The automatic formula rejects a negative numerator. Explicit output
dimensions are retained even when they differ from the automatic result; the
loader prints a warning in that case. Providing only one output dimension is
an error.

For `W <= 16`, the reference RTL requires `out_w <= W`. The scratchpad
footprint must also satisfy:

```text
spad_base + N * C * spatial_words_per_channel <= 4096
```

All derived products and counters are checked unsigned 64-bit values.

### Deterministic activation data

The internal scratchpad is preloaded with:

```text
tb_act_value_v1 = (n * 97 + c * 31 + h * 7 + w + 1) mod 256
```

Only the low eight bits are stored. No external tensor, NPY, or binary input
is read.

## Run gem5

Run the default ready-every-cycle configuration from the gem5 worktree:

```bash
build/RISCV/gem5.opt \
    --outdir=m5out/im2col-w5 \
    configs/example/im2col_timing.py \
    --fixture tests/gem5/im2col/fixtures/w5_pack3_pad1.json
```

The default trace path is `m5out/im2col-w5/im2col/trace.csv`. Override it with
`--trace` when needed.

To drive periodic downstream backpressure:

```bash
build/RISCV/gem5.opt \
    --outdir=m5out/im2col-w5-backpressure \
    configs/example/im2col_timing.py \
    --fixture tests/gem5/im2col/fixtures/w5_pack3_pad1.json \
    --ready-period 11 \
    --ready-high-cycles 1
```

The ready value for cycle `t` is:

```text
feed_ready[t] = (t % ready_period) < ready_high_cycles
```

`ready_period` must be at least one, and `ready_high_cycles` must be in
`1..ready_period`.

The run prints `expected_vectors` and the canonical resolved-config SHA256.
Successful simulation exits with cause `im2col model drained` and writes:

```text
<outdir>/im2col/trace.csv
<outdir>/stats.txt
<outdir>/config.ini
<outdir>/config.json
```

## Trace comparison

The CSV contains a fixed 47-field schema from cycle 0 through the drained
cycle, inclusive. It uses normalized lowercase fixed-width hexadecimal
payloads. Bank 0 and lane 0 occupy the least-significant packed positions.

Compare a gem5 run with the validated W5 RTL golden:

```bash
python3 util/im2col/compare_traces.py \
    tests/gem5/im2col/ref/01_w5_pack3_pad1_p1_h1/trace.csv \
    m5out/im2col-w5/im2col/trace.csv
```

The comparator validates both inputs before comparison and reports the first
differing cycle and field. A successful comparison prints the number of
matching cycles.

## Statistics

`stats.txt` reports the `im2col.*` group:

- `rtlDoneCycle`, `drainedCycle`, `postDoneDrainCycles`,
  `totalDoneCycles`, and `totalDrainedCycles`;
- `feedVectors`, `handshakes`, and `feedVectorsPerCycle`;
- `presentedLanes`, `sramReadLanes`, `paddingZeroLanes`, and
  `invalidLanes`;
- `bankRequestCycles::b00..b15` and `bankUtilization::b00..b15`;
- `bankRowConflicts` and `extraCollectCycles`;
- `fifoAverageOccupancy`, `fifoPeakOccupancy`, and
  `fifoFullStallCycles`; and
- `backpressureCycles`.

Counts and averages cover cycle 0 through drained, inclusive. A full-stall
cycle is `state==PUSH && fifo_count==4`. A backpressure cycle has
`feed_valid && !feed_ready`.

At drained, the model enforces:

```text
feedVectors = handshakes = expected_vectors
fifo_count = 0
presentedLanes = sramReadLanes + paddingZeroLanes
sramReadLanes + paddingZeroLanes + invalidLanes = expected_vectors * 16
```

## Golden regression

The frozen matrix is `tests/gem5/im2col/golden_matrix.json`. Its seven RTL
profiles are registered as RISC-V quick tests:

```bash
cd tests
./main.py run --skip-build gem5/im2col
```

Each test runs gem5, checks the expected done/drained statistics, and compares
the generated trace against the corresponding VCS RTL golden.

Each `tests/gem5/im2col/ref/<profile>/manifest.json` records the resolved
configuration and SHA256, DUT/testbench SHA256, fixed hardware parameters,
trace schema version, ready pattern, simulator name/version, and completion
cycles. Absolute paths in a manifest are provenance from the workstation run;
the adjacent checked-in `trace.csv` is the golden used by regression.

To run a new fixture through an already compiled RTL simulator image, use:

```bash
python3 util/im2col/rtl_fixture_runner.py \
    --fixture tests/gem5/im2col/fixtures/w5_pack3_pad1.json \
    --trace /tmp/im2col-rtl/trace.csv \
    --sim-executable /path/to/simv \
    --simulator-name VCS \
    --simulator-version "VCS T-2022.06"
```

The runner validates the RTL trace against the independent logical feed
oracle and writes a provenance manifest next to the trace.

## Unsupported behavior

The first version intentionally does not support:

- DMA/DRAM preload, gem5 request ports, a system crossbar, or system memory;
- fixed or variable multi-cycle SRAM response latency, tags, pending requests,
  or request deduplication adapters;
- systolic-array timing, weights, MAC operations, or output writeback;
- CSR or instruction decoding and a complete NPU dataflow;
- depthwise mode, non-`0xffff` kernel patterns, or kernel area above 16;
- external tensors or arbitrary input generators;
- `W <= 16 && out_w > W`;
- scans of clock, FIFO depth, bank count, or other structural parameters; or
- a functional or performance claim for a real Gemmini implementation.

Large legal workloads with explicit `out_h/out_w` can require a long
simulation. The model does not add a functional watchdog or reject a legal
configuration merely because it is slow. Batch timeout policy belongs to the
external test framework.
