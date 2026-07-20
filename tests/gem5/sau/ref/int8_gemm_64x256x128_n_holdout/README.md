# int8_gemm_64x256x128_n_holdout — Golden Package

## Fixture Info
- **Fixture Name**: `int8_gemm_64x256x128_n_holdout`
- **Fixture Role**: holdout (N dimension)
- **Sweep**: N=128 (baseline N=256), M=64 K=256 held constant
- **Precision**: int8 | **Operation**: GEMM
- **Matrix**: M=64, K=256, N=128
- **trans_mode**: 0x1 | **reuse_mode**: 0x1
- **flow_loop_times**: 8 | **cutbit**: 13
- **SA Size**: 32×32 | **Command Count**: 2 (2 M-blocks)

## Simulation
- **Simulator**: VCS T-2022.06_Full64
- **Firmware**: INT8_SAU_MATMUL_TEST_ID_0 (64×256×128)
- **Result**: `*** ALL TESTS PASSED ***` (npu_done)

## Architecture Events
| Event | Count |
|-------|-------|
| phase_changed | 10 | command_accepted | 2 |
| read_accepted | 2560 | read_response_visible | 2560 |
| array_input_accepted | 4096 | result_produced | 256 |
| write_accepted | 256 | command_complete | 2 |
| **Total** | **9742** |

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
   30001 →  30233  ( 232 cyc)        REUSE_LOAD → FIRST_LOAD
   30233 →  30266  (  33 cyc)        FIRST_LOAD → D_OUT
   30266 →  30357  (  91 cyc)             D_OUT → REGISTER_UNLOAD
   30357 →  30494  ( 137 cyc)   REGISTER_UNLOAD → IDLE
   30494 →  30815  ( 321 cyc)              IDLE → REGISTER_LOAD
   30815 →  31071  ( 256 cyc)     REGISTER_LOAD → TRANSPOSE_LOAD
   31071 →  31103  (  32 cyc)    TRANSPOSE_LOAD → REUSE_LOAD
   31103 →  31334  ( 231 cyc)        REUSE_LOAD → TRANSPOSE_CLIP
   31334 →  31367  (  33 cyc)    TRANSPOSE_CLIP → D_OUT
   31367 →  31368  (   1 cyc)             D_OUT → REUSE_LOAD
   31368 →  31600  ( 232 cyc)        REUSE_LOAD → TRANSPOSE_CLIP
   31600 →  31633  (  33 cyc)    TRANSPOSE_CLIP → D_OUT
   31633 →  31634  (   1 cyc)             D_OUT → REUSE_LOAD
   31634 →  31866  ( 232 cyc)        REUSE_LOAD → TRANSPOSE_CLIP
   31866 →  31899  (  33 cyc)    TRANSPOSE_CLIP → D_OUT
   31899 →  31900  (   1 cyc)             D_OUT → REUSE_LOAD
   31900 →  32132  ( 232 cyc)        REUSE_LOAD → FIRST_LOAD
   32132 →  32165  (  33 cyc)        FIRST_LOAD → D_OUT
   32165 →  32256  (  91 cyc)             D_OUT → REGISTER_UNLOAD
   32256 →  32393  ( 137 cyc)   REGISTER_UNLOAD → IDLE
```

## Files
| File | Rows | Description |
|------|------|-------------|
| csr_writes.csv | 14 | Raw CSR write trace |
| csr_snapshot.json | 2 | Per-command snapshot |
| architecture.csv | 9742 | Event trace |
| diagnostic.csv | 3521 | Per-cycle diagnostic |
| manifest.json + SHA256SUMS + README.md | — | Metadata |
```
sha256sum -c SHA256SUMS
```
