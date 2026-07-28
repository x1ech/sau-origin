# PLAN3 Step-0 functional references

These packages freeze functional and resource-boundary observations from the
current SAU RTL contract.  Every package contains the pre-simulation memory
image, raw NPI value changes, the testbench result dump, an address-ordered
final output dump, the simulation log, a manifest, and checksums.

| Package | Purpose | Software result |
| --- | --- | --- |
| `int8_gemm_32x32x32_atbd_cutbit8` | Primary ATBD/reuse-A end-to-end oracle | 1024/1024 match |
| `int8_gemm_32x32x32_atbd_cutbit1` | Independent cutbit boundary | 1024/1024 match |
| `int8_gemm_32x32x32_abtd_boundary` | B/transposer boundary oracle | Not an end-to-end oracle: the input image deliberately retains ATBD layout |
| `int8_gemm_32x32x32_reuse11_probe` | Raw `reuse_mode=11` legality probe | 1024/1024 match |
| `int8_gemm_64x160x64_atbd_flow1_cutbit8` | Two-command flow1 timing and tile-order oracle | RTL actual is the oracle; each fixed-grid 32x32 tile rotates clockwise |
| `int8_gemm_32x512x32_atbd_flow2_cutbit8` | One retained K256 segment followed by final output | 1024/1024 match |
| `int8_gemm_32x768x32_atbd_flow2_cutbit8` | Two consecutive retained K256 segments followed by final output | 1024/1024 match |

`initial_memory.hex` is generated before simulation and is not a post-run
memory dump. Each manifest records the address range covered by
`final_output_memory.hex`; one two-digit byte is stored per line in ascending
address order. Full 256-bit read/write payload changes and their FSDB times are
in `boundary.csv` for the functional and ABTD packages.

The cutbit-1 package also contains a strict `csr_writes.csv` replay extracted
from its frozen FSDB at the SAU instance's accepted posedge boundaries.
The Flow1 package contains two command-local CSR sequences and is registered
as the model's strict byte-exact functional regression.

Rebuild a package with `util/sau/build_plan3_step0_package.py` and the source
paths recorded in its manifest. Verify a package from its directory with:

```sh
sha256sum -c SHA256SUMS
```
