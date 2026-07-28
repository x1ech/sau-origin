# int8_gemm_32x768x32_atbd_flow2_cutbit8

This package freezes the accepted RTL behavior for signed-int8
`M=32, K=768, N=32, cutbit=8` GEMM with two consecutive retained K256
segments followed by the final output segment:

```text
flow2 -> flow2 -> flow0
```

The three commands take 569, 569, and 685 SAU cycles. Their done-to-next-start
gaps are 82 and 89 cycles. The two retain commands produce no result or
writeback beats; the final command produces 32 result rows and 32 writes.
Final memory matches the software reference for all 1024 bytes.

The package contains the pre-simulation memory image, 21 accepted raw CSR
writes and decoded snapshots, complete RTL boundary changes, final output,
comparison log, simulator provenance, and checksums.

From the gem5 worktree, run:

```bash
./build/RISCV/gem5.opt \
  --outdir=m5out/sau-flow2-32x768x32 \
  tests/gem5/sau/configs/sau_flow2_double_functional.py
```

The permanent quick verifier checks final memory, all three command extents,
both inter-command gaps, and per-command result/write counts.
