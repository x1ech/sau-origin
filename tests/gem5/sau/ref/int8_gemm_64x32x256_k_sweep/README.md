# int8_gemm_64x32x256_k_sweep — Current RTL Golden Package

## Fixture

- Role: `coverage`
- Matrix: `64 × 32 × 256` (M × K × N)
- Precision/operation: INT8 GEMM
- Control path: `trans_mode=0x1`, `reuse_mode=0x1`
- Commands: 2
- `flow_loop_times`: 1
- `cutbit`: 8

## Provenance

- RTL head: `d894466f15ea84cffab5596fa87fc33767c78361`
- Simulator: `VCS T-2022.06_Full64`
- Run command: `make yinglong_sim && make yinglong_run`
- Result: `TEST PASSED`, zero matmul mismatches
- FSDB: `/home/xch/work/npu_lpnpu/tmp/rtl_golden_sources/int8_gemm_64x32x256_k_sweep/yinglong.fsdb`
- Sampled CSV: `/home/xch/work/npu_lpnpu/tmp/rtl_golden_sources/int8_gemm_64x32x256_k_sweep/golden_64x32x256_sampled.csv`

Cycle zero is the first sampled SAU clock positive edge in the FSDB
(`1250` ps). CSR writes are accepted when `csr_we && csr_ready` are
both high at that sampled edge. Command start cycles: 28914, 30163.

## Event counts

- `command_accepted`: 2
- `read_accepted`: 576
- `read_response_visible`: 576
- `array_input_accepted`: 1088
- `result_produced`: 512
- `write_accepted`: 512
- `command_complete`: 2

## Verification

```bash
sha256sum -c SHA256SUMS
```
