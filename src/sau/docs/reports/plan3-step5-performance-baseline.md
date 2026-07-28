# PLAN3 Step 5 Performance Baseline

Date: 2026-07-28

This report freezes the first reproducible host-performance baseline after
strict Flow0/1/2 payload support. It is a regression reference, not a general
gem5 performance claim.

## Environment

- Host: Linux 4.18.0-545.el8.x86_64, x86_64
- CPU: AMD Ryzen 9 7900X, 12 cores / 24 hardware threads
- Host memory: 93 GiB
- gem5: 25.1.0.1, optimized RISCV build
- `build/RISCV/gem5.opt` SHA-256:
  `7648386d7b4574c80bced38a36ddfb880d3a0cfefa8a68533ec184735a43765e`
- Worktree base HEAD: `bef303b86c6af2e2ae3b59dd91c638cb3a9b34f6`

Each row is the median of three sequential `/usr/bin/time` runs. No runs were
parallel. Payload boundary tracing was disabled. The architecture trace,
semantic-state trace, and timing ledger remain enabled because strict replay
requires its architecture trace to contain `command_complete`; they are not
the optional per-cycle payload debug trace.

## Results

| Workload | Path | Wall (s) | Peak RSS (KiB) | simTicks | Command cycles | Reads / writes |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 32x32x32 | strict timing/control | 0.16 | 66,328 | 233,000 | 231 | 64 / 32 |
| 64x256x256 | strict timing/control | 0.17 | 70,140 | 5,865,000 | 5,543 | 4,608 / 512 |
| 32x32x32 cutbit1 | strict real payload | 0.16 | 66,612 | 233,000 | 231 | 64 / 32 |
| 32x768x32 `[2,2,0]` | strict real payload | 0.17 | 66,968 | 1,996,000 | 1,821 | 1,536 / 32 |

Observed three-run ranges:

- 32x32x32 timing: 0.16 s; 66,184–66,544 KiB RSS.
- 64x256x256 timing: 0.17–0.18 s; 69,644–70,932 KiB RSS.
- 32x32x32 payload: 0.16–0.17 s; 66,612–66,692 KiB RSS.
- K768 payload: 0.17 s; 66,880–67,084 KiB RSS.
- Every run had zero major page faults.

The two functional runs also reproduced their complete final-memory oracle.
The K768 payload run recorded 1,536 payload reads, 768 transposer input rows,
768 output columns, zero payload underflows, and maximum combined bank
occupancy 33.

## Reproduction

From the gem5 worktree, wrap the selected command with:

```bash
/usr/bin/time \
  -f 'wall_seconds=%e\nuser_seconds=%U\nsystem_seconds=%S\nmax_rss_kib=%M\nminor_faults=%R\nmajor_faults=%F' \
  -o OUTDIR/time.txt \
  ./build/RISCV/gem5.opt --outdir=OUTDIR \
  configs/example/sau_timing.py \
  --rtl-profile tests/gem5/sau/ref/int8_gemm_64x256x256_baseline
```

The 32x32 timing profile is
`tests/gem5/sau/ref/int8_gemm_32x32x32_single_flow`. The functional K768
command is:

```bash
/usr/bin/time -v \
  ./build/RISCV/gem5.opt --outdir=OUTDIR \
  tests/gem5/sau/configs/sau_flow2_double_functional.py
```

## Allocation Analysis

- gem5's `hostMemory` statistic remains approximately 1.41 MB in every run;
  it does not grow with 64x more command cycles or 24x more payload reads.
- The functional image contains 16,384 16-byte words (256 KiB). Sparse
  `FunctionalMemory` therefore materializes 64 pages, not its declared
  512 MiB address range.
- Fixed datapath storage is bounded: two 32x32-byte transposer banks, one
  32x32-byte result serializer, a 256x32-byte output RF, 1,024 PE
  accumulators, one 32x32 snapshot, and an 18-slot macro-event pipeline.
- The largest size-dependent artifact in these runs is the architecture CSV:
  17,479 bytes for 32x32x32, 294,929 bytes for K768, and 1,182,424 bytes for
  64x256x256. This is output I/O, not retained datapath state.
- No heap profiler is installed on this host. The stable `hostMemory`, small
  RSS delta, sparse-page contract, bounded containers, and zero major faults
  provide the current allocation evidence.

## Regression Threshold

Repeat all three runs before accepting a suspected regression. Investigate
when the median wall time or peak RSS increases by more than 25% with the same
binary, host load class, command, and trace configuration. First separate
architecture-trace growth from model-state growth.

## Fixture Migration

The early `int8_gemm_32x32x32_atbd_cutbit8` functional manifest predates the
current replay schema and originally lacked `beat_bytes` and `command_count`,
so its direct replay was rejected during baseline setup. This baseline uses
the already migrated, byte-exact cutbit1 fixture for the same 32x32x32
geometry. The cutbit8 manifest was then migrated without changing any golden
artifact; its package checksums and direct replay are verified separately.
