# PLAN3 Step-0 functional references

These packages freeze functional and resource-boundary observations from the
current SAU RTL contract.  Every package contains the pre-simulation memory
image, raw NPI value changes, the testbench result dump, an address-ordered
final output dump, the simulation log, a manifest, and checksums.

| Package | Purpose | Software result |
| --- | --- | --- |
| `int8_gemm_32x32x32_atbd_cutbit8` | Primary ATBD/reuse-A end-to-end oracle | 1024/1024 match |
| `int8_gemm_32x32x32_atbd_cutbit1` | Independent cutbit boundary | 1024/1024 match |
| `int8_gemm_32x32x32_abtd_boundary` | Legal-but-unintended ABTD boundary and end-to-end RTL oracle | RTL output is authoritative; 1005/1024 bytes differ from the software GEMM |
| `int8_gemm_32x32x32_reuse11_probe` | Raw `reuse_mode=11` legality probe | 1024/1024 match |
| `int8_gemm_64x160x64_atbd_flow1_cutbit8` | Two-command flow1 timing and tile-order oracle | RTL actual is the oracle; each fixed-grid 32x32 tile rotates clockwise |
| `int8_gemm_32x512x32_atbd_flow2_cutbit8` | One retained K256 segment followed by final output | 1024/1024 match |
| `int8_gemm_32x768x32_atbd_flow2_cutbit8` | Two consecutive retained K256 segments followed by final output | 1024/1024 match |
| `int8_gemm_chain_32x768x32_to_32x32x32_atbd_cutbit8` | Four-command write-to-Operand-A-read dependency | 1024/1024 match |

`initial_memory.hex` is generated before simulation and is not a post-run
memory dump. Each manifest records the address range covered by
`final_output_memory.hex`; one two-digit byte is stored per line in ascending
address order. Full 256-bit read/write payload changes and their FSDB times are
in `boundary.csv` for the functional and ABTD packages.

The cutbit-1 package also contains a strict `csr_writes.csv` replay extracted
from its frozen FSDB at the SAU instance's accepted posedge boundaries.
The Flow1 package contains two command-local CSR sequences and is registered
as the model's strict byte-exact functional regression.
The ABTD package is also a permanent byte-exact regression. Its ATBD-layout
input is intentionally incompatible with mathematical ABTD GEMM: passing
means reproducing the frozen RTL's actual output, not the software result.
The chain package adds optional verifier-only `memory_dependencies` metadata.
It checks address/order/visibility using the existing architecture trace;
the existing final-memory comparator remains the only functional oracle.

The ATBD cutbit-8 and ABTD boundary packages form the frozen
legal-but-unintended configuration pair: their initial memory, geometry,
elaboration parameters, output range, and other CSR modes are identical;
only `trans_mode` changes from 1 to 2, and their RTL final-memory oracles
differ. Verify that pairing contract with:

```sh
python3 util/sau/validate_functional_pair.py \
  tests/gem5/sau/functional_ref/int8_gemm_32x32x32_atbd_cutbit8 \
  tests/gem5/sau/functional_ref/int8_gemm_32x32x32_abtd_boundary
```

The original ABTD boundary extraction did not retain `csr_we` or `csr_addr`.
Its replay files are therefore explicitly reconstructed from the seven
sampled nonzero `csr_wdata` setup pulses and the fixed firmware write order
`0x200..0x20c`. Accepted writes occur on the following posedge, as
cross-checked against the complete paired ATBD trace and the sampled ABTD
internal start/modes. Reproduce that derivation with:

```sh
python3 util/sau/extract_boundary_csr.py \
  --boundary tests/gem5/sau/functional_ref/int8_gemm_32x32x32_abtd_boundary/boundary.csv \
  --csr-writes tests/gem5/sau/functional_ref/int8_gemm_32x32x32_abtd_boundary/csr_writes.csv \
  --csr-snapshot tests/gem5/sau/functional_ref/int8_gemm_32x32x32_abtd_boundary/csr_snapshot.json \
  --infer-fixed-write-order
```

Rebuild a package with `util/sau/build_plan3_step0_package.py` and the source
paths recorded in its manifest. Verify a package from its directory with:

```sh
sha256sum -c SHA256SUMS
```
