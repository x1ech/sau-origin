# PLAN3 Step 0: frozen RTL and functional contract

Status: complete on 2026-07-24.

This document is the implementation gate for PLAN3 Steps 1–3. It records facts
from the elaborated RTL rooted at `/home/xch/work/npu_lpnpu`, commit
`d894466f15ea84cffab5596fa87fc33767c78361`, and from VCS
T-2022.06_Full64 runs of the same `simv`
(`c8048098eb5237ffe5e39bb54d917fd4fe9ef501757f30ef07433854e498df57`).
File-level RTL hashes remain frozen in `RTL_TIMING_PROVENANCE.md`.

## 1. Instantiated hierarchy and parameters

The active path is:

```text
SA_CORE
├── feeder + register_file_in
├── scheduler + mem_addr + register_addr + mem_ctrl
├── u_trans2sa_top (sa_execute/sa_feeder)
│   ├── transposer_tiny × 3
│   └── SA_ENGINE
│       └── SA_ROW × 8
│           └── SA_PE_array × 8
│               └── SA_PE × 4 × 4
└── register_file_out
```

The legacy `trans2sa_top/SA_TOP/SA_row_unit/SA_pe` path is not instantiated and
is not an oracle.

| Parameter | Frozen value | Evidence |
| --- | ---: | --- |
| `SA_SIZE`, `ROW_NUM`, `COL_NUM` | 32 | `SA_pkg.sv`; `SA_CORE.sv:10-11` |
| PE macro / macro grid | 4×4 / 8×8 | `SA_ENGINE.sv` elaboration |
| PE accumulator | signed 24-bit saturating | `SA_PE.sv:47-65` |
| output-RF arithmetic | signed 16-bit add, then int8 saturation | `register_file_out.sv:266-310` |
| external beat | 256 bits / 32 bytes | `SA_CORE.sv:5-6` |
| `REGDEPTH` | 256 | `SA_CORE.sv:12` |
| `SRAM_DELAY`, `ADDR_DELAY` | 3, 2 cycles | `SA_CORE.sv:7-8` |
| clock | 1.667 ns | sampled FSDB manifests |

## 2. Resource abstraction contract

| Resource | Capacity / ports / throughput | Accepted and backpressure contract | Strict boundary and primary stall |
| --- | --- | --- | --- |
| Controller | One active command; no implicit command queue | CSR write is accepted only on `csr_we && csr_ready`; start is a pulse and overlapping unaccepted starts are not replayed later | accepted CSR edge, state, `flow_end`, `sau_crossbar_done`; busy/start conflict |
| Address generators | Independent x/y/flow/instruction counters; step fields 8-bit, burst/cycle fields 6-bit except vertical flow cycle 8-bit | Advance only when the selected read/write request is accepted | external address plus request valid/last; selected port unavailable |
| External SRAM port | One 256-bit request interface; fixed RTL read delay 3 | Native RTL has no ready input. The gem5 timing adapter owns retry/outstanding buffering and must hold a rejected packet stable | `sau_sram_enable/addr/wstrb/rdata/wdata`; retry, response starvation, outstanding limit |
| Input RF | `REGDEPTH=256`, 256-bit data; one modeled read and one write path | Fill consumes visible read response; feed consumes only selected valid data | RF write/read valid+last, A/B payload; empty/full or shared SRAM-port conflict |
| Transpose/reuse | Three `transposer_tiny` banks, each 32 rows × 256 bits; at most one row accepted and one column emitted per bank per cycle | input requires `*_inRow_en && *_ready`; output requires `*_rden && *_ready_o`; T0 has input priority over T1 and T2 is the result path | bank input/output payload, ready/rden/valid/last; bank full, output not consumed, ownership conflict |
| Systolic array | 32×32 PEs, arranged as 8×8 macros of 4×4 PEs; one 32-lane A/B token per enabled cycle | token accepted only when both selected operands and downstream pipeline capacity exist | A/B valid/payload, SA enable, result valid/last; operand starvation or result backpressure |
| Output RF | Two SRAM halves; effective 256 entries; 32 lanes of signed 16-bit accumulation and int8 writeout | result-valid updates one internal address; RAW forwarding applies to consecutive same-address accesses | result payload, internal write address/data/last; read/write half conflict or unload conflict |
| Writeback | One 256-bit beat per valid cycle, four-stage finish indication | strict RTL has no ready; timing adapter advances only on memory acceptance and completes after write visibility | write address/payload/valid/last and done; memory retry/visibility barrier |

