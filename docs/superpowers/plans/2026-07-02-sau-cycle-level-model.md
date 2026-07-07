# SAU int8 GEMM Cycle-Level Model Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and RTL-calibrate the first usable gem5 SAU cycle-level model for int8 GEMM, with real 256-bit timing memory traffic, retry/backpressure, token-pipeline timing, trace output, and explanatory statistics.

**Architecture:** Implement a `ClockedObject` whose scheduler moves metadata tokens through operand loading, systolic-array execution, draining, and writeback. A custom `RequestPort` issues each 256-bit read or write beat at an explicit SAU clock edge; fixed-latency RTL and gem5 runs emit the same CSV event schema for exact differential comparison.

**Tech Stack:** C++17, gem5 SimObject/Python configuration, gem5 timing ports and event queue, gem5 statistics, GoogleTest, Python 3 `unittest`, SystemVerilog/VCS RTL regression.

---

## Scope and execution rules

This plan implements only the first milestone from
`docs/superpowers/specs/2026-07-02-sau-cycle-level-model-design.md`:

- direct synthetic command injection;
- int8 GEMM using the RTL reuse path, where reuse means routing operands
  through `register_file_in`; the first milestone models the observed
  Operand-A preload and resident array-input behavior without CSR decode;
- real timing reads and writes;
- fixed-memory RTL calibration;
- variable-memory retry and backpressure validation.

Do not add custom RISC-V instructions, CSR wiring, interrupts, functional
arithmetic, int16, convolution, padding, or a generic transpose engine in this
plan. Reuse is in scope in the RTL sense of using `register_file_in`: the first
milestone hardwires the measured int8 GEMM behavior where Operand-A is loaded
once into the SAU-side `register_file_in` abstraction and then reused for array
input while Operand-B streams from SRAM responses. The corrected RTL baseline
shows `TRANSPOSE_LOAD` and `TRANSPOSE_CLIP` as high-frequency states in this
matmul path, so their cycle-level timing effect is in scope for the first
milestone. Decoding CSR reuse fields and covering every operator-specific
`register_file_in`/transpose timing variant are later work.

The direct-command model should assume the target operator uses
`register_file_in`, matching the current RTL CSR-configured operator path.

The work spans two repositories. Use separate feature branches and separate
commits:

- `/home/xch/workspace/npu_lpnpu`: RTL trace instrumentation only.
- `/home/xch/workspace/gem5`: model, tests, comparator, golden trace, and docs.

Before each task, run `git status --short` in the affected repository. Preserve
all pre-existing user changes. Do not clean, reset, delete, or reformat
unrelated files.

## File structure

### RTL repository

- Modify `sim/testbench/tb/top_sau_regress_tb.sv`
  - Emit architecture-relevant SAU events in a stable CSV format.
- Modify `sim/vcs/script/case_sau_regress/Makefile`
  - Add an opt-in trace output argument and one deterministic timing target.

### gem5 repository

- Create `src/sau/SConscript`
  - Register the SimObject, C++ sources, debug flag, and unit tests.
- Create `src/sau/Sau.py`
  - Declare clock, port, timing, capacity, trace, and synthetic-command
    parameters.
- Create `src/sau/types.hh`
  - Shared enums and immutable command/stream/token records.
- Create `src/sau/command.hh`, `src/sau/command.cc`
  - Command construction and admission validation.
- Create `src/sau/address_generator.hh`, `src/sau/address_generator.cc`
  - Ordered external 256-bit read-beat generation: Operand-A preload once per
    instruction, then Operand-B streaming per flow.
- Create `src/sau/a_register_file.hh`, `src/sau/a_register_file.cc`
  - Architecture-level `register_file_in` abstraction for resident Operand-A
    beats and virtual Operand-A array-input reuse.
- Create `src/sau/token_pipeline.hh`, `src/sau/token_pipeline.cc`
  - Capacity-limited token buffers and array fill/II/drain behavior.
- Create `src/sau/memory_port.hh`, `src/sau/memory_port.cc`
  - Request packet ownership, retry, response, and outstanding accounting.
- Create `src/sau/trace_writer.hh`, `src/sau/trace_writer.cc`
  - Common CSV event output.
- Create `src/sau/sau_model.hh`, `src/sau/sau_model.cc`
  - Clock-edge ordering, scheduler phases, statistics, drain, and completion.
- Create `src/sau/command.test.cc`
  - Command validation unit tests.
- Create `src/sau/address_generator.test.cc`
  - Address and beat-order unit tests.
- Create `src/sau/token_pipeline.test.cc`
  - Fill, II, capacity, backpressure, and token-conservation tests.
- Create `configs/example/sau_timing.py`
  - Standalone timing-memory configuration and synthetic command injection.
- Create `tests/gem5/sau/test_sau.py`
  - gem5 system-level quick tests.
- Create `tests/gem5/sau/ref/int8_gemm.csv`
  - Versioned RTL reference trace for one deterministic GEMM command.
- Create `util/sau/compare_trace.py`
  - Strict and causal trace comparison modes.
- Create `util/sau/compare_trace_test.py`
  - Python comparator unit tests.
- Modify `docs/superpowers/specs/2026-07-02-sau-cycle-level-model-design.md`
  - Record measured timing-profile values and the exact RTL reference case.

## Stable interfaces used by all tasks

Use these names and types consistently:

```cpp
namespace gem5::sau
{

enum class Operation : uint8_t { Gemm };
enum class Precision : uint8_t { Int8 };
enum class StreamKind : uint8_t { OperandA, OperandB, Output };
enum class Phase : uint8_t {
    Idle, OperandLoad, ArrayActive, ArrayDrain, Writeback, Complete
};
enum class EventKind : uint8_t {
    CommandAccepted, ReadAccepted, ReadResponseVisible,
    ArrayInputAccepted, ResultProduced, WriteAccepted,
    PhaseChanged, CommandComplete
};

struct StreamDesc
{
    Addr base = 0;
    uint32_t beats = 0;
    uint32_t strideBytes = 32;
    uint32_t flowStrideBytes = 0;
    uint32_t instructionStrideBytes = 0;
};

struct SauCommand
{
    uint64_t id = 0;
    Operation operation = Operation::Gemm;
    Precision precision = Precision::Int8;
    StreamDesc operandA;
    StreamDesc operandB;
    StreamDesc output;
    uint32_t flowLoops = 1;
    uint32_t instructionLoops = 1;
    uint32_t workItems = 0;
};

struct Beat
{
    StreamKind stream;
    Addr address;
    uint32_t index;
    bool last;

    bool operator==(const Beat &other) const
    {
        return stream == other.stream && address == other.address &&
               index == other.index && last == other.last;
    }
};

struct PipelineToken
{
    uint64_t commandId;
    uint32_t index;
    Cycles readyCycle;
    bool last;
};

} // namespace gem5::sau
```

For the first milestone, the baseline read and array-input policy follows the
RTL-observed matmul reuse datapath. In this RTL, “reuse” means whether operands
use `register_file_in`; this plan models that boundary at cycle/timing level,
not as a separate functional feature:

