# int8_gemm_64x160x64_atbd_flow1_cutbit8

This package freezes the accepted RTL flow-mode-1 behavior for a signed-int8
`M=64, K=160, N=64, cutbit=8` GEMM.

`K=160` uses no retain command. `M=64` produces two sequential flow-mode-1
commands, one for each 32-row M tile. Both commands take 690 SAU cycles.

## RTL output order

The current RTL keeps the 2x2 output-tile grid fixed and rotates each 32x32
tile clockwise. For tile coordinates `(tile_r, tile_c)` and local coordinates
`(r, c)`:

```text
actual[tile_r * 32 + r][tile_c * 32 + c]
    == expected[tile_r * 32 + (31 - c)][tile_c * 32 + r]
```

This relation matches 4096/4096 bytes. The ordinary row-major software
reference differs at 4072/4096 bytes and is deliberately not the oracle;
`final_output_memory.hex` is the address-ordered RTL oracle.

## Contents

- `initial_memory.hex`: pre-simulation 16-byte-word firmware memory image.
- `csr_writes.csv`: fourteen accepted raw CSR writes for the two commands.
- `boundary.csv`: complete CSR, input, array/output and SRAM boundary changes.
- `matmul_compare.csv`: normal software reference versus RTL actual bytes.
- `final_output_memory.hex`: 4096 RTL output bytes at
  `0x29126000..0x29126fff`.
- `sim.log`: VCS run log.
- `manifest.json`: replay, RTL, simulator, layout and Hash provenance.
- `SHA256SUMS`: package integrity.

## Strict gem5 replay

The strict model executes both `T-ATBD/R-A/F-trans` commands, reproduces their
690-cycle RTL command extents, and matches the 4096-byte final-memory oracle.
The command below is the standalone acceptance replay.

From the gem5 worktree:

```bash
./build/RISCV/gem5.opt \
  --outdir=m5out/sau-flow1-64x160x64 \
  configs/example/sau_timing.py \
  --rtl-profile \
    tests/gem5/sau/functional_ref/int8_gemm_64x160x64_atbd_flow1_cutbit8 \
  --memory-image \
    tests/gem5/sau/functional_ref/int8_gemm_64x160x64_atbd_flow1_cutbit8/initial_memory.hex \
  --memory-image-base 0x29120000 \
  --functional-memory-base 0x20000000 \
  --functional-memory-size 0x20000000 \
  --final-memory-dump m5out/sau-flow1-64x160x64/final_output_memory.hex \
  --final-memory-dump-base 0x29126000 \
  --final-memory-dump-size 4096 \
  --boundary-trace m5out/sau-flow1-64x160x64/boundary.csv
```

Compare final memory:

```bash
python3 util/sau/compare_memory.py \
  tests/gem5/sau/functional_ref/int8_gemm_64x160x64_atbd_flow1_cutbit8/final_output_memory.hex \
  m5out/sau-flow1-64x160x64/final_output_memory.hex
```

Verify package integrity from this directory:

```bash
sha256sum -c SHA256SUMS
```