When several causes coexist, strict diagnostics use this priority:
memory retry → response/outstanding starvation → input/transposer capacity →
array/result capacity → output/writeback capacity. Secondary causes may also be
reported, but a stalled cycle is counted once.

## 3. State lifetime

| State | reset | accepted start | command done | `sa_flow_mode` effect |
| --- | --- | --- | --- | --- |
| Controller and all address counters | clear | load raw CSR and start from zero | return idle; no queued start | all four modes use the same command boundary |
| Input RF addressing/valid | clear | new fill/read sequence | valid/counters clear | no retained accumulator semantics |
| Transposer T0/T1 | clear | clear when `!sa_flow_mode[1]`; otherwise retain bank contents/ownership until consumed | ready/valid follow bank state | 00 CNORMAL and 01 CTRANS clear; 10 RETAIN and 11 TRETAIN retain |
| Transposer T2/result serializer | clear | starts empty for non-retain; result ordering follows flow mode | drains before non-retain done | CTRANS/TRETAIN select transposed output ordering |
| PE pipeline | clear | control restarts | valid pipeline drains | `SA_ENGINE.keep_mode=sa_flow_mode[1]`; 10/11 retain accumulator state |
| Output RF | reset storage is not a functional initializer; valid/control clear | 00/01 add against zero, 10/11 read and add existing 16-bit value | non-retain unloads before done; retain completion follows keep guard | only bit 1 enables accumulation; bit 0 changes ordering |
| valid/last tokens | clear | generated from accepted transfers | must fully drain before done | retained data never implies retention of stale valid tokens |

## 4. RTL path/equivalence classes

`T` is `trans_mode`, `R` is `reuse_mode`, and `F` is `sa_flow_mode`.
Numeric counter values within one row are equivalent when they select the same
guards; zero is still a distinct wrap/underflow boundary and must be tested.

| path_id | RTL guard / CSR class | Selected resources and effects | Observable boundary | Representative |
| --- | --- | --- | --- | --- |
| T-ABD | `T=00` | no operand transpose; direct result when non-flow-transpose | A/B mux, no T0/T1 load | pending before this path is implemented |
| T-ATBD | `T=01` | A loads operand transposer; result passes transpose path | T0/T1 input/output and final result | `atbd_cutbit8`, `atbd_cutbit1` |
| T-ABTD | `T=10` | B loads operand transposer; initial switch is B-side | B read payload and T0/T1 bank traffic | `abtd_boundary` |
| T-ABDT | `T=11` | no operand transpose load; transposed-result selection | result transposer and serializer | pending before this path is implemented |
| R-none | `R=00` | no resident operand reuse | alternating input-switch path | pending |
| R-A | `R=01` | `A_reuse_flag`; resident A reused | A RF and streamed B | `atbd_cutbit8` |
| R-B | `R=10` | `B_reuse_flag`; resident B reused | B RF and streamed A | pending |
| R-AB | `R=11` | both reuse bits asserted; `|R` and equality guards also active | both reuse selects plus scheduler switch | `reuse11_probe`: executable, completes, 1024/1024 |
| F-normal | `F=00` | clear, normal order, output RF adds zero | ordinary serializer/unload | `atbd_cutbit8` |
| F-trans | `F=01` | clear, transposed output order | T2/result ordering | pending |
| F-retain | `F=10` | retain PE/transposer state; output RF accumulates old value | keep completion, output RAW | pending |
| F-tretain | `F=11` | retain plus transposed order | combined keep/T2/output RAW | pending |