```text
external SRAM reads:
  for each instruction:
    preload all Operand-A beats into register_file_in once
    for each flow:
      stream all Operand-B beats from SRAM

array input:
  when A for the current instruction is resident and one B beat is visible:
    accept B from the B response queue
    accept a virtual A beat from resident register_file_in
```

Repeated A array-input beats do not imply repeated external A SRAM reads,
because they are supplied from the modeled `register_file_in` residency. This
is the RTL reuse path, but it is still not RTL-register-accurate modeling of
register ports, banks, stored data values, or transpose behavior. The RTL trace
created in Task 1 is the authority for one-cycle offsets and phase boundaries.

---

### Task 1: Capture a deterministic RTL timing reference

**Files:**
- Modify: `/home/xch/workspace/npu_lpnpu/sim/testbench/tb/top_sau_regress_tb.sv`
- Modify: `/home/xch/workspace/npu_lpnpu/sim/vcs/script/case_sau_regress/Makefile`
- Reference: `/home/xch/workspace/npu_lpnpu/testcase/sau_regress_matmul/INT8_SAU_MATMUL_TEST_ID_0/`

- [ ] **Step 1: Add an opt-in CSV trace block to the testbench**

Add one file descriptor, a cycle counter reset with the DUT, and a
`+sau_trace=<path>` plusarg. Emit this exact header:

```systemverilog
cycle,event,command_id,stream,address,beat,phase
```

Use one monitor block with the existing DUT hierarchy
`u_dut.u_dut_kui.SAU_1_inst` and these event names:

```systemverilog
always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        sau_trace_cycle <= 0;
    end else begin
        sau_trace_cycle <= sau_trace_cycle + 1;
        if (sau_trace_fd != 0) begin
            if (u_dut.u_dut_kui.SAU_1_inst.start)
                trace_event("command_accepted", "none", 0, 0,
                            sau_trace_phase);
            if (u_dut.u_dut_kui.SAU_1_inst.sau_sram_enable &&
                u_dut.u_dut_kui.SAU_1_inst.sau_sram_wstrb == '0)
                trace_event("read_accepted", trace_read_stream(),
                            u_dut.u_dut_kui.SAU_1_inst.sau_sram_addr,
                            sau_read_beat, sau_trace_phase);
            if (u_dut.u_dut_kui.SAU_1_inst.core_register_data_out_valid)
                trace_event("read_response_visible", trace_input_stream(),
                            0, sau_read_response_beat,
                            sau_trace_phase);
            if (u_dut.u_dut_kui.SAU_1_inst.data_A_valid &&
                u_dut.u_dut_kui.SAU_1_inst.data_B_valid)
                trace_event("array_input_accepted", "array", 0,
                            sau_array_beat, sau_trace_phase);
            if (u_dut.u_dut_kui.SAU_1_inst.result_final_valid_o)
                trace_event("result_produced", "output", 0,
                            sau_result_beat, sau_trace_phase);
            if (u_dut.u_dut_kui.SAU_1_inst.sau_sram_enable &&
                u_dut.u_dut_kui.SAU_1_inst.sau_sram_wstrb != '0)
                trace_event("write_accepted", "output",
                            u_dut.u_dut_kui.SAU_1_inst.sau_sram_addr,
                            sau_write_beat, sau_trace_phase);
            if (u_dut.u_dut_kui.SAU_1_inst.sau_crossbar_done)
                trace_event("command_complete", "none", 0, 0,
                            sau_trace_phase);
        end
    end
end
```

Implement `trace_read_stream()` and `trace_input_stream()` from
`input_switch_s`; do not infer streams from addresses. Increment each beat
counter only when its corresponding event is emitted. Maintain the abstract
`sau_trace_phase` independently of the RTL enum: `idle` before start,
`operand_load` at command acceptance, `array_active` at the first array-input
event, `array_drain` when the final A input is accepted
(`data_A_last && last_flow_time_f && last_ins_time_s`), `writeback` at the
first accepted output write after that point, and `complete` on
`sau_crossbar_done`. Emit one `phase_changed` row on each abstract transition.
Assign stable command IDs in acceptance order, starting at `1`. CSV cycles
remain raw testbench cycles; the comparator normalizes both traces to their
respective first `command_accepted` cycle. The corrected Task 1 package for
`INT8_SAU_MATMUL_TEST_ID_0` contains two commands, so do not assume the trace
is single-command.

- [ ] **Step 2: Add the deterministic Make target**

Add:

```make
TRACE_CSV ?= $(LPNPU_HOME)/sim/vcs/build/sau_regress/int8_gemm_timing.csv

timing_trace: compile
	@mkdir -p $(dir $(TRACE_CSV))
	cd $(BUILD_DIR) && ./simv $(SIM_FLAG) \
	    +firmware=$(REGRESS_DIR)/INT8_SAU_MATMUL_TEST_ID_0 \
	    +sau_trace=$(TRACE_CSV) \
	    +TIMEOUT_NS=5000000
	@test -s $(TRACE_CSV)
	@grep -q "command_accepted" $(TRACE_CSV)
	@grep -q "command_complete" $(TRACE_CSV)
```

Use the variable names already present in this Makefile if they differ; do not
duplicate the simulator path or compile flags.

- [ ] **Step 3: Run the RTL trace target**

Run:

```bash
make -f sim/vcs/script/case_sau_regress/Makefile timing_trace
```

Expected:

- simulation reports `TEST PASSED`;
- `sim/vcs/build/sau_regress/int8_gemm_timing.csv` is non-empty;
- the trace has at least one `command_accepted` and the same number of
  `command_complete` rows;
- cycles are monotonically increasing.

If VCS is unavailable, stop this task and record the exact command and missing
tool. Do not fabricate the golden CSV.

- [ ] **Step 4: Commit the RTL instrumentation**

```bash
git add sim/testbench/tb/top_sau_regress_tb.sv \
        sim/vcs/script/case_sau_regress/Makefile
git commit -m "test: emit SAU cycle timing trace"
```

---

### Task 2: Add the common trace comparator

**Files:**
- Create: `util/sau/compare_trace.py`
- Create: `util/sau/compare_trace_test.py`
- Create: `tests/gem5/sau/ref/int8_gemm.csv`

- [ ] **Step 1: Copy the measured RTL trace into the gem5 reference tree**

Run:

```bash
mkdir -p tests/gem5/sau/ref
cp ../npu_lpnpu/sim/vcs/build/sau_regress/int8_gemm_timing.csv \
   tests/gem5/sau/ref/int8_gemm.csv
```

Expected: the copied file begins with the seven-column header from Task 1.

- [ ] **Step 2: Write failing comparator tests**

Create tests that use temporary files:

```python
class TraceComparatorTest(unittest.TestCase):
    def test_strict_accepts_identical_rows(self):
        result = compare_rows(ROWS, ROWS, mode="strict")
        self.assertEqual([], result)

    def test_strict_reports_one_cycle_offset(self):
        actual = [dict(ROWS[0], cycle="2")]
        errors = compare_rows(ROWS, actual, mode="strict")
        self.assertIn("cycle", errors[0])

    def test_causal_allows_latency_but_not_reordering(self):
        delayed = [dict(row, cycle=str(int(row["cycle"]) + 7))
                   for row in ROWS]
        self.assertEqual([], compare_rows(ROWS, delayed, mode="causal"))
        self.assertNotEqual(
            [], compare_rows(ROWS, list(reversed(delayed)), mode="causal")
        )
```

