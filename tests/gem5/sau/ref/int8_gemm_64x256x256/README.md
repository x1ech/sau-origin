# SAU RTL baseline: INT8 GEMM 64x256x256

This directory contains the deterministic RTL timing baseline produced by
Task 1 for the SAU cycle-level gem5 model.

The original Task 1 plan asked for a 32x32x32 testcase, but the available RTL
testcase `INT8_SAU_MATMUL_TEST_ID_0` was confirmed to be 64x256x256. The user
accepted this testcase as the current baseline input. Keep the directory name
explicit so these cycle counts are not accidentally reused as a 32x32x32
calibration.

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

- command total: 2036 cycles
- first read: cycle 3
- first array input: cycle 77
- first result: cycle 324
- last array input: cycle 1689
- last result: cycle 1769
- first write: cycle 1777
- command complete: cycle 2036
- array fill latency: 247 cycles
- array drain latency: 80 cycles
- last result to first write: 8 cycles
- last write to complete: 4 cycles
