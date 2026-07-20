#ifndef __SAU_TYPES_HH__
#define __SAU_TYPES_HH__

#include <cstdint>

#include "base/types.hh"

namespace gem5::sau
{

enum class Operation : uint8_t
{
    Gemm
};

enum class Precision : uint8_t
{
    Int8
};

enum class StreamKind : uint8_t
{
    OperandA,
    OperandB,
    Output
};

enum class Phase : uint8_t
{
    Idle,
    OperandLoad,
    ArrayActive,
    ArrayDrain,
    Writeback,
    Complete
};

enum class EventKind : uint8_t
{
    CommandAccepted,
    ReadAccepted,
    ReadResponseVisible,
    ArrayInputAccepted,
    ResultProduced,
    WriteAccepted,
    PhaseChanged,
    CommandComplete
};

struct StreamDesc
{
    Addr base = 0;
    uint32_t beats = 0;
    uint32_t strideBytes = 32;
    uint32_t flowStrideBytes = 0;
    uint32_t instructionStrideBytes = 0;
};

/**
 * Optional nested address program for the streamed Operand-B reads.
 *
 * mem_addr.sv advances x, y, flow, then instruction counters.  Synthetic
 * commands keep this disabled and continue to use StreamDesc's linear
 * address formula.
 */
struct NestedAddressProgram
{
    bool enabled = false;
    uint32_t xCount = 1;
    uint32_t yCount = 1;
    uint32_t flowCount = 1;
    uint32_t instructionCount = 1;
    uint32_t xStepBytes = 0;
    uint32_t yStepBytes = 0;
    uint32_t flowStepBytes = 0;
    uint32_t instructionStepBytes = 0;
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
    // Effective array work tokens are driven by the streaming Operand-B path.
    uint32_t workItems = 0;
    // scheduler.sv ins_times_i is driven by vertical.ins_cycle independently
    // of flow_times_i, which is driven by flowLoops. Zero preserves the
    // legacy direct-command interpretation (flowLoops * instructionLoops).
    uint32_t scheduleInstructions = 0;
    NestedAddressProgram operandBAddress;
};

struct Beat
{
    StreamKind stream;
    Addr address;
    uint32_t index;
    bool last;

    bool
    operator==(const Beat &other) const
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

#endif // __SAU_TYPES_HH__