- [ ] **Step 3: Run the test and verify failure**

Run:

```bash
python3 -m unittest util.sau.compare_trace_test -v
```

Expected: FAIL because `compare_rows` does not exist.

- [ ] **Step 4: Implement strict and causal comparison**

`read_trace(path)` must validate the exact header, parse cycles as integers,
reject unknown event names, require at least one `command_accepted`, and
subtract the first `command_accepted` cycle from every row. Multiple commands
are legal and are distinguished by `command_id`. `compare_rows(expected,
actual, "strict")` compares every normalized field and row count. `"causal"`
compares event/stream/address/beat/phase sequence and checks nondecreasing
actual cycles while ignoring absolute cycle differences.

The CLI must be:

```bash
python3 util/sau/compare_trace.py \
    --mode strict EXPECTED.csv ACTUAL.csv
```

Exit `0` on match and `1` after printing every mismatch on failure.

- [ ] **Step 5: Run the comparator tests**

Run:

```bash
python3 -m unittest util.sau.compare_trace_test -v
```

Expected: 3 tests PASS.

- [ ] **Step 6: Commit the trace contract**

```bash
git add util/sau/compare_trace.py util/sau/compare_trace_test.py \
        tests/gem5/sau/ref/int8_gemm.csv
git commit -m "test: add SAU timing trace comparator"
```

---

### Task 3: Add command types and admission validation

**Files:**
- Create: `src/sau/SConscript`
- Create: `src/sau/types.hh`
- Create: `src/sau/command.hh`
- Create: `src/sau/command.cc`
- Create: `src/sau/command.test.cc`

- [ ] **Step 1: Register the first unit test**

Start `src/sau/SConscript` with:

```python
Import("*")

Source("command.cc")
GTest("command.test", "command.test.cc", "command.cc")
```

- [ ] **Step 2: Write failing validation tests**

Cover one valid command and each rejected condition:

```cpp
TEST(SauCommand, AcceptsAlignedInt8Gemm)
{
    auto cmd = makeCommand();
    EXPECT_NO_THROW(validateCommand(cmd, 32));
}

TEST(SauCommand, RejectsZeroBeatStream)
{
    auto cmd = makeCommand();
    cmd.operandA.beats = 0;
    EXPECT_THROW(validateCommand(cmd, 32), std::invalid_argument);
}

TEST(SauCommand, RejectsMisalignedAddress)
{
    auto cmd = makeCommand();
    cmd.output.base = 0x3004;
    EXPECT_THROW(validateCommand(cmd, 32), std::invalid_argument);
}

TEST(SauCommand, RejectsInconsistentWorkItems)
{
    auto cmd = makeCommand();
    cmd.workItems = cmd.operandA.beats * cmd.flowLoops *
                    cmd.instructionLoops - 1;
    EXPECT_THROW(validateCommand(cmd, 32), std::invalid_argument);
}
```

`makeCommand()` uses
A=`{0x1000, 4, 32, 0x100, 0x1000}`,
B=`{0x2000, 4, 32, 0x100, 0x1000}`,
output=`{0x3000, 4, 32, 0, 0x1000}`, one flow, one instruction, and four
work items.

- [ ] **Step 3: Verify the test fails to compile**

Run:

```bash
scons build/ALL/sau/command.test.opt -j4
```

Expected: FAIL because `types.hh` and validation functions do not exist.

- [ ] **Step 4: Implement the stable types and validator**

Add the stable interfaces declared above. Expose:

```cpp
void validateCommand(const SauCommand &command, unsigned beatBytes);
```

Validation must require:

- `beatBytes == 32`;
- all three bases aligned to `beatBytes`;
- nonzero beats, stride, loops, and work items;
- `workItems == operandA.beats * flowLoops * instructionLoops`;
- `output.beats * instructionLoops == workItems`;
- operation `Gemm` and precision `Int8`.

Use checked 64-bit multiplication before narrowing to prevent overflow.

- [ ] **Step 5: Build and run the unit test**

```bash
scons build/ALL/sau/command.test.opt -j4
./build/ALL/sau/command.test.opt
```

Expected: all `SauCommand` tests PASS.

- [ ] **Step 6: Commit**

```bash
git add src/sau/SConscript src/sau/types.hh src/sau/command.hh \
        src/sau/command.cc src/sau/command.test.cc
git commit -m "feat: define SAU timing command"
```

---

### Task 4: Implement deterministic external beat generation

**Files:**
- Create: `src/sau/address_generator.hh`
- Create: `src/sau/address_generator.cc`
- Create: `src/sau/address_generator.test.cc`
- Modify: `src/sau/SConscript`

- [ ] **Step 1: Register and write failing tests**

Add:

```python
Source("address_generator.cc")
GTest("address_generator.test", "address_generator.test.cc",
      "address_generator.cc", "command.cc")
```

Test this exact external read sequence:

```cpp
TEST(AddressGenerator, PreloadsAThenStreamsB)
{
    SauCommand cmd = makeCommand();
    AddressGenerator gen(cmd);
    EXPECT_THAT(collect(gen), ElementsAre(
        Beat{StreamKind::OperandA, 0x1000, 0, false},
        Beat{StreamKind::OperandA, 0x1020, 1, false},
        Beat{StreamKind::OperandA, 0x1040, 2, false},
        Beat{StreamKind::OperandA, 0x1060, 3, true},
        Beat{StreamKind::OperandB, 0x2000, 0, false},
        Beat{StreamKind::OperandB, 0x2020, 1, false},
        Beat{StreamKind::OperandB, 0x2040, 2, false},
        Beat{StreamKind::OperandB, 0x2060, 3, true}));
}
```

Add a two-flow test named `PreloadsAOnceThenStreamsBForEachFlow`: Operand-A is
loaded once for the instruction, while Operand-B repeats for each flow using
`base + flow * flowStrideBytes + beat * strideBytes`.

Add a two-instruction test named `AppliesInstructionFlowAndBeatStrides`:
Operand-A uses `base + instruction * instructionStrideBytes + beat *
strideBytes` because A flow reuse is internal after preload; Operand-B uses
`base + instruction * instructionStrideBytes + flow * flowStrideBytes + beat *
strideBytes`. Neither test may modify the original `SauCommand`.

- [ ] **Step 2: Verify failure**

```bash
scons build/ALL/sau/address_generator.test.opt -j4
```

Expected: FAIL because `AddressGenerator` is undefined.

- [ ] **Step 3: Implement the generator**

Expose:

```cpp
class AddressGenerator
{
  public:
    explicit AddressGenerator(const SauCommand &command);
    bool empty() const;
    const Beat &front() const;
    void pop();
    uint64_t totalReadBeats() const;
};
```

Precompute only stream cursors, not a vector of every beat. `front()` returns
Operand-A preload beats until the current instruction's A data is loaded, then
Operand-B beats for each flow. `pop()` advances beat, flow, and instruction
cursors. `last` is true at the end of each external stream occurrence.

- [ ] **Step 4: Run tests**

```bash
scons build/ALL/sau/address_generator.test.opt -j4
./build/ALL/sau/address_generator.test.opt
```

