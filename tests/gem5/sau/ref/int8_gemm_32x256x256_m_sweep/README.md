# int8_gemm_32x256x256_m_sweep — Golden Package

## Fixture Info
- **Fixture Name**: 
- **Fixture Role**: coverage (M dimension sweep)
- **Sweep**: M=32 (baseline M=64), K=256 N=256 held constant
- **Precision**: int8
- **Operation**: GEMM (matmul)
- **Matrix**: M=32, K=256, N=256
- **trans_mode**: 0x1 (ATB)
- **reuse_mode**: 0x1 (reuse Operand-A)
- **flow_loop_times**: 8
- **cutbit**: 13
- **SA Size**: 32×32
- **Command Count**: 1

## Simulation
- **Simulator**: VCS T-2022.06_Full64
- **Testbench**: top_yinglong_tb
- **Firmware**: INT8_SAU_MATMUL_TEST_ID_0 (32×256×256)
- **Result**:  (npu_done)
- **FSDB**: sim/vcs/build/yinglong/yinglong.fsdb

## State Machine Trace


## Architecture Events
| Event | Count |
|-------|-------|
| phase_changed | 5 |
| command_accepted | 1 |
| read_accepted | 2304 |
| read_response_visible | 2304 |
| array_input_accepted | 4096 |
| result_produced | 256 |
| write_accepted | 256 (unique addrs: 256) |
| command_complete | 1 |
| **Total** | **9223** |

## Files
| File | Size | Description |
|------|------|-------------|
| csr_writes.csv | 7 rows | Raw CSR write trace |
| csr_snapshot.json | 1 object | CSR register snapshot decoded from raw writes |
| architecture.csv | 9223 events | Architecture event trace |
| diagnostic.csv | 2813 rows | Per-cycle diagnostic (trimmed) |
| manifest.json | — | Metadata and reproducibility |
| SHA256SUMS | — | File integrity checks |
| README.md | — | This file |

## Verification


## CSR Snapshot Validation
All fields decoded from raw csr_writes.csv wdata, verified against RTL csr.sv register field layout.
