# int8_gemm_64x32x256_k_sweep — Golden Package

## Fixture Info
- **Fixture Name**: `int8_gemm_64x32x256_k_sweep`
- **Fixture Role**: coverage (K dimension sweep)
- **Sweep**: K=32 (baseline K=256), M=64 N=256 held constant
- **Precision**: int8
- **Operation**: GEMM (matmul)
- **Matrix**: M=64, K=32, N=256
- **trans_mode**: 0x1 (ATB)
- **reuse_mode**: 0x1 (reuse Operand-A)
- **flow_loop_times**: 1 (per command)
- **cutbit**: 8
- **SA Size**: 32×32
- **Command Count**: 2 (2 M-blocks × 1 flow each)

## Simulation
- **Simulator**: VCS T-2022.06_Full64
- **Testbench**: top_yinglong_tb
- **Firmware**: INT8_SAU_MATMUL_TEST_ID_0 (64×32×256)
- **Result**: `*** ALL TESTS PASSED ***` (npu_done)
- **FSDB**: sim/vcs/build/yinglong/yinglong.fsdb

## CSR Write Summary
| Command | Start Cycle | CSR Writes | Addresses |
|---------|------------|------------|-----------|
| 1 | 28914 | rows 1-7 | v=0x29120800, h=0x29120000, out=0x29126800 |
| 2 | 30163 | rows 8-14 | v=0x29120800, h=0x29120400, out=0x29128800 |

## State Machine Trace
```
   28876 →  28916  (  40 cyc)              IDLE → REGISTER_LOAD
   28916 →  28948  (  32 cyc)     REGISTER_LOAD → TRANSPOSE_LOAD
   28948 →  28980  (  32 cyc)    TRANSPOSE_LOAD → REUSE_LOAD
   28980 →  29013  (  33 cyc)        REUSE_LOAD → D_OUT
   29013 →  29028  (  15 cyc)             D_OUT → REUSE_LOAD
   29028 →  29062  (  34 cyc)        REUSE_LOAD → D_OUT
   29062 →  29104  (  42 cyc)             D_OUT → REUSE_LOAD
   29104 →  29138  (  34 cyc)        REUSE_LOAD → D_OUT
   29138 →  29172  (  34 cyc)             D_OUT → REUSE_LOAD
   29172 →  29206  (  34 cyc)        REUSE_LOAD → D_OUT
   29206 →  29240  (  34 cyc)             D_OUT → REUSE_LOAD
   29240 →  29274  (  34 cyc)        REUSE_LOAD → D_OUT
   29274 →  29308  (  34 cyc)             D_OUT → REUSE_LOAD
   29308 →  29342  (  34 cyc)        REUSE_LOAD → D_OUT
   29342 →  29376  (  34 cyc)             D_OUT → REUSE_LOAD
   29376 →  29410  (  34 cyc)        REUSE_LOAD → D_OUT
   29410 →  29444  (  34 cyc)             D_OUT → REUSE_LOAD
   29444 →  29478  (  34 cyc)        REUSE_LOAD → D_OUT
   29478 →  29512  (  34 cyc)             D_OUT → REGISTER_UNLOAD
   29512 →  29844  ( 332 cyc)   REGISTER_UNLOAD → IDLE
   29844 →  30165  ( 321 cyc)              IDLE → REGISTER_LOAD
   30165 →  30197  (  32 cyc)     REGISTER_LOAD → TRANSPOSE_LOAD
   30197 →  30229  (  32 cyc)    TRANSPOSE_LOAD → REUSE_LOAD
   30229 →  30262  (  33 cyc)        REUSE_LOAD → D_OUT
   30262 →  30277  (  15 cyc)             D_OUT → REUSE_LOAD
   30277 →  30311  (  34 cyc)        REUSE_LOAD → D_OUT
   30311 →  30353  (  42 cyc)             D_OUT → REUSE_LOAD
   30353 →  30387  (  34 cyc)        REUSE_LOAD → D_OUT
   30387 →  30421  (  34 cyc)             D_OUT → REUSE_LOAD
   30421 →  30455  (  34 cyc)        REUSE_LOAD → D_OUT
   30455 →  30489  (  34 cyc)             D_OUT → REUSE_LOAD
   30489 →  30523  (  34 cyc)        REUSE_LOAD → D_OUT
   30523 →  30557  (  34 cyc)             D_OUT → REUSE_LOAD
   30557 →  30591  (  34 cyc)        REUSE_LOAD → D_OUT
   30591 →  30625  (  34 cyc)             D_OUT → REUSE_LOAD
   30625 →  30659  (  34 cyc)        REUSE_LOAD → D_OUT
   30659 →  30693  (  34 cyc)             D_OUT → REUSE_LOAD
   30693 →  30727  (  34 cyc)        REUSE_LOAD → D_OUT
   30727 →  30761  (  34 cyc)             D_OUT → REGISTER_UNLOAD
   30761 →  31093  ( 332 cyc)   REGISTER_UNLOAD → IDLE
```

## Architecture Events
| Event | Count |
|-------|-------|
| phase_changed | 10 |
| command_accepted | 2 |
| read_accepted | 576 |
| read_response_visible | 576 |
| array_input_accepted | 1088 |
| result_produced | 512 |
| write_accepted | 512 |
| command_complete | 2 |
| **Total** | **3278** |

## Files
| File | Size | Description |
|------|------|-------------|
| csr_writes.csv | 14 rows | Raw CSR write trace (14 writes = 2 commands × 7) |
| csr_snapshot.json | 2 objects | CSR register snapshots decoded from raw writes |
| architecture.csv | 3278 events | Architecture event trace |
| diagnostic.csv | 2221 rows | Per-cycle diagnostic (trimmed) |
| manifest.json | — | Metadata and reproducibility |
| SHA256SUMS | — | File integrity checks |
| README.md | — | This file |

## Cycle Definition
Cycle zero = first clk_acc posedge after rst_n_acc deassertion. CSR writes recorded at the posedge where csr_we && (csr_addr[11:4]==0x20) && csr_ready are simultaneously high.

## Verification
```bash
sha256sum -c SHA256SUMS
```