Expected: all generator tests PASS.

- [ ] **Step 5: Commit**

```bash
git add src/sau/address_generator.hh src/sau/address_generator.cc \
        src/sau/address_generator.test.cc src/sau/SConscript
git commit -m "feat: generate SAU operand beats"
```

---

### Task 4.5: Model the Operand-A `register_file_in` reuse path

**Files:**
- Create: `src/sau/a_register_file.hh`
- Create: `src/sau/a_register_file.cc`
- Create: `src/sau/a_register_file.test.cc`
- Modify: `src/sau/SConscript`

- [ ] **Step 1: Register and write failing tests**

Add:

```python
Source("a_register_file.cc")
GTest("a_register_file.test", "a_register_file.test.cc",
      "a_register_file.cc", "command.cc")
```

Add these tests:

```cpp
TEST(ARegisterFileIn, TracksAReadPreloadPerInstruction);
TEST(ARegisterFileIn, ReportsArrayReuseMoreThanExternalReads);
TEST(ARegisterFileIn, ProducesVirtualAArrayInputBeatsAfterPreload);
TEST(ARegisterFileIn, RejectsNonAExternalLoad);
```

The key invariant is:

```cpp
EXPECT_EQ(registerFile.totalExternalLoadBeats(), command.operandA.beats);
EXPECT_EQ(registerFile.totalArrayInputBeats(),
          command.operandA.beats * command.flowLoops *
          command.instructionLoops);
```

- [ ] **Step 2: Verify failure**

```bash
scons build/ALL/sau/a_register_file.test.opt -j4
```

Expected: FAIL because `ARegisterFileIn` is undefined.

- [ ] **Step 3: Implement the abstraction**

Expose:

```cpp
class ARegisterFileIn
{
  public:
    explicit ARegisterFileIn(const SauCommand &command);

    void load(const Beat &beat);

    bool instructionReady(uint32_t instruction) const;
    uint32_t loadedBeats(uint32_t instruction) const;

    uint64_t totalExternalLoadBeats() const;
    uint64_t totalArrayInputBeats() const;

    Beat arrayInputBeat(uint32_t instruction, uint32_t flow, uint32_t beat,
                        uint32_t arrayIndex) const;
};
```

`load()` accepts only `StreamKind::OperandA` beats and records residency per
instruction. `arrayInputBeat()` returns a virtual Operand-A beat with address
zero because array-input trace events are internal SAU events, not external
SRAM accesses.

- [ ] **Step 4: Run tests**

```bash
scons build/ALL/sau/a_register_file.test.opt -j4
./build/ALL/sau/a_register_file.test.opt
```

Expected: all A-register-file tests PASS.

- [ ] **Step 5: Commit**

```bash
git add src/sau/a_register_file.hh src/sau/a_register_file.cc \
        src/sau/a_register_file.test.cc src/sau/SConscript
git commit -m "feat: model SAU A register file input"
```

---

### Task 5: Implement token buffers and array timing

**Files:**
- Create: `src/sau/token_pipeline.hh`
- Create: `src/sau/token_pipeline.cc`
- Create: `src/sau/token_pipeline.test.cc`
- Modify: `src/sau/SConscript`

- [ ] **Step 1: Register and write failing pipeline tests**

Add tests for:

```cpp
TEST(TokenBuffer, RefusesPushAtCapacity);
TEST(ArrayPipeline, ProducesAfterFillLatency);
TEST(ArrayPipeline, EnforcesInitiationInterval);
TEST(ArrayPipeline, StopsAtMaximumInFlight);
TEST(ArrayPipeline, ConservesAcceptedTokens);
```

The concrete fill/II test uses fill latency 3, II 1, accepts token 0 at cycle
0 and token 1 at cycle 1, then expects outputs at cycles 3 and 4.

- [ ] **Step 2: Verify failure**

```bash
scons build/ALL/sau/token_pipeline.test.opt -j4
```

Expected: FAIL because the buffer and pipeline are undefined.

- [ ] **Step 3: Implement capacity and pipeline rules**

Expose:

```cpp
class TokenBuffer
{
  public:
    explicit TokenBuffer(size_t capacity);
    bool canPush() const;
    void push(PipelineToken token);
    const PipelineToken &front() const;
    void pop();
    size_t size() const;
};

class ArrayPipeline
{
  public:
    ArrayPipeline(Cycles fillLatency, Cycles initiationInterval,
                  size_t maxInFlight);
    bool canAccept(Cycles now) const;
    void accept(uint64_t commandId, uint32_t index, bool last, Cycles now);
    bool hasReady(Cycles now) const;
    PipelineToken takeReady(Cycles now);
    size_t inFlight() const;
};
```

Store tokens ordered by `readyCycle = acceptCycle + fillLatency`. Advance no
hidden cycle counter inside the class; the owning model passes the current
SAU cycle explicitly.

- [ ] **Step 4: Run tests**

```bash
scons build/ALL/sau/token_pipeline.test.opt -j4
./build/ALL/sau/token_pipeline.test.opt
```

Expected: all token tests PASS.

- [ ] **Step 5: Commit**

```bash
git add src/sau/token_pipeline.hh src/sau/token_pipeline.cc \
        src/sau/token_pipeline.test.cc src/sau/SConscript
git commit -m "feat: model SAU token pipeline"
```

---

### Task 6: Add the SimObject skeleton, parameters, trace writer, and stats

**Files:**
- Create: `src/sau/Sau.py`
- Create: `src/sau/trace_writer.hh`
- Create: `src/sau/trace_writer.cc`
- Create: `src/sau/sau_model.hh`
- Create: `src/sau/sau_model.cc`
- Modify: `src/sau/SConscript`

- [ ] **Step 1: Declare the SimObject**

Create `Sau.py` with:

```python
class SauModel(ClockedObject):
    type = "SauModel"
    cxx_class = "gem5::sau::SauModel"
    cxx_header = "sau/sau_model.hh"

    system = Param.System(Parent.any, "System used for requestor IDs")
    memory = RequestPort("SAU timing-memory request port")
    beat_bytes = Param.Unsigned(32, "SRAM beat size")
    read_issue_width = Param.Unsigned(1, "Maximum accepted reads per cycle")
    write_issue_width = Param.Unsigned(1, "Maximum accepted writes per cycle")
    max_outstanding_reads = Param.Unsigned(4, "Read response slots")
    max_outstanding_writes = Param.Unsigned(4, "Write response slots")
    input_buffer_entries = Param.Unsigned(8, "Returned operand token slots")
    output_buffer_entries = Param.Unsigned(8, "Result token slots")
    array_fill_cycles = Param.Cycles(1, "Calibrated fill latency")
    array_ii_cycles = Param.Cycles(1, "Array initiation interval")
    array_capacity = Param.Unsigned(16, "Maximum in-flight work tokens")
    command_start_cycles = Param.Cycles(1, "Command acceptance to first issue")
    trace_file = Param.String("", "CSV timing trace path")
    exit_on_done = Param.Bool(True, "Exit simulation when command completes")

    command_id = Param.UInt64(1, "Synthetic command ID")
    a_base = Param.Addr(0x1000, "Operand A base")
    b_base = Param.Addr(0x2000, "Operand B base")
    output_base = Param.Addr(0x3000, "Output base")
    a_beats = Param.Unsigned(4, "Operand A beats per flow")
    b_beats = Param.Unsigned(4, "Operand B beats per flow")
    output_beats = Param.Unsigned(4, "Output beats")
    flow_loops = Param.Unsigned(1, "Flow repetitions")
    instruction_loops = Param.Unsigned(1, "Instruction repetitions")
    a_flow_stride = Param.Unsigned(0x100, "A bytes between flows")
    b_flow_stride = Param.Unsigned(0x100, "B bytes between flows")
    output_instruction_stride = Param.Unsigned(
        0x1000, "Output bytes between instructions"
    )
```

