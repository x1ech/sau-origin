# int8_gemm_96x256x256_m_holdout — Golden Package

## Fixture Info
- **Fixture Name**: `int8_gemm_96x256x256_m_holdout`
- **Fixture Role**: holdout (M dimension)
- **Sweep**: M=96 (baseline M=64), K=256 N=256 held constant
- **Precision**: int8 | **Operation**: GEMM
- **Matrix**: M=96, K=256, N=256
- **trans_mode**: 0x1 | **reuse_mode**: 0x1
- **flow_loop_times**: 8 | **cutbit**: 13
- **SA Size**: 32×32 | **Command Count**: 3 (ceil(96/32) = 3 M-blocks)

## Simulation
- **Simulator**: VCS T-2022.06_Full64 (top_yinglong_tb)
- **Firmware**: INT8_SAU_MATMUL_TEST_ID_0 (96×256×256)
- **Result**: `*** ALL TESTS PASSED ***` (npu_done)
- **FSDB**: sim/vcs/build/yinglong/yinglong.fsdb (215 MB)

## Architecture Events
| Event | Count |
|-------|-------|
| phase_changed | 15 |
| command_accepted | 3 |
| read_accepted | 6912 |
| read_response_visible | 6912 |
| array_input_accepted | 12288 |
| result_produced | 768 |
| write_accepted | 768 |
| command_complete | 3 |
| **Total** | **27669** |