Cross-product mapping is compositional: T selects operand/result transpose,
R selects operand retention and scheduler input switching, and F selects
clear/retain plus output order. A combination is not illegal merely because
its payload maturity is pending. Before implementing a row marked `pending`,
its listed boundary must be captured and bound to this RTL contract.

## 5. CSR support domain

The RTL has no general illegal-value response for these packed fields.
“RTL-executable” therefore means raw modulo-width behavior, not that software
assigns every value a friendly semantic name.

| Field | Width | RTL consumer / path | Int8 GEMM classification | Validation |
| --- | ---: | --- | --- | --- |
| `trans_mode` | 2 | scheduler, operand/result transposers | 00–11 executable | 01 E2E; 10 boundary |
| `reuse_mode` | 2 | scheduler, A/B reuse bits | 00–11 executable; 11 means both bits, not reserved | 01 E2E; 11 E2E probe |
| `sa_flow_mode` | 2 | PE keep, transposer clear, output RF | 00–11 executable | 00 E2E; others pending |
| `register_mode` | 2 | input RF and feeder | 00/01/11 share non-DW guard; 10 selects depthwise/single-column path and is an operator switch | 00 E2E |
| `pe_work_mode` | 2 | `mode_operands()` | 00 MATMUL in scope; 01 CONV, 10 TRANSPOSER, 11 ADD are operator switches | 00 E2E |
| `conv_kernal` | 3 | padding/concat and execute-cycle selection | 0 is plain GEMM; nonzero values select convolution/packing behavior and are operator switches | 0 E2E |
| `stride_flag` | 1 | padding/feeder address selection | 0 plain GEMM; 1 selects stride path and needs its own boundary before implementation | 0 E2E |
| `shift_flag` | 1 | 16-bit packed input/result and bank ownership | 0 int8 GEMM; 1 switches packed/shift datapath | 0 E2E |
| `cutbit` | 5 | arithmetic shift before saturation | 0–31 executable | 1 and 8 E2E |
| `flow_loop_times` | 6 | execute count | 0–63 raw; zero follows RTL counter wrap semantics, not “one” | 1 E2E |
| base/output/bias address | 32 each | address generators | all aligned raw addresses accepted; range/alignment is an integration preflight concern | baseline addresses |
| all step fields | 8 | address generators | 0–255 modulo arithmetic | baseline includes zero and one |
| x/y/flow/ins burst or cycle | 6, except vertical flow cycle 8 | address-generator terminal guards | full raw width; zero is a distinct wrap boundary | baseline value 1/32 |
| valid X/Y start/end | 6 | input valid window/padding | 0–63; start/end ordering follows RTL compare behavior | baseline zero window |
| padding | 4 | padding shifter | 0–15 raw; nonzero needs boundary capture | baseline zero |

Validation maturity is tracked separately as decoded, resource-timed,
data-functional, and end-to-end. An executable raw value may be rejected by a
complete workload only as `rtl_legal_unimplemented`, never mislabeled illegal.

## 6. Golden packages and conclusions

The packages are under `tests/gem5/sau/functional_ref`. Their manifests bind
the RTL commit, dirty-worktree digest, elaboration parameters, `simv`, FSDB,
commands, initial image, output range, and checksums.

Important conclusions:

- `memory.hex` is generated before VCS execution and is the initial image.
- The output range is `[0x29120c00, 0x29121000)` and contains 1024 bytes.
- `cutbit=1` and `cutbit=8` both pass an independent software reference;
  cutbit is not a fixture constant.
- `reuse_mode=11` is executable and completed at 110305 ns in the probe; its
  1024 output bytes match the software result.
- ABTD reaches the B/transposer/read/write boundaries. Its 1005 software
  mismatches are expected because the deliberately unchanged ATBD-layout
  image is not an ABTD mathematical oracle; only the recorded RTL boundaries
  are authoritative for that package.
- The testbench warning that `instruction.hex` has entries after line 4096 is
  a known ROM-size warning; the boot image and all four simulations complete.

No gem5 implementation was changed in Step 0.