- [ ] **Step 2: Register build products**

Add:

```python
SimObject("Sau.py", sim_objects=["SauModel"])
Source("trace_writer.cc")
Source("sau_model.cc")
DebugFlag("SAU")
```

- [ ] **Step 3: Implement the trace writer**

`TraceWriter` opens only when `trace_file` is non-empty, writes the exact
seven-column header, and exposes:

```cpp
void emit(uint64_t cycle, EventKind event, uint64_t commandId,
          std::string_view stream, Addr address, uint32_t beat,
          Phase phase);
```

Flush on `CommandComplete`. Use stable lowercase strings matching Task 1.

- [ ] **Step 4: Implement a buildable idle model and statistics group**

`SauModel` derives from `ClockedObject`. Add `getPort("memory")`, one
`EventFunctionWrapper tickEvent`, `startup()`, `tick()`, `submitCommand()`,
`drain()`, and a nested `statistics::Group`.

The first implementation builds the synthetic command at `startup()`, sets
`workItems = a_beats * flow_loops * instruction_loops`, validates and accepts
it, emits `command_accepted`, schedules the next clock edge, and remains in
`OperandLoad`. Register scalar statistics for commands, cycles, bytes,
utilization, and every stall reason named in the design spec.

- [ ] **Step 5: Build gem5**

```bash
scons build/ALL/gem5.opt -j4
```

Expected: build succeeds and generated `params/SauModel.hh` is present.

- [ ] **Step 6: Commit**

```bash
git add src/sau/Sau.py src/sau/trace_writer.hh src/sau/trace_writer.cc \
        src/sau/sau_model.hh src/sau/sau_model.cc src/sau/SConscript
git commit -m "feat: add SAU timing SimObject"
```

---

### Task 7: Implement the timing memory port

**Files:**
- Create: `src/sau/memory_port.hh`
- Create: `src/sau/memory_port.cc`
- Modify: `src/sau/sau_model.hh`
- Modify: `src/sau/sau_model.cc`
- Modify: `src/sau/SConscript`

- [ ] **Step 1: Add public memory-port invariants to the model**

Add assertions exercised by the later integration test:

```cpp
panic_if(memoryPort.hasBlockedPacket() && memoryPort.canIssue(),
         "blocked packet must stop new issue");
panic_if(memoryPort.outstandingReads() > maxOutstandingReads,
         "read outstanding limit exceeded");
panic_if(memoryPort.outstandingWrites() > maxOutstandingWrites,
         "write outstanding limit exceeded");
```

Build now and confirm failure because `SauMemoryPort` does not exist.

- [ ] **Step 2: Implement packet sender state and retry ownership**

Expose:

```cpp
class SauMemoryPort : public RequestPort
{
  public:
    SauMemoryPort(std::string name, SauModel &owner, RequestorID requestorId,
                  unsigned beatBytes);
    bool trySend(const Beat &beat, bool write);
    std::vector<Beat> takeVisibleResponses();
    bool canIssue() const;
    bool hasBlockedPacket() const;
    unsigned outstandingReads() const;
    unsigned outstandingWrites() const;

  protected:
    bool recvTimingResp(PacketPtr packet) override;
    void recvReqRetry() override;
};
```

Create each `Request` with the SAU requestor ID and exact beat size. Allocate
packet data. Zero-fill writes. Increment outstanding only after
`sendTimingReq()` returns true; preserve one rejected packet for retry. On
retry acceptance, notify `SauModel::requestAccepted(beat, write)`. On response,
decrement the matching count, queue read metadata for next-edge visibility,
and delete packet and sender state.

- [ ] **Step 3: Connect response visibility to the clock edge**

`recvTimingResp()` must not push directly into `InputBuffer`. It only queues
metadata and schedules `tickEvent` at `nextCycle()` if the model is otherwise
sleeping. `tick()` calls `takeVisibleResponses()` in its first update step.

- [ ] **Step 4: Build unit tests and gem5**

```bash
scons build/ALL/sau/command.test.opt \
      build/ALL/sau/address_generator.test.opt \
      build/ALL/sau/token_pipeline.test.opt \
      build/ALL/gem5.opt -j4
```

Expected: all targets build.

- [ ] **Step 5: Commit**

```bash
git add src/sau/memory_port.hh src/sau/memory_port.cc \
        src/sau/sau_model.hh src/sau/sau_model.cc src/sau/SConscript
git commit -m "feat: issue SAU timing memory requests"
```

---

### Task 8: Integrate scheduler, array pipeline, writeback, and drain

**Files:**
- Modify: `src/sau/sau_model.hh`
- Modify: `src/sau/sau_model.cc`

- [ ] **Step 1: Update the scheduler member state**

Add these includes to `src/sau/sau_model.hh`:

```cpp
#include <deque>

#include "sau/a_register_file.hh"
#include "sau/address_generator.hh"
#include "sau/token_pipeline.hh"
```

Add these runtime members near `visibleMemoryResponses`:

```cpp
std::optional<AddressGenerator> readGenerator;
std::optional<ARegisterFileIn> aRegisterFile;
std::deque<Beat> availableB;
TokenBuffer outputBuffer;
ArrayPipeline arrayPipeline;

uint32_t currentInstruction = 0;
uint32_t currentFlow = 0;
uint32_t currentArrayBeat = 0;
uint32_t nextArrayIndex = 0;
uint32_t nextResultIndex = 0;
uint32_t nextWriteIndex = 0;

uint64_t acceptedReadBeats = 0;
uint64_t visibleReadBeats = 0;
uint64_t arrayAdmissions = 0;
uint64_t resultsProduced = 0;
uint64_t writesAccepted = 0;
```

Initialize `outputBuffer` and `arrayPipeline` in the constructor initializer
list:

```cpp
outputBuffer(outputBufferEntries),
arrayPipeline(arrayFillCycles, arrayIiCycles, arrayCapacity),
```

In `submitCommand()`, after `activeCommand = command`, initialize:

```cpp
readGenerator.emplace(command);
aRegisterFile.emplace(command);
availableB.clear();
visibleMemoryResponses.clear();
currentInstruction = 0;
currentFlow = 0;
currentArrayBeat = 0;
nextArrayIndex = 0;
nextResultIndex = 0;
nextWriteIndex = 0;
acceptedReadBeats = 0;
visibleReadBeats = 0;
arrayAdmissions = 0;
resultsProduced = 0;
writesAccepted = 0;
```

This explicitly separates external Operand-A preload from Operand-A array
reuse. Do not reintroduce an input buffer that treats every A array input as a
fresh SRAM read.

