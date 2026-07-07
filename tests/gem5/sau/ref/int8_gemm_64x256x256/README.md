# SAU RTL baseline: INT8 GEMM 64x256x256

This directory contains the deterministic RTL timing baseline produced by
Task 1 for the SAU cycle-level gem5 model. The reference files were refreshed
from the corrected package received on 2026-07-07.

The original Task 1 plan asked for a 32x32x32 testcase, but the available RTL
testcase `INT8_SAU_MATMUL_TEST_ID_0` was confirmed to be 64x256x256. The user
accepted this testcase as the current baseline input. Keep the directory name
explicit so these cycle counts are not accidentally reused as a 32x32x32
calibration.

The corrected trace contains two accepted commands (`command_id` 1 and 2) from
the same testcase run. Aggregate counts and latencies in `summary.json` cover
both commands.

Files:

- `architecture.csv`: public seven-column architecture trace consumed by the
  future trace comparator.
- `manifest.json`: source, tool, testcase, and platform identity.
- `summary.json`: validator-derived counts and latency summary.
- `analysis.md`: timing interpretation and token-model guidance.
- `diagnostic.csv`: internal RTL signal dump for debugging only; it is not a
  gem5 public interface.
- `HANDOFF.md`: original handoff notes from the RTL-side agent.
- `SHA256SUMS`: hashes for the original package contents. Paths are relative
  to the original package root, so run it from that layout if checking the full
  package.

Key timing values from `summary.json`:

- command total: 5897 cycles
- first read: cycle 3
- first array input: cycle 269
- first result: cycle 612
- last array input: cycle 5550
- last result: cycle 5630
- first write: cycle 2513
- command complete: cycle 5897
- array fill latency: 343 cycles
- array drain latency: 80 cycles
- last result to first write: -3117 cycles
- last write to complete: 4 cycles
