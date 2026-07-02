# SAU Cycle-Level Behavioral Model Design

## 1. Purpose

Build a cycle-level behavioral model of the Systolic Array Unit (SAU) in
gem5 for architecture research and design-space exploration.

The model must:

- preserve cycle-level timing behavior at architecture-relevant boundaries;
- generate real gem5 timing memory traffic, including retry and backpressure;
- expose configurable array, buffer, pipeline, and memory-interface parameters;
- remain substantially simpler and faster than the SystemVerilog RTL; and
- use RTL simulation as the timing-calibration authority.

The model is not an RTL translation and does not model register-level state.
The first implementation is also not a functional arithmetic model: it does
not calculate matrix values or promise correct output data.

## 2. Source-of-Truth Hierarchy

Timing and behavior are resolved in this order:

1. Event traces from the current RTL under a controlled testbench.
2. Current RTL source under `npu_lpnpu/hardware/src/sa_element/` and
   `npu_lpnpu/hardware/src/sa_execute/`.
3. Current SAU software command construction and regression cases.
4. `npu_lpnpu/docs/SAU_SPEC.md` as explanatory material only.

This ordering is necessary because the documentation currently describes a
five-state scheduler and four instruction-register pairs, while the RTL and
current software contain an eight-state scheduler and seven register pairs.
Array dimensions must likewise be configurable rather than inferred from an
outdated document default.

## 3. Confirmed Scope

### 3.1 First usable milestone

The first usable milestone supports:

- direct injection of decoded SAU commands from a test harness;
- int8 GEMM;
- 128-bit timing reads and writes through the gem5 memory system;
- configurable request concurrency, buffers, and systolic-array timing;
- fixed-latency RTL differential calibration;
- variable-latency and retry/backpressure tests; and
- performance and stall statistics.

### 3.2 Explicitly excluded from the first milestone

- RISC-V `msetins1..7` instruction decoding;
- CSR and completion-interrupt integration;
- arithmetic result generation;
- reuse, transpose, and int16 modes;
- pointwise, standard, and depthwise convolution;
- padding behavior; and
- RTL register-by-register equivalence.

These exclusions are staged extensions, not architectural dead ends.

## 4. Chosen Modeling Approach

Use an event-driven token pipeline implemented as a gem5 `ClockedObject`.

This sits between two rejected extremes:

- An analytical total-latency formula is fast but cannot faithfully represent
  individual memory beats, pipeline overlap, retry, or buffer backpressure.
- An RTL-style cycle simulator can represent those effects but duplicates too
  much RTL state and becomes slow and expensive to maintain.

The token model advances only architecture-relevant work items. It preserves
cycle boundaries, resource occupancy, flow dependencies, and memory traffic
without storing PE register contents or performing arithmetic.

## 5. Architecture

The model is divided into focused units:

- **`SauCommand`**: immutable, decoded low-level command descriptor.
- **`SauScheduler`**: command lifetime, abstract phases, and dependencies.
- **`AddressGenerator`**: ordered 128-bit read/write beat generation.
- **`SauMemoryPort`**: timing requests, responses, retry, and outstanding
  limits.
- **`InputBuffer`**: capacity and metadata for returned operand tokens.
- **`ArrayPipeline`**: systolic fill, initiation interval, throughput, and
  drain timing.
- **`OutputBuffer`**: result-token capacity and writeback eligibility.
- **`SauStats`**: latency, traffic, utilization, occupancy, and stall causes.

The main flow is:

```text
SauCommand
    |
    v
SauScheduler --> AddressGenerator --> SauMemoryPort (reads)
                                         |
                                         v
                                  returned input tokens
                                         |
                                         v
InputBuffer --> ArrayPipeline --> OutputBuffer --> SauMemoryPort (writes)
                                                           |
                                                           v
                                                        complete
```

The components are C++ units inside one SimObject, not independently connected
SimObjects. This keeps per-cycle coordination explicit and avoids unnecessary
event and port overhead.

## 6. Command Boundary

`SauCommand` stores the decoded hardware-level operation rather than only
high-level `M`, `N`, and `K`. It contains:

- operand, output, and optional bias addresses;
- burst, stride, flow-loop, and instruction-loop parameters for each stream;
- precision, reuse, transpose, and flow modes;
- command identity and completion metadata.

The command becomes immutable when accepted.

For the first milestone, a test harness constructs and injects `SauCommand`
directly. A later ISA/CSR frontend will decode the seven 64-bit instruction
register pairs into the same structure. No ISA-specific state may leak into
the scheduler, address generator, or pipeline.

Commands are validated at admission. Zero or out-of-range dimensions,
misaligned addresses, unsupported modes, and impossible queue requirements
produce explicit errors. Memory-port retry is a normal stall condition rather
than an error.

## 7. Timing Semantics

### 7.1 Abstract phases

The public phase model is:

```text
Idle -> OperandLoad -> ArrayActive -> ArrayDrain -> Writeback -> Complete
```

These phases may overlap internally where the RTL overlaps loading, execution,
and writeback. RTL states such as `REGISTER_LOAD`, `FIRST_LOAD`, `REUSE_LOAD`,
`TRANSPOSE_LOAD`, and `TRANSPOSE_CLIP` become mode-specific policies, not a
public gem5 state-machine copy.

### 7.2 Clock-edge ordering

At each SAU clock edge, the model performs this ordered update:

1. Make queued memory responses visible.
2. Update outstanding counts and buffer occupancy.
3. Advance existing array tokens.
4. Generate eligible output tokens.
5. Attempt one or more eligible write requests within configured bandwidth.
6. Attempt one or more eligible read requests within configured bandwidth.
7. Update command phase and statistics.