- [ ] **Step 2: Define the edge-ordered scheduler helpers**

Declare these methods in this order:

```cpp
void consumeResponses();
void advanceArray();
void produceResults();
void issueWrites();
void issueReads();
void updatePhase();
void accountCycle();
```

`tick()` calls exactly that sequence and then either schedules the next edge
or sleeps when idle with no pending response.

- [ ] **Step 3: Implement response consumption**

Move beats from `visibleMemoryResponses` into the correct logical resource:

```cpp
void
SauModel::consumeResponses()
{
    for (const auto &beat : visibleMemoryResponses) {
        ++visibleReadBeats;
        if (beat.stream == StreamKind::OperandA) {
            aRegisterFile->load(beat);
        } else if (beat.stream == StreamKind::OperandB) {
            availableB.push_back(beat);
        } else {
            panic("unexpected SAU read response stream");
        }
    }
    visibleMemoryResponses.clear();
}
```

The old plan said Operand-A responses enter `inputBuffer`; that is no longer
correct. Operand-A responses preload `ARegisterFileIn`. Operand-A array input
beats are later generated virtually from the resident A register file.

- [ ] **Step 4: Implement array admission with A reuse and B streaming**

In `advanceArray()`, admit one work token when all of these are true:

- `aRegisterFile->instructionReady(currentInstruction)`;
- `availableB` is nonempty;
- `arrayPipeline.canAccept(Cycles(sauCycle))`;
- `outputBuffer.canPush()` or there is enough output headroom to avoid
  overfilling before the next result is consumed;
- `nextArrayIndex < activeCommand->workItems`.

When admitting:

```cpp
const auto bBeat = availableB.front();
availableB.pop_front();
const Beat aBeat = aRegisterFile->arrayInputBeat(
    currentInstruction, currentFlow, currentArrayBeat, nextArrayIndex);

if (phase == Phase::OperandLoad) {
    phase = Phase::ArrayActive;
    traceWriter.emit(sauCycle, EventKind::PhaseChanged,
                     activeCommand->id, "none", 0, 0, phase);
}
traceWriter.emit(sauCycle, EventKind::ArrayInputAccepted,
                 activeCommand->id, "operand_b", 0, bBeat.index, phase);
traceWriter.emit(sauCycle, EventKind::ArrayInputAccepted,
                 activeCommand->id, "operand_a", 0, aBeat.index, phase);

arrayPipeline.accept(activeCommand->id, nextArrayIndex,
                     nextArrayIndex + 1 == activeCommand->workItems,
                     Cycles(sauCycle));
```

Then increment `arrayAdmissions`, `nextArrayIndex`, and the
`currentArrayBeat/currentFlow/currentInstruction` cursors. The cursor order is:

```text
beat within flow -> next flow -> next instruction
```

This task deliberately models only the architecture-visible effect: A is
resident and reused; B is streamed. It does not model RTL register-file ports
or stored values.

- [ ] **Step 5: Implement result and writeback accounting**

Each completed work token creates one output token until
`command.output.beats * command.instructionLoops` tokens have been produced.
Emit `result_produced` on insertion into `OutputBuffer`.

Write addresses use `output.base + beatIndex * output.strideBytes`.
Emit `write_accepted` at the `SauMemoryPort` acceptance callback, not at
response time. If `trySend()` returns false, the port owns one blocked packet
for retry; the scheduler must not reissue the same output token from
`outputBuffer`.

- [ ] **Step 6: Issue external reads and writes**

`issueReads()` sends beats from `readGenerator`, not from array demand:

```cpp
unsigned issued = 0;
while (issued < readIssueWidth && readGenerator &&
       !readGenerator->empty() &&
       memoryPort.outstandingReads() < maxOutstandingReads &&
       memoryPort.canIssue()) {
    const Beat beat = readGenerator->front();
    const bool acceptedOrBlocked = memoryPort.trySend(beat, false);
    readGenerator->pop();
    ++issued;
    if (!acceptedOrBlocked) {
        ++stats.stallRequestRetry;
        break;
    }
}
```

`issueWrites()` sends from `outputBuffer` in output beat order. Build the write
beat as:

```cpp
Beat writeBeat{
    StreamKind::Output,
    activeCommand->output.base +
        static_cast<Addr>(nextWriteIndex) *
            activeCommand->output.strideBytes,
    nextWriteIndex,
    nextWriteIndex + 1 == activeCommand->output.beats *
        activeCommand->instructionLoops,
};
```

After `memoryPort.trySend(writeBeat, true)`, pop the output token because the
port either accepted it immediately or retained it as the single blocked packet
for retry. Increment the write-accepted counter only in `requestAccepted()`,
because retry acceptance may happen later.

- [ ] **Step 7: Implement phases and completion**

Use:

- `OperandLoad` from command acceptance through the first array admission;
- `ArrayActive` while new work can still enter;
- `ArrayDrain` after the last work admission until the last result token;
- `Writeback` after the first accepted output write while output tokens,
  blocked write packets, or outstanding writes remain;
- `Complete` after all expected writes are accepted and no command-local
  memory packet state remains.

Emit `phase_changed` exactly once per transition. Emit `command_complete` in
the same SAU cycle as transition to `Complete`. If `exit_on_done`, call
`exitSimLoop("SAU command complete")`.

When a phase transition and its anchor event occur on the same edge, emit
`phase_changed` first and then the anchor event using the new phase. Apply the
same ordering in the RTL monitor.

- [ ] **Step 8: Implement drain behavior**

Return `DrainState::Drained` only when idle or complete with no blocked packet
and zero outstanding requests. Otherwise return `Draining`; call
`signalDrainDone()` after the final response releases all packet state.
Do not serialize an active command in this milestone.

- [ ] **Step 9: Add token and request conservation assertions**

Maintain cumulative counters and assert on every tick:

```cpp
assert(readResponses <= acceptedReads);
assert(aRegisterFile->totalExternalLoadBeats() <= acceptedReads);
assert(arrayAdmissions <= aRegisterFile->totalArrayInputBeats());
assert(resultsProduced <= arrayAdmissions);
assert(writesAccepted <= resultsProduced);
assert(availableB.size() <= visibleReadBeats);
assert(outputBuffer.size() + writesAccepted <= resultsProduced);
```

At `CommandComplete`, require all command-local expected counts to match and
require both token buffers and the array pipeline to be empty.

- [ ] **Step 10: Add a focused `SauModel` scheduler unit test if feasible**

If a small standalone `SauModel` unit test can be written without duplicating a
gem5 Python config, add a test that exercises:

- A read responses loading `ARegisterFileIn`;
- B read responses entering `availableB`;
- one `array_input_accepted` pair emitted with B before A;
- one result token and one write accepted.

If this is too intrusive for this task, document the reason in `STATUS.md` and
cover end-to-end execution in Task 9's standalone simulation instead.

- [ ] **Step 11: Build all SAU unit tests**

