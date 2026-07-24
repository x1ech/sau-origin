# int8_gemm_96x256x256_m_holdout — Current RTL Golden Package

## Fixture

- Role: `holdout`
- Matrix: `96 × 256 × 256` (M × K × N)
- Precision/operation: INT8 GEMM
- Control path: `trans_mode=0x1`, `reuse_mode=0x1`
- Commands: 3
- `flow_loop_times`: 8
- `cutbit`: 8

## Provenance

- RTL head: `d894466f15ea84cffab5596fa87fc33767c78361`
- Simulator: `VCS T-2022.06_Full64`
- Run command: `make yinglong_sim && make yinglong_run`
- Result: `TEST PASSED`, zero matmul mismatches
- FSDB: `/home/xch/work/npu_lpnpu/tmp/rtl_golden_sources/int8_gemm_96x256x256_m_holdout/yinglong.fsdb`
- Sampled CSV: `/home/xch/work/npu_lpnpu/tmp/rtl_golden_sources/int8_gemm_96x256x256_m_holdout/golden_96x256x256_sampled.csv`

Cycle zero is the first sampled SAU clock positive edge in the FSDB
(`1250` ps). CSR writes are accepted when `csr_we && csr_ready` are
both high at that sampled edge. Command start cycles: 28914, 32005, 35096.

## Event counts

- `command_accepted`: 3
- `read_accepted`: 6912
- `read_response_visible`: 6912
- `array_input_accepted`: 12288
- `result_produced`: 768
- `write_accepted`: 768
- `command_complete`: 3

## Verification

```bash
sha256sum -c SHA256SUMS
```
