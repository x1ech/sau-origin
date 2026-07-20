# int8_gemm_64x128x256_k_holdout — Golden Package

## Fixture Info
- **Fixture Name**: `int8_gemm_64x128x256_k_holdout`
- **Fixture Role**: holdout (K dimension)
- **Sweep**: K=128 (baseline K=256), M=64 N=256 held constant
- **Precision**: int8 | **Operation**: GEMM
- **Matrix**: M=64, K=128, N=256
- **trans_mode**: 0x1 | **reuse_mode**: 0x1
- **flow_loop_times**: 4 | **cutbit**: 13
- **SA Size**: 32×32 | **Command Count**: 2 (2 M-blocks)

## Simulation
- **Simulator**: VCS T-2022.06_Full64
- **Firmware**: INT8_SAU_MATMUL_TEST_ID_0 (64×128×256)
- **Result**: `*** ALL TESTS PASSED ***` (npu_done)
- **FSDB**: sim/vcs/build/yinglong/yinglong.fsdb (91 MB)

## Architecture Events
| Event | Count |
|-------|-------|
| phase_changed | 10 | command_accepted | 2 |
| read_accepted | 2304 | read_response_visible | 2304 |
| array_input_accepted | 4096 | result_produced | 512 |
| write_accepted | 512 | command_complete | 2 |
| **Total** | **9742** |

## State Machine Trace
```
   28876 →  28916  (  40 cyc)              IDLE → REGISTER_LOAD
   28916 →  29044  ( 128 cyc)     REGISTER_LOAD → TRANSPOSE_LOAD
   29044 →  29076  (  32 cyc)    TRANSPOSE_LOAD → REUSE_LOAD
   29076 →  29175  (  99 cyc)        REUSE_LOAD → TRANSPOSE_CLIP
   29175 →  29208  (  33 cyc)    TRANSPOSE_CLIP → D_OUT
   29208 →  29209  (   1 cyc)             D_OUT → REUSE_LOAD
   29209 →  29309  ( 100 cyc)        REUSE_LOAD → TRANSPOSE_CLIP
   29309 →  29342  (  33 cyc)    TRANSPOSE_CLIP → D_OUT
   29342 →  29343  (   1 cyc)             D_OUT → REUSE_LOAD
   29343 →  29443  ( 100 cyc)        REUSE_LOAD → TRANSPOSE_CLIP
   29443 →  29476  (  33 cyc)    TRANSPOSE_CLIP → D_OUT
   29476 →  29477  (   1 cyc)             D_OUT → REUSE_LOAD
   29477 →  29577  ( 100 cyc)        REUSE_LOAD → TRANSPOSE_CLIP
   29577 →  29610  (  33 cyc)    TRANSPOSE_CLIP → D_OUT
   29610 →  29611  (   1 cyc)             D_OUT → REUSE_LOAD
   29611 →  29711  ( 100 cyc)        REUSE_LOAD → TRANSPOSE_CLIP
   29711 →  29744  (  33 cyc)    TRANSPOSE_CLIP → D_OUT
   29744 →  29745  (   1 cyc)             D_OUT → REUSE_LOAD
   29745 →  29845  ( 100 cyc)        REUSE_LOAD → TRANSPOSE_CLIP
   29845 →  29878  (  33 cyc)    TRANSPOSE_CLIP → D_OUT
   29878 →  29879  (   1 cyc)             D_OUT → REUSE_LOAD
   29879 →  29979  ( 100 cyc)        REUSE_LOAD → TRANSPOSE_CLIP
   29979 →  30012  (  33 cyc)    TRANSPOSE_CLIP → D_OUT
   30012 →  30013  (   1 cyc)             D_OUT → REUSE_LOAD
   30013 →  30113  ( 100 cyc)        REUSE_LOAD → FIRST_LOAD
   30113 →  30146  (  33 cyc)        FIRST_LOAD → D_OUT
   30146 →  30237  (  91 cyc)             D_OUT → REGISTER_UNLOAD
   30237 →  30502  ( 265 cyc)   REGISTER_UNLOAD → IDLE
   30502 →  30823  ( 321 cyc)              IDLE → REGISTER_LOAD
   30823 →  30951  ( 128 cyc)     REGISTER_LOAD → TRANSPOSE_LOAD
   30951 →  30983  (  32 cyc)    TRANSPOSE_LOAD → REUSE_LOAD
   30983 →  31082  (  99 cyc)        REUSE_LOAD → TRANSPOSE_CLIP
   31082 →  31115  (  33 cyc)    TRANSPOSE_CLIP → D_OUT
   31115 →  31116  (   1 cyc)             D_OUT → REUSE_LOAD
   31116 →  31216  ( 100 cyc)        REUSE_LOAD → TRANSPOSE_CLIP
   31216 →  31249  (  33 cyc)    TRANSPOSE_CLIP → D_OUT
   31249 →  31250  (   1 cyc)             D_OUT → REUSE_LOAD
   31250 →  31350  ( 100 cyc)        REUSE_LOAD → TRANSPOSE_CLIP
   31350 →  31383  (  33 cyc)    TRANSPOSE_CLIP → D_OUT
   31383 →  31384  (   1 cyc)             D_OUT → REUSE_LOAD
   31384 →  31484  ( 100 cyc)        REUSE_LOAD → TRANSPOSE_CLIP
   31484 →  31517  (  33 cyc)    TRANSPOSE_CLIP → D_OUT
   31517 →  31518  (   1 cyc)             D_OUT → REUSE_LOAD
   31518 →  31618  ( 100 cyc)        REUSE_LOAD → TRANSPOSE_CLIP
   31618 →  31651  (  33 cyc)    TRANSPOSE_CLIP → D_OUT
   31651 →  31652  (   1 cyc)             D_OUT → REUSE_LOAD
   31652 →  31752  ( 100 cyc)        REUSE_LOAD → TRANSPOSE_CLIP
   31752 →  31785  (  33 cyc)    TRANSPOSE_CLIP → D_OUT
   31785 →  31786  (   1 cyc)             D_OUT → REUSE_LOAD
   31786 →  31886  ( 100 cyc)        REUSE_LOAD → TRANSPOSE_CLIP
   31886 →  31919  (  33 cyc)    TRANSPOSE_CLIP → D_OUT
   31919 →  31920  (   1 cyc)             D_OUT → REUSE_LOAD
   31920 →  32020  ( 100 cyc)        REUSE_LOAD → FIRST_LOAD
   32020 →  32053  (  33 cyc)        FIRST_LOAD → D_OUT
   32053 →  32144  (  91 cyc)             D_OUT → REGISTER_UNLOAD
   32144 →  32409  ( 265 cyc)   REGISTER_UNLOAD → IDLE
```

## Files
| File | Rows | Description |
|------|------|-------------|
| csr_writes.csv | 14 (2×7) | Raw CSR write trace |
| csr_snapshot.json | 2 objects | Per-command snapshot |
| architecture.csv | 9742 | Event trace |
| diagnostic.csv | 3537 | Per-cycle diagnostic |
| manifest.json + SHA256SUMS + README.md | — | Metadata |

```bash
sha256sum -c SHA256SUMS
```
