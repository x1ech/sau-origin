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
