# PLAN3 Step 5 Flow1 64x160x64 RTL Diagnostic

Date: 2026-07-28

## Purpose

This follow-up isolates flow-mode-1 output ordering from K-tile retention.
Unlike the earlier K512 diagnostic, `K=160` fits in one K tile and never uses
flow mode 2.

## Configuration

- Matrix: `M=64, K=160, N=64`
- Precision/cutbit: signed int8, `cutbit=8`
- Final flow mode: 1
- K tiles per M tile: 1
- M tiles: 2
- Accepted command sequence: `[flow1, flow1]`

## Functional Result

The normal-layout comparator reports 4072/4096 mismatches. A whole-matrix pure
transpose reports 4074/4096 mismatches, so the result is not the required
`actual[r][c] == expected[c][r]`.

The exact observed mapping operates independently on each 32x32 tile. For
tile coordinates `(tile_r, tile_c)` and local coordinates `(r, c)`:

```text
actual[tile_r * 32 + r][tile_c * 32 + c]
    == expected[tile_r * 32 + (31 - c)][tile_c * 32 + r]
```

This tile-local 90-degree clockwise rotation matches 4096/4096 elements. Tile
positions remain unchanged; the 2x2 tile grid is not transposed.

The extra rotation is therefore intrinsic to the current flow-mode-1 output
path and is not caused by flow-mode-2 retention.

On 2026-07-28 the user confirmed that gem5 must align to this observed RTL
ordering rather than impose mathematical whole-matrix transpose semantics.
This capture is therefore accepted as flow-mode-1 timing and ordering evidence.

The self-checking package is stored at
`tests/gem5/sau/functional_ref/int8_gemm_64x160x64_atbd_flow1_cutbit8`.
It passes package Hash, 4096-byte tile-order, and fourteen-write CSR checks.
PLAN3 Step 5 increment 5 enables the strict Flow1 runtime. Both commands match
the 690-cycle RTL extent and the final memory matches 4096/4096 bytes.

## Control Timing

The SAU clock period is 1667 ps.

| Boundary | Command 0 | Command 1 |
| --- | ---: | ---: |
| start-to-first result-valid | 417 cycles | 417 cycles |
| start-to-second result-valid | 584 cycles | 584 cycles |
| write-valid extent | 64 cycles | 64 cycles |
| start-to-done | 690 cycles | 690 cycles |

Command 1 starts 327 cycles after command 0 done. Both commands are accepted
as flow mode 1.

## Evidence

```text
/home/xch/work/npu_lpnpu/tmp/flow1_m64_k160_n64/control.csv
SHA-256 38237e7f6f948d9422f336013682ff42680802de20399923e4959ad32de1ef3c

/home/xch/work/npu_lpnpu/sim/vcs/build/yinglong/matmul_compare.csv
SHA-256 24deca381657c8b471b3201f85e473c14b0a5f945f33e235d4e45289bf88f233

/home/xch/work/npu_lpnpu/sim/vcs/build/yinglong/matmul_mismatches.csv
SHA-256 5bdf4842e71bc33ea4f1cdc5ce48e91050ce088e9fad6275d713391edefff6d0

/home/xch/work/npu_lpnpu/sim/vcs/build/yinglong/yinglong.fsdb
SHA-256 ebae0177e8f72b01f627bf6d4303255db11457ff9f09295669c1b00c81204139
```

The earlier retain-plus-flow1 result is documented in
[`plan3-step5-flow1-k512-rtl.md`](plan3-step5-flow1-k512-rtl.md).
