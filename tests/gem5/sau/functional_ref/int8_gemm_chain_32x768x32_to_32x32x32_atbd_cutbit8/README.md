# int8_gemm_chain_32x768x32_to_32x32x32_atbd_cutbit8

This package freezes a strict Reuse-A command-to-command memory dependency.
The first GEMM is `M=32, K=768, N=32, cutbit=8` and runs:

```text
flow2 -> flow2 -> flow0
```

Its final command writes 32 beats at `0x2912c800..0x2912cbe0`. After an
independent firmware reference calculation, a fourth `M=K=N=32` Flow0
command reads exactly those 32 beats as Operand-A, multiplies them by the
first 32x32 block of Operand-B, and writes the final 1024 bytes at
`0x2912c400`.

The four command extents are 569, 569, 685, and 231 SAU cycles. Their gaps
are 82, 89, and 1,079,597 cycles; the long final gap is software-reference
work, not an SAU delay. RTL and the independent software reference match all
1024 final bytes.

`memory_dependencies` in `manifest.json` is verifier metadata only. It does
not create another memory implementation or comparator. The existing
functional verifier reuses its single architecture-trace parse to check:

- command 3 writes 32 consecutive beats at the dependency range;
- command 4 reads the same sequence as Operand-A;
- every producer write is visible before the first consumer read;
- final memory remains byte-exact against the RTL oracle.

From the gem5 worktree, run:

```bash
./build/RISCV/gem5.opt \
  --outdir=m5out/sau-chain-32x768-to-32 \
  tests/gem5/sau/configs/sau_chain_functional.py
```
