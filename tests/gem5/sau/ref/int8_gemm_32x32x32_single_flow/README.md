# int8_gemm_32x32x32_single_flow — Current RTL Golden Package

## Fixture

- Role: `coverage`
- Matrix: `32 × 32 × 32` (M × K × N)
- Precision/operation: INT8 GEMM
- Control path: `trans_mode=0x1`, `reuse_mode=0x1`
- Commands: 1
- `flow_loop_times`: 1
- `cutbit`: 8

## Provenance

- RTL head: `d894466f15ea84cffab5596fa87fc33767c78361`
- Simulator: `VCS T-2022.06_Full64`
- Run command: `make yinglong_sim && make yinglong_run`
- Result: `TEST PASSED`, zero matmul mismatches
- FSDB: `/home/xch/work/npu_lpnpu/tmp/rtl_golden_sources/int8_gemm_32x32x32_single_flow/yinglong.fsdb`
- Sampled CSV: `/home/xch/work/npu_lpnpu/tmp/rtl_golden_sources/int8_gemm_32x32x32_single_flow/golden_32x32x32_sampled.csv`

Cycle zero is the first sampled SAU clock positive edge in the FSDB
(`1250` ps). CSR writes are accepted when `csr_we && csr_ready` are
both high at that sampled edge. Command start cycles: 29112.

## Event counts

- `command_accepted`: 1
- `read_accepted`: 64
- `read_response_visible`: 64
- `array_input_accepted`: 96
- `result_produced`: 32
- `write_accepted`: 32
- `command_complete`: 1

## Verification

```bash
sha256sum -c SHA256SUMS
```
