# int8_gemm_32x32x32_single_flow — Golden Package

## Fixture Info
- **Fixture Name**: `int8_gemm_32x32x32_single_flow`
- **Fixture Role**: coverage
- **Precision**: int8
- **Operation**: GEMM (matmul)
- **Matrix**: M=32, K=32, N=32
- **trans_mode**: 0x1 (ATB)
- **reuse_mode**: 0x1 (reuse Operand-A)
- **flow_loop_times**: 1 (single flow command)
- **cutbit**: 8
- **SA Size**: 32x32
- **Command Count**: 1

## Simulation
- **Simulator**: VCS T-2022.06_Full64
- **Testbench**: top_yinglong_tb (yinglong full-chip simulation)
- **Firmware**: INT8_SAU_MATMUL_TEST_ID_0 (32x32x32)
- **Result**: `*** ALL TESTS PASSED ***` (npu_done at 110855000 ps)
- **FSDB**: sim/vcs/build/yinglong/yinglong.fsdb (5.6 MB)

## Data Sources
- `csr_writes.csv` — CSR interface signals (csr_we/csr_addr/csr_wdata/csr_ready/csr_write_type) extracted from SA_CORE.csr_inst boundary via npi_fsdb_probe
- `csr_snapshot.json` — CSR register state decoded from raw csr_writes.csv wdata; verified against RTL csr.sv register field layout
- `architecture.csv` — 295 events reconstructed from SAU internal signals (start, core_state, input_switch, sram_enable, sram_addr, sram_wstrb, data_A/B valid/last, result valid/last, crossbar_done) via npi_fsdb_probe + Python
- `diagnostic.csv` — 272 rows, per-cycle diagnostic from first CSR write to command_done+2 cycles

## Cycle Definition
Cycle zero = first clk_acc posedge after rst_n_acc deassertion. CSR writes recorded at the posedge where csr_we && csr_addr[11:4]==0x20 && csr_ready are simultaneously high (handshake complete).

## State Machine Trace


## Files
| File | Size | Description |
|------|------|-------------|
| csr_writes.csv | 7 rows | Raw CSR write trace |
| csr_snapshot.json | 1 object | CSR register snapshot decoded from raw writes |
| architecture.csv | 295 events | Architecture event trace |
| diagnostic.csv | 272 rows | Per-cycle diagnostic (trimmed) |
| manifest.json | — | Metadata and reproducibility |
| SHA256SUMS | — | File integrity checks |
| README.md | — | This file |

## Verification
```bash
sha256sum -c SHA256SUMS
```