```bash
scons build/RISCV/sau/address_generator.test.opt \
      build/RISCV/sau/a_register_file.test.opt \
      build/RISCV/sau/command.test.opt \
      build/RISCV/sau/memory_port.test.opt \
      build/RISCV/sau/token_pipeline.test.opt \
      build/RISCV/sau/trace_writer.test.opt \
      build/RISCV/gem5.opt -j4

./build/RISCV/sau/address_generator.test.opt
./build/RISCV/sau/a_register_file.test.opt
./build/RISCV/sau/command.test.opt
./build/RISCV/sau/memory_port.test.opt
./build/RISCV/sau/token_pipeline.test.opt
./build/RISCV/sau/trace_writer.test.opt
python3 -m unittest util.sau.compare_trace_test -v
```

Expected: all tests PASS.

- [ ] **Step 12: Commit**

```bash
git add src/sau/sau_model.hh src/sau/sau_model.cc
git commit -m "feat: run SAU GEMM timing pipeline"
```

---

### Task 8.5: Calibrate and model the matmul `TRANSPOSE_LOAD/CLIP` timing path

Task 9 must not start until Task 8.5 has either implemented the calibrated
matmul transpose/reuse timing policy or explicitly documented why the
standalone simulation is expected to differ from the corrected RTL baseline.

**Files:**
- Modify: `src/sau/Sau.py`
- Modify: `src/sau/sau_model.hh`
- Modify: `src/sau/sau_model.cc`
- Modify: `src/sau/STATUS.md`
- Test: existing SAU C++ tests and Task 9 standalone trace comparison.

- [ ] **Step 1: Record the corrected baseline state spans**

Use the corrected diagnostic trace:

```bash
python3 - <<'PY'
import csv, collections
p = "tests/gem5/sau/ref/int8_gemm_64x256x256/diagnostic.csv"
counts = collections.Counter()
with open(p) as f:
    for row in csv.DictReader(f):
        cycle = int(row["cycle"])
        if cycle <= 5897:
            counts[row["core_state"]] += 1
for state, count in counts.most_common():
    print(state, count)
PY
```

Expected per command in the corrected package:

```text
REGISTER_LOAD    256 cycles
TRANSPOSE_LOAD    32 cycles
REUSE_LOAD      1854 cycles
TRANSPOSE_CLIP   231 cycles
D_OUT             99 cycles
FIRST_LOAD        33 cycles
REGISTER_UNLOAD  265 cycles
```

These states are not optional for this matmul baseline. In particular,
`TRANSPOSE_LOAD` appears once per command and `TRANSPOSE_CLIP` appears once per
flow-loop boundary before the final `FIRST_LOAD/D_OUT` tail.

- [ ] **Step 2: Replace the same-cycle A/B array-input assumption**

The corrected public trace shows staggered array-input windows:

```text
command 1: A array_input cycles 269..2392, B array_input cycles 301..2425
command 2: A array_input cycles 3394..5517, B array_input cycles 3426..5550
```

Do not require every A and B array input to be admitted on the same SAU cycle.
Model the 32-cycle skew introduced by the transpose/reuse path at cycle-level
granularity. Preserve event order within a cycle when both streams are present.

- [ ] **Step 3: Add explicit timing parameters**

Add parameters with defaults matching the corrected baseline:

```python
register_load_cycles = Param.Cycles(256, "A register_file_in preload span")
transpose_load_cycles = Param.Cycles(32, "matmul transpose-load span")
transpose_clip_cycles = Param.Cycles(33, "per-flow transpose-clip span")
final_first_load_cycles = Param.Cycles(33, "final first-load tail span")
register_unload_cycles = Param.Cycles(265, "writeback/unload completion span")
```

Keep these as timing-policy knobs, not RTL register-accurate state replicas.

- [ ] **Step 4: Update `SauModel` scheduling**

The scheduler must be able to reproduce the corrected trace shape:

- external A reads complete before the first B stream;
- A array input begins after `REGISTER_LOAD + TRANSPOSE_LOAD` timing;
- B array input begins 32 cycles after A array input;
- each flow boundary includes a `TRANSPOSE_CLIP`-like gap;
- the final tail uses `FIRST_LOAD/D_OUT` timing before register unload/writeback
  completion.

Preserve token/request conservation assertions.

- [ ] **Step 5: Verify against the corrected baseline shape**

After Task 9 can generate a gem5 trace, compare against:

```bash
python3 util/sau/compare_trace.py --mode causal \
    tests/gem5/sau/ref/int8_gemm_64x256x256/architecture.csv \
    m5out/sau-fixed/sau.csv
```

Expected before full calibration: event order and stream/beat metadata should
match, while strict cycles may still differ. Do not claim cycle accuracy until
Task 10.

- [ ] **Step 6: Commit**

```bash
git add src/sau docs/superpowers/plans src/sau/STATUS.md
git commit -m "feat: model SAU matmul transpose timing path"
```

---

### Task 9: Add standalone fixed- and constrained-memory simulations

**Files:**
- Create: `configs/example/sau_timing.py`
- Create: `tests/gem5/sau/test_sau.py`

- [ ] **Step 1: Create the standalone config**

Build a timing-only `System` containing:

```python
system = System(
    clk_domain=SrcClockDomain(
        clock=args.sau_clock, voltage_domain=VoltageDomain()
    ),
    mem_mode="timing",
    mem_ranges=[AddrRange("64MiB")],
    membus=SystemXBar(width=32),
)
system.memory = SimpleMemory(
    range=system.mem_ranges[0],
    latency=args.memory_latency,
    latency_var=args.memory_latency_var,
    bandwidth=args.memory_bandwidth,
)
system.sau = SauModel(
    trace_file=args.trace,
    a_base=0x1000,
    b_base=0x2000,
    output_base=0x3000,
    a_beats=args.a_beats,
    b_beats=args.b_beats,
    output_beats=args.output_beats,
    flow_loops=args.flow_loops,
    array_fill_cycles=args.array_fill_cycles,
)
system.sau.memory = system.membus.cpu_side_ports
system.memory.port = system.membus.mem_side_ports
system.system_port = system.membus.cpu_side_ports
```

Add CLI arguments for every value shown above and simulate until the SAU exit
event. Also expose `--input-buffer-entries`, `--output-buffer-entries`,
`--array-capacity`, `--max-outstanding-reads`, and
`--max-outstanding-writes`. Reject any exit cause other than
`SAU command complete`.

- [ ] **Step 2: Run the fixed-memory simulation**

```bash
./build/ALL/gem5.opt \
    --outdir=m5out/sau-fixed \
    configs/example/sau_timing.py \
    --memory-latency=3ns \
    --memory-latency-var=0ns \
    --memory-bandwidth=256GiB/s \
    --trace=m5out/sau-fixed/sau.csv
```

Expected: exit cause is `SAU command complete`, the trace contains read,
array, result, write, and complete events, and stats report one command.

- [ ] **Step 3: Run a constrained-memory simulation**

```bash
./build/ALL/gem5.opt \
    --outdir=m5out/sau-constrained \
    configs/example/sau_timing.py \
    --memory-latency=20ns \
    --memory-latency-var=5ns \
    --memory-bandwidth=1GiB/s \
    --trace=m5out/sau-constrained/sau.csv
```

Expected: command completes; total cycles exceed the fixed run; at least one
memory/backpressure stall statistic is nonzero.

- [ ] **Step 4: Register quick system tests**