A response arriving between SAU clock edges is queued and becomes visible at
the next edge. This rule removes host event-order ambiguity and makes
single-cycle offsets testable.

### 7.3 Read and write completion

Read responses create input tokens and therefore directly gate computation.

A write beat is considered committed by the modeled RTL interface when its
timing request is accepted downstream. The later gem5 write response releases
packet and outstanding-request resources. Command completion follows the
calibrated RTL write-commit rule; the memory port can still retain bookkeeping
for accepted writes awaiting responses.

### 7.4 Timing parameters

Calibrated delays are named parameters grouped into one timing profile. This
includes command-start delay, feeder delay, array fill and drain, initiation
interval, result latency, and writeback offsets. Calibration constants must
not be scattered through control logic.

Array dimensions, beat width, buffer depths, request issue width, outstanding
limits, and pipeline characteristics remain configurable DSE parameters.

## 8. Memory and Data Semantics

The model sends actual `ReadReq` and `WriteReq` packets through a custom
`RequestPort`. A custom port is required because the model must control each
128-bit beat's issue cycle, retry behavior, and outstanding count; bulk
`DmaPort` actions do not expose the required per-beat scheduling contract.

The timing model does not consume returned operand values. Input packet data is
discarded after the response creates the corresponding metadata token.

Write requests carry deterministic zero-filled payloads and therefore modify
the destination range. Timing-only tests must use isolated output memory and
must not inspect output values. A future optional functional backend may
provide correct output bytes, but arithmetic must remain outside the timing
pipeline so timing and functionality can be tested independently.

## 9. Backpressure and Resource Rules

Progress is resource-driven:

- the address generator stalls when the memory port cannot accept another
  request;
- read issue stalls at the configured outstanding-read limit;
- returned operands wait when downstream pairing or array admission is not
  ready;
- the array stalls when the output buffer has no capacity;
- write issue stalls on downstream rejection or the outstanding-write limit;
- command completion cannot discard pending token or packet state.

Every non-idle cycle is attributed to useful work or a named stall cause.
Occupancy and conservation assertions ensure that accepted beats and tokens
are neither lost nor duplicated.

## 10. RTL Calibration and Verification

### 10.1 Common trace format

The RTL testbench and gem5 model emit the same CSV event schema. Required
events include:

- command accepted;
- read request accepted, with cycle, stream, address, and beat index;
- read response visible;
- first array input accepted;
- first and last result token produced;
- write request accepted, with cycle, address, and beat index;
- abstract phase transition; and
- command complete.

The comparison deliberately excludes RTL register values and other
implementation-only signals.

### 10.2 Verification layers

1. **C++ unit tests**
   - command validation;
   - address sequences;
   - token conservation;
   - buffer limits;
   - array fill, initiation interval, and drain.
2. **Memory-port component tests**
   - fixed response latency;
   - variable response latency;
   - request rejection and retry;
   - read/write outstanding limits.
3. **RTL differential tests**
   - identical int8 GEMM descriptors;
   - exact event-cycle and address comparison under fixed-latency SRAM.
4. **DSE property tests**
   - increased bandwidth or resources do not create unexplained throughput
     regressions;
   - reduced buffers create attributable stalls;
   - all extra latency is explained by phase or stall statistics.

### 10.3 Accuracy criteria

With deterministic, contention-free memory, architecture-relevant events must
match RTL exactly by cycle.

With cache or memory contention, absolute cycles need not match the standalone
RTL testbench. Requests must remain legal, event causality and token
conservation must hold, and extra latency must be explained by real responses,
retry, resource occupancy, or backpressure.

## 11. Statistics

At minimum, expose:

- commands accepted and completed;
- total and per-phase command cycles;
- read/write requests and bytes;
- average and maximum outstanding reads/writes;
- input/output buffer occupancy;
- array active cycles and utilization;
- stall cycles by request retry, outstanding limit, input starvation, output
  buffer full, and writeback blockage;
- first-read, first-input, first-result, last-result, last-write, and complete
  offsets relative to command acceptance.

These statistics form part of the model contract because they explain DSE
results and timing mismatches.

## 12. Progressive Delivery

1. **RTL baseline**
   - produce a minimal int8 GEMM case and reference CSV trace.
2. **gem5 skeleton**
   - SimObject, command type, parameters, statistics, and test structure.
3. **Memory frontend**
   - per-beat address generation, timing port, retry, outstanding limits, and
     buffers.
4. **Array token model**
   - fill, initiation interval, drain, overlap, output buffering, and
     writeback.
5. **GEMM calibration**
   - exact fixed-memory trace agreement and variable-memory backpressure
     validation.
6. **Mode expansion**
   - reuse, transpose, and int16.
7. **Convolution expansion**
   - pointwise, standard, depthwise, and padding behavior.
8. **System integration**
   - custom RISC-V instruction/CSR frontend, completion interrupt, and
     optional functional backend.

Each stage must be independently testable and must preserve all earlier
calibration cases.

## 13. First-Milestone Definition of Done

The first milestone is complete only when:

- int8 GEMM commands can be injected without ISA support;
- all operand and output beats traverse the gem5 timing memory system;
- retry, outstanding limits, and buffer backpressure affect progress;
- fixed-latency SRAM runs match the RTL reference trace exactly by cycle;
- variable-latency tests preserve causality and token conservation;
- performance and stall statistics explain the measured command latency;
- unsupported modes fail explicitly; and
- no arithmetic correctness is claimed or tested.

