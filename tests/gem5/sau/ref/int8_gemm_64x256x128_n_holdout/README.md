# int8_gemm_64x256x128_n_holdout — Current RTL Golden Package

## Fixture

- Role: `holdout`
- Matrix: `64 × 256 × 128` (M × K × N)
- Precision/operation: INT8 GEMM
- Control path: `trans_mode=0x1`, `reuse_mode=0x1`
- Commands: 2
- `flow_loop_times`: 8
- `cutbit`: 8

## Provenance

- RTL head: `d894466f15ea84cffab5596fa87fc33767c78361`
- Simulator: `VCS T-2022.06_Full64`
- Run command: `make yinglong_sim && make yinglong_run`
- Result: `TEST PASSED`, zero matmul mismatches
- FSDB: `/home/xch/work/npu_lpnpu/tmp/rtl_golden_sources/int8_gemm_64x256x128_n_holdout/yinglong.fsdb`
- Sampled CSV: `/home/xch/work/npu_lpnpu/tmp/rtl_golden_sources/int8_gemm_64x256x128_n_holdout/golden_64x256x128_sampled.csv`

Cycle zero is the first sampled SAU clock positive edge in the FSDB
(`1250` ps). CSR writes are accepted when `csr_we && csr_ready` are
both high at that sampled edge. Command start cycles: 28914, 30813.

## Event counts

- `command_accepted`: 2
- `read_accepted`: 2560
- `read_response_visible`: 2560
- `array_input_accepted`: 4096
- `result_produced`: 256
- `write_accepted`: 256
- `command_complete`: 2

## Verification

```bash
sha256sum -c SHA256SUMS
```