Use `gem5_verify_config` twice: fixed and constrained. Verify stdout contains
`SAU command complete`; verify both return code 0. Keep these tests tagged
`constants.quick_tag` and `constants.all_compiled_tag`.

- [ ] **Step 5: Run the focused system suite**

```bash
cd tests
./main.py run --skip-build gem5/sau
```

Expected: both SAU suites PASS.

- [ ] **Step 6: Commit**

```bash
git add configs/example/sau_timing.py tests/gem5/sau/test_sau.py
git commit -m "test: run SAU timing model with memory backpressure"
```

---

### Task 10: Calibrate gem5 against the RTL reference

**Files:**
- Modify: `configs/example/sau_timing.py`
- Modify: `src/sau/Sau.py`
- Modify: `src/sau/sau_model.cc`
- Modify: `docs/superpowers/specs/2026-07-02-sau-cycle-level-model-design.md`

- [ ] **Step 1: Generate an uncalibrated gem5 trace**

Run the fixed-memory config with the same clock period, memory latency, beat
count, flow count, and instruction count recorded in the RTL CSV:

```bash
./build/ALL/gem5.opt \
    --outdir=m5out/sau-rtl-match \
    configs/example/sau_timing.py \
    --rtl-profile \
    --trace=m5out/sau-rtl-match/sau.csv
```

- [ ] **Step 2: Run strict comparison and preserve the first mismatch**

```bash
python3 util/sau/compare_trace.py --mode strict \
    tests/gem5/sau/ref/int8_gemm.csv \
    m5out/sau-rtl-match/sau.csv
```

Expected before calibration: FAIL with the first differing event and cycle.

- [ ] **Step 3: Calibrate named timing parameters only**

Measure and set:

- `command_start_cycles`;
- memory response-to-token visibility offset;
- feeder delay before `ArrayInputAccepted`;
- `array_fill_cycles`;
- `array_ii_cycles`;
- final array drain offset;
- result-to-write eligibility offset;
- completion offset after last accepted write.

Every adjustment must map to one RTL event delta. Do not add event-specific
conditionals or command-ID special cases. Record the measured values and RTL
signals in the design document's timing-profile section.

- [ ] **Step 4: Require exact trace equality**

Repeat the strict comparator until it exits 0.

Expected: same row count and exact equality of cycle, event, stream, address,
beat, and abstract phase.

- [ ] **Step 5: Re-run constrained memory in causal mode**

```bash
python3 util/sau/compare_trace.py --mode causal \
    tests/gem5/sau/ref/int8_gemm.csv \
    m5out/sau-constrained/sau.csv
```

Expected: PASS despite longer cycles.

- [ ] **Step 6: Commit calibration**

```bash
git add src/sau/Sau.py src/sau/sau_model.cc \
        configs/example/sau_timing.py \
        docs/superpowers/specs/2026-07-02-sau-cycle-level-model-design.md
git commit -m "test: calibrate SAU GEMM timing to RTL"
```

---

### Task 11: Final regression, statistics audit, and documentation

**Files:**
- Modify: `src/sau/sau_model.hh`
- Modify: `src/sau/sau_model.cc`
- Modify: `configs/example/sau_timing.py`
- Modify: `tests/gem5/sau/test_sau.py`
- Create: `src/sau/README.md`

- [ ] **Step 1: Audit statistics against the design contract**

Ensure the stats output contains:

```text
commandsAccepted
commandsCompleted
commandCycles
operandLoadCycles
arrayActiveCycles
arrayDrainCycles
writebackCycles
readRequests
readBytes
writeRequests
writeBytes
maxOutstandingReads
maxOutstandingWrites
averageOutstandingReads
averageOutstandingWrites
averageInputBufferOccupancy
averageOutputBufferOccupancy
maxInputBufferOccupancy
maxOutputBufferOccupancy
arrayInputStallCycles
arrayOutputStallCycles
readRetryStallCycles
writeRetryStallCycles
readLimitStallCycles
writeLimitStallCycles
firstReadOffset
firstArrayInputOffset
firstResultOffset
lastResultOffset
lastWriteOffset
completeOffset
```

Add a focused system-test verifier that checks all names exist, command counts
are one, byte totals equal beat totals times 32, and constrained command
latency exceeds fixed command latency.

- [ ] **Step 2: Add DSE monotonicity checks**

Run two otherwise identical fixed-memory configurations with
`--array-capacity=1` and `--array-capacity=16`, then two with
`--output-buffer-entries=1` and `--output-buffer-entries=8`. Assert that adding
capacity never increases `commandCycles`, and that any slower run reports a
nonzero matching capacity stall. Add these as quick system-test variants.

- [ ] **Step 3: Document usage and limitations**

`src/sau/README.md` must include:

- build command;
- fixed and constrained example commands;
- strict RTL comparison command;
- parameter table;
- statistics table;
- explicit warning that writes contain zero data;
- first-milestone exclusions;
- links to the design spec and this plan.

- [ ] **Step 4: Run all focused verification**

```bash
scons build/ALL/sau/command.test.opt \
      build/ALL/sau/address_generator.test.opt \
      build/ALL/sau/token_pipeline.test.opt \
      build/ALL/gem5.opt -j4
./build/ALL/sau/command.test.opt
./build/ALL/sau/address_generator.test.opt
./build/ALL/sau/token_pipeline.test.opt
python3 -m unittest util.sau.compare_trace_test -v
cd tests
./main.py run --skip-build gem5/sau
```

Expected: every focused unit and system test PASS.

- [ ] **Step 5: Run format and whitespace checks**

```bash
git diff --check
pre-commit run --files \
    src/sau/SConscript src/sau/Sau.py src/sau/types.hh \
    src/sau/command.hh src/sau/command.cc src/sau/command.test.cc \
    src/sau/address_generator.hh src/sau/address_generator.cc \
    src/sau/address_generator.test.cc src/sau/token_pipeline.hh \
    src/sau/token_pipeline.cc src/sau/token_pipeline.test.cc \
    src/sau/memory_port.hh src/sau/memory_port.cc \
    src/sau/trace_writer.hh src/sau/trace_writer.cc \
    src/sau/sau_model.hh src/sau/sau_model.cc \
    configs/example/sau_timing.py tests/gem5/sau/test_sau.py \
    util/sau/compare_trace.py util/sau/compare_trace_test.py \
    src/sau/README.md
```

Expected: no whitespace errors and all configured hooks PASS. If
`pre-commit` is unavailable, report that limitation and retain the successful
compiler/test evidence.

- [ ] **Step 6: Commit the completed first milestone**

```bash
git add src/sau configs/example/sau_timing.py tests/gem5/sau \
        util/sau docs/superpowers/specs/2026-07-02-sau-cycle-level-model-design.md
git commit -m "docs: complete SAU timing model milestone"
```

## Follow-on plans

Create separate design/implementation plans, in this order, after the first
milestone passes:

1. CSR-decoded `register_file_in`/reuse control, transpose, and int16 timing
   policies;
2. pointwise, standard, and depthwise convolution plus padding;
3. RISC-V `msetins1..7`, CSR, and completion-interrupt integration;
4. optional arithmetic functional backend.

Each follow-on plan must add new RTL reference traces and retain the int8 GEMM
trace as a regression.
