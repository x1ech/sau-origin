# int8_gemm_64x256x32_n_sweep — Golden Package

## Fixture Info
- **Fixture Name**: `int8_gemm_64x256x32_n_sweep`
- **Fixture Role**: coverage (N dimension sweep)
- **Sweep**: N=32 (baseline N=256), M=64 K=256 held constant
- **Precision**: int8 | **Operation**: GEMM
- **Matrix**: M=64, K=256, N=32
- **trans_mode**: 0x1 (ATB) | **reuse_mode**: 0x1 (reuse A)
- **flow_loop_times**: 8 | **cutbit**: 13
- **SA Size**: 32×32 | **Command Count**: 2

## Simulation
- **Simulator**: VCS T-2022.06_Full64 (top_yinglong_tb)
- **Firmware**: INT8_SAU_MATMUL_TEST_ID_0 (64×256×32)
- **Result**: `*** ALL TESTS PASSED ***` (npu_done)
- **FSDB**: sim/vcs/build/yinglong/yinglong.fsdb

## CSR Write Summary
| Cmd | Start Cycle | v_addr | h_addr | out_addr |
|-----|------------|--------|--------|----------|
| 1 | 28914 | 0x29124000 | 0x29120000 | 0x29126800 |
| 2 | 29918 | 0x29124000 | 0x29122000 | 0x29126c00 |

## State Machine Trace
```
   28876 →  28916  (  40 cyc)              IDLE → REGISTER_LOAD
   28916 →  29172  ( 256 cyc)     REGISTER_LOAD → TRANSPOSE_LOAD
   29172 →  29204  (  32 cyc)    TRANSPOSE_LOAD → REUSE_LOAD
   29204 →  29435  ( 231 cyc)        REUSE_LOAD → FIRST_LOAD
   29435 →  29468  (  33 cyc)        FIRST_LOAD → D_OUT
   29468 →  29469  (   1 cyc)             D_OUT → REGISTER_UNLOAD
   29469 →  29599  ( 130 cyc)   REGISTER_UNLOAD → IDLE
   29599 →  29920  ( 321 cyc)              IDLE → REGISTER_LOAD
   29920 →  30176  ( 256 cyc)     REGISTER_LOAD → TRANSPOSE_LOAD
   30176 →  30208  (  32 cyc)    TRANSPOSE_LOAD → REUSE_LOAD
   30208 →  30439  ( 231 cyc)        REUSE_LOAD → FIRST_LOAD
   30439 →  30472  (  33 cyc)        FIRST_LOAD → D_OUT
   30472 →  30473  (   1 cyc)             D_OUT → REGISTER_UNLOAD
   30473 →  30603  ( 130 cyc)   REGISTER_UNLOAD → IDLE
```

## Architecture Events
| Event | Count |
|-------|-------|-------|
| phase_changed | 10 | command_accepted | 2 |
| read_accepted | 1024 | read_response_visible | 1024 |
| array_input_accepted | 1024 | result_produced | 64 |
| write_accepted | 64 | command_complete | 2 |
| **Total** | **3214** |

## Files
| File | Rows | Description |
|------|------|-------------|
| csr_writes.csv | 14 (2×7) | Raw CSR write trace |
| csr_snapshot.json | 2 objects | Per-command snapshot |
| architecture.csv | 3214 | Event trace |
| diagnostic.csv | 1731 | Per-cycle diagnostic |
| manifest.json + SHA256SUMS + README.md | — | Metadata |

```bash
sha256sum -c SHA256SUMS
```