## State Machine Trace
```
   28876 →  28916  (  40 cyc)              IDLE → REGISTER_LOAD
   28916 →  29172  ( 256 cyc)     REGISTER_LOAD → TRANSPOSE_LOAD
   29172 →  29204  (  32 cyc)    TRANSPOSE_LOAD → REUSE_LOAD
   29204 →  29435  ( 231 cyc)        REUSE_LOAD → TRANSPOSE_CLIP
   29435 →  29468  (  33 cyc)    TRANSPOSE_CLIP → D_OUT
   29468 →  29469  (   1 cyc)             D_OUT → REUSE_LOAD
   29469 →  29701  ( 232 cyc)        REUSE_LOAD → TRANSPOSE_CLIP
   29701 →  29734  (  33 cyc)    TRANSPOSE_CLIP → D_OUT
   29734 →  29735  (   1 cyc)             D_OUT → REUSE_LOAD
   29735 →  29967  ( 232 cyc)        REUSE_LOAD → TRANSPOSE_CLIP
   29967 →  30000  (  33 cyc)    TRANSPOSE_CLIP → D_OUT
   30000 →  30001  (   1 cyc)             D_OUT → REUSE_LOAD
   30001 →  30233  ( 232 cyc)        REUSE_LOAD → TRANSPOSE_CLIP
   30233 →  30266  (  33 cyc)    TRANSPOSE_CLIP → D_OUT
   30266 →  30267  (   1 cyc)             D_OUT → REUSE_LOAD
   30267 →  30499  ( 232 cyc)        REUSE_LOAD → TRANSPOSE_CLIP
   30499 →  30532  (  33 cyc)    TRANSPOSE_CLIP → D_OUT
   30532 →  30533  (   1 cyc)             D_OUT → REUSE_LOAD
   30533 →  30765  ( 232 cyc)        REUSE_LOAD → TRANSPOSE_CLIP
   30765 →  30798  (  33 cyc)    TRANSPOSE_CLIP → D_OUT
   30798 →  30799  (   1 cyc)             D_OUT → REUSE_LOAD
   30799 →  31031  ( 232 cyc)        REUSE_LOAD → TRANSPOSE_CLIP
   31031 →  31064  (  33 cyc)    TRANSPOSE_CLIP → D_OUT
   31064 →  31065  (   1 cyc)             D_OUT → REUSE_LOAD
   31065 →  31297  ( 232 cyc)        REUSE_LOAD → FIRST_LOAD
   31297 →  31330  (  33 cyc)        FIRST_LOAD → D_OUT
   31330 →  31421  (  91 cyc)             D_OUT → REGISTER_UNLOAD
   31421 →  31686  ( 265 cyc)   REGISTER_UNLOAD → IDLE
   31686 →  32007  ( 321 cyc)              IDLE → REGISTER_LOAD
   32007 →  32263  ( 256 cyc)     REGISTER_LOAD → TRANSPOSE_LOAD
   32263 →  32295  (  32 cyc)    TRANSPOSE_LOAD → REUSE_LOAD
   32295 →  32526  ( 231 cyc)        REUSE_LOAD → TRANSPOSE_CLIP
   32526 →  32559  (  33 cyc)    TRANSPOSE_CLIP → D_OUT
   32559 →  32560  (   1 cyc)             D_OUT → REUSE_LOAD
   32560 →  32792  ( 232 cyc)        REUSE_LOAD → TRANSPOSE_CLIP
   32792 →  32825  (  33 cyc)    TRANSPOSE_CLIP → D_OUT
   32825 →  32826  (   1 cyc)             D_OUT → REUSE_LOAD
   32826 →  33058  ( 232 cyc)        REUSE_LOAD → TRANSPOSE_CLIP
   33058 →  33091  (  33 cyc)    TRANSPOSE_CLIP → D_OUT
   33091 →  33092  (   1 cyc)             D_OUT → REUSE_LOAD
   33092 →  33324  ( 232 cyc)        REUSE_LOAD → TRANSPOSE_CLIP
   33324 →  33357  (  33 cyc)    TRANSPOSE_CLIP → D_OUT
   33357 →  33358  (   1 cyc)             D_OUT → REUSE_LOAD
   33358 →  33590  ( 232 cyc)        REUSE_LOAD → TRANSPOSE_CLIP
   33590 →  33623  (  33 cyc)    TRANSPOSE_CLIP → D_OUT
   33623 →  33624  (   1 cyc)             D_OUT → REUSE_LOAD
   33624 →  33856  ( 232 cyc)        REUSE_LOAD → TRANSPOSE_CLIP
   33856 →  33889  (  33 cyc)    TRANSPOSE_CLIP → D_OUT
   33889 →  33890  (   1 cyc)             D_OUT → REUSE_LOAD
   33890 →  34122  ( 232 cyc)        REUSE_LOAD → TRANSPOSE_CLIP
   34122 →  34155  (  33 cyc)    TRANSPOSE_CLIP → D_OUT
   34155 →  34156  (   1 cyc)             D_OUT → REUSE_LOAD
   34156 →  34388  ( 232 cyc)        REUSE_LOAD → FIRST_LOAD
   34388 →  34421  (  33 cyc)        FIRST_LOAD → D_OUT
   34421 →  34512  (  91 cyc)             D_OUT → REGISTER_UNLOAD
   34512 →  34777  ( 265 cyc)   REGISTER_UNLOAD → IDLE
   34777 →  35098  ( 321 cyc)              IDLE → REGISTER_LOAD
   35098 →  35354  ( 256 cyc)     REGISTER_LOAD → TRANSPOSE_LOAD
   35354 →  35386  (  32 cyc)    TRANSPOSE_LOAD → REUSE_LOAD
   35386 →  35617  ( 231 cyc)        REUSE_LOAD → TRANSPOSE_CLIP
   35617 →  35650  (  33 cyc)    TRANSPOSE_CLIP → D_OUT
   35650 →  35651  (   1 cyc)             D_OUT → REUSE_LOAD
   35651 →  35883  ( 232 cyc)        REUSE_LOAD → TRANSPOSE_CLIP
   35883 →  35916  (  33 cyc)    TRANSPOSE_CLIP → D_OUT
   35916 →  35917  (   1 cyc)             D_OUT → REUSE_LOAD
   35917 →  36149  ( 232 cyc)        REUSE_LOAD → TRANSPOSE_CLIP
   36149 →  36182  (  33 cyc)    TRANSPOSE_CLIP → D_OUT
   36182 →  36183  (   1 cyc)             D_OUT → REUSE_LOAD
   36183 →  36415  ( 232 cyc)        REUSE_LOAD → TRANSPOSE_CLIP
   36415 →  36448  (  33 cyc)    TRANSPOSE_CLIP → D_OUT
   36448 →  36449  (   1 cyc)             D_OUT → REUSE_LOAD
   36449 →  36681  ( 232 cyc)        REUSE_LOAD → TRANSPOSE_CLIP
   36681 →  36714  (  33 cyc)    TRANSPOSE_CLIP → D_OUT
   36714 →  36715  (   1 cyc)             D_OUT → REUSE_LOAD
   36715 →  36947  ( 232 cyc)        REUSE_LOAD → TRANSPOSE_CLIP
   36947 →  36980  (  33 cyc)    TRANSPOSE_CLIP → D_OUT
   36980 →  36981  (   1 cyc)             D_OUT → REUSE_LOAD
   36981 →  37213  ( 232 cyc)        REUSE_LOAD → TRANSPOSE_CLIP
   37213 →  37246  (  33 cyc)    TRANSPOSE_CLIP → D_OUT
   37246 →  37247  (   1 cyc)             D_OUT → REUSE_LOAD
   37247 →  37479  ( 232 cyc)        REUSE_LOAD → FIRST_LOAD
   37479 →  37512  (  33 cyc)        FIRST_LOAD → D_OUT
   37512 →  37603  (  91 cyc)             D_OUT → REGISTER_UNLOAD
   37603 →  37868  ( 265 cyc)   REGISTER_UNLOAD → IDLE
```

## Files
| File | Rows | Description |
|------|------|-------------|
| csr_writes.csv | 21 (3×7) | Raw CSR write trace |
| csr_snapshot.json | 3 objects | Per-command snapshot |
| architecture.csv | 27669 | Event trace |
| diagnostic.csv | 8996 | Per-cycle diagnostic |
| manifest.json + SHA256SUMS + README.md | — | Metadata |

```bash
sha256sum -c SHA256SUMS
```
