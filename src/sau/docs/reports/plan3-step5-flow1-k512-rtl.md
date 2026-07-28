# PLAN3 Step 5 Flow1 K512 RTL Diagnostic

Date: 2026-07-27

## Configuration

- RTL worktree: `/home/xch/work/npu_lpnpu`
- Remote baseline at rerun: `origin/yinglong` `f4cbb25`
- Matrix: `M=32, K=512, N=32`
- Precision/cutbit: signed int8, `cutbit=8`
- Local operator change: final K tile uses `sa_flow_mode=1` instead of 0
- Accepted command sequence: `[flow2, flow1]`

The first K256 tile retains its result with flow mode 2. The final K256 tile
requests transpose output with flow mode 1.

## Commands

```bash
cd /home/xch/work/npu_lpnpu/software/benchmarks/yinglong_sau_test
make
make mod3

cd /home/xch/work/npu_lpnpu
make yinglong_sim
make yinglong_run
```

The existing SAU-regression `simv` was reused; RTL sources did not change.

## Functional Result

The built-in comparator uses the normal, non-transposed `D_sa` layout and
therefore reports:

```text
mismatches=1019, elems=1024
*** TEST FAILED (npu_error) ***
```

A separate layout check gives:

| Candidate relation | Mismatches |
| --- | ---: |
| `actual[r][c] == expected[r][c]` | 1019/1024 |
| `actual[r][c] == expected[c][r]` (pure transpose) | 1018/1024 |
| `actual[r][c] == expected[31-c][r]` | 0/1024 |

The observed RTL result is therefore an exact 90-degree clockwise rotation,
not a mathematical pure transpose. On 2026-07-28 the user clarified that gem5
must reproduce the current RTL semantics; under that scope this run is valid
flow-mode-1 alignment evidence.

## Control Timing

SAU clock period in the FSDB is 1667 ps.

| Boundary | Time (ps) | Relative cycles |
| --- | ---: | ---: |
| flow2 start | 47,799,141 | 0 |
| flow2 done | 48,747,664 | 569 after flow2 start |
| flow1 start | 48,904,362 | 94 after flow2 done |
| flow1 result-valid | 49,924,566 | 612 after flow1 start |
| flow1 done | 50,046,257 | 685 after flow1 start |
| write-valid extent | 49,986,245–50,039,589 | 32 cycles |

Both commands are accepted sequentially. The failure is output ordering, not
overlapping start or missing K-tile execution.

## Evidence

```text
/home/xch/work/npu_lpnpu/tmp/flow2_k512/flow21_control.csv
SHA-256 c56620ff7c983af67afade071701c199159c4fd566b0671c863687d8d0470ba3

/home/xch/work/npu_lpnpu/sim/vcs/build/yinglong/matmul_compare.csv
SHA-256 2397626422cc5fda28ec2051128894a08945f551a8ccb5ef5c90f9eb8317b93e

/home/xch/work/npu_lpnpu/sim/vcs/build/yinglong/matmul_mismatches.csv
SHA-256 af2ec7443d6849482dd813c483c43da6af55374ff8eafb16d1a19c85988ccb68

/home/xch/work/npu_lpnpu/sim/vcs/build/yinglong/yinglong.fsdb
SHA-256 6204d80a3079a0b08e3355f9db7d6c0f5a2410fa6e2a9939325276d019674fe9
```
