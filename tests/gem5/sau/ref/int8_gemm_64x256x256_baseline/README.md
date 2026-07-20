# int8_gemm_64x256x256_baseline — Golden Package

## Fixture Info
- **Fixture Name**: `int8_gemm_64x256x256_baseline`
- **Fixture Role**: coverage (baseline)
- **Note**: This is the baseline fixture — 2 commands, flow_loop_times=8, dual M-block 32×256×256 each
- **Precision**: int8 | **Operation**: GEMM
- **Matrix**: M=64, K=256, N=256
- **trans_mode**: 0x1 (ATB) | **reuse_mode**: 0x1 (reuse A)
- **flow_loop_times**: 8 | **cutbit**: 13
- **SA Size**: 32×32 | **Command Count**: 2 (2 M-blocks)

## Simulation
- **Simulator**: VCS T-2022.06_Full64 (top_yinglong_tb)
- **Firmware**: INT8_SAU_MATMUL_TEST_ID_0 (64×256×256)
- **Result**: `*** ALL TESTS PASSED ***` (npu_done)
- **FSDB**: sim/vcs/build/yinglong/yinglong.fsdb (167 MB)

## CSR Write Summary
| Cmd | Start Cycle | v_addr | h_addr | out_addr |
|-----|------------|--------|--------|----------|
| 1 | 28914 | 0x29124000 | 0x29120000 | 0x29138000 |
| 2 | 32005 | 0x29124000 | 0x29122000 | 0x2913a000 |

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
```

## Architecture Events
| Event | Count |
|-------|-------|
| phase_changed | 10 |
| command_accepted | 2 |
| read_accepted | 4608 |
| read_response_visible | 4608 |
| array_input_accepted | 8192 |
| result_produced | 512 |
| write_accepted | 512 |
| command_complete | 2 |
| **Total** | **18446** |

## Files
| File | Rows | Description |
|------|------|-------------|
| csr_writes.csv | 14 (2×7) | Raw CSR write trace |
| csr_snapshot.json | 2 objects | Per-command snapshot |
| architecture.csv | 18446 | Event trace |
| diagnostic.csv | 5905 | Per-cycle diagnostic |
| manifest.json + SHA256SUMS + README.md | — | Metadata |

## Cycle Definition
Cycle zero = first clk_acc posedge after rst_n_acc deassertion. CSR writes recorded at the posedge where csr_we && (csr_addr[11:4]==0x20) && csr_ready are simultaneously high.

```bash
sha256sum -c SHA256SUMS
```
