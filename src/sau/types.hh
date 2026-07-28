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

/** Raw 2-bit trans_mode values named after SA_pkg.sv. */
enum class SauTransMode : uint8_t
{
    ABD = 0,   // Neither operand is transposed
    ATBD = 1,  // Transpose operand A
    ABTD = 2,  // Transpose operand B
    ABDT = 3   // Raw RTL name; no output-order ownership
};

/** Raw 2-bit reuse_mode values; 11 asserts both reuse bits (Step 0). */
enum class SauReuseMode : uint8_t
{
    None = 0,
    ReuseA = 1,
    ReuseB = 2,
    ReuseAB = 3
};

/** Raw 2-bit output flow-mode values named after the RTL modes. */
enum class SauSaFlowMode : uint8_t
{
    CNormal = 0,
    CTrans = 1,
    Retain = 2,
    TRetain = 3
};

/** Raw 2-bit pe_work_mode operator selection. Only Matmul is in stage. */
enum class SauPeWorkMode : uint8_t
{
    Matmul = 0,
    Conv = 1,
    Transposer = 2,
    Add = 3
};

/**
 * Highest validation level a decoded configuration has reached.  RTL
 * legality and model maturity are deliberately separate: a legal raw
 * configuration whose resource path is not executable yet must be
 * reported as RtlLegalUnimplemented, never mislabeled illegal.
 */
enum class ValidationMaturity : uint8_t
{
    Decoded,
    RtlLegalUnimplemented,
    ResourceTimed,
    DataFunctional,
    EndToEndValidated
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

/** Raw csr.sv streamed-input counter fields (register index 2). */
struct SauInputCsrConfig
{
    uint8_t xStep = 0;
    uint8_t xBurst = 0;
    uint8_t yStep = 0;
    uint8_t yBurst = 0;
    uint8_t flowStep = 0;
    uint8_t flowBurst = 0;
    uint8_t instructionStep = 0;
    uint8_t instructionBurst = 0;
};

/** Raw csr.sv vertical address counter fields (register index 4). */
struct SauVerticalCsrConfig
{
    uint8_t xStep = 0;
    uint8_t xBurst = 0;
    uint8_t yStep = 0;
    uint8_t yCycle = 0;
    uint8_t flowStep = 0;
    uint8_t flowCycle = 0;
    uint8_t instructionStep = 0;
    uint8_t instructionCycle = 0;
};

/** Raw csr.sv register-input counter and valid-window fields (index 1). */
struct SauRegisterInputCsrConfig
{
    uint8_t xBurst = 0;
    uint8_t yStep = 0;
    uint8_t yCycle = 0;
    uint8_t cStep = 0;
    uint8_t cCycle = 0;
    uint8_t validYStart = 0;
    uint8_t validYEnd = 0;
    uint8_t validXStart = 0;
    uint8_t validXEnd = 0;
    uint8_t padding = 0;
};

/** Raw csr.sv output counter fields (register indexes 5 and 6). */
struct SauOutputCsrConfig
{
    uint8_t xStep = 0;
    uint8_t xBurst = 0;
    uint8_t yStep = 0;
    uint8_t yBurst = 0;
    uint8_t flowStep = 0;
    uint8_t flowBurst = 0;
    uint8_t instructionStep = 0;
    uint8_t instructionBurst = 0;
    uint8_t registerXBurst = 0;
    uint8_t registerYStep = 0;
    uint8_t registerYCycle = 0;
    uint8_t registerCStep = 0;
    uint8_t registerCCycle = 0;
};

/**
 * The complete raw int8-GEMM control state a command was started with.
 * Every field keeps its csr.sv raw width and value; typed accessors only
 * name the raw encodings.  Resources must consume these fields instead of
 * re-deriving behavior from flattened beat counts or fixture assumptions.
 */
struct SauControlFields
{
    uint8_t transMode = 0;
    uint8_t reuseMode = 0;
    uint8_t saFlowMode = 0;
    uint8_t registerMode = 0;
    uint8_t peWorkMode = 0;
    uint8_t convKernal = 0;
    bool strideFlag = false;
    bool shiftFlag = false;
    uint8_t cutbit = 0;
    uint8_t flowLoopTimes = 0;
    Addr verticalAddress = 0;
    Addr horizontalAddress = 0;
    Addr outputAddress = 0;
    Addr biasAddress = 0;
    SauInputCsrConfig input;
    SauVerticalCsrConfig vertical;
    SauRegisterInputCsrConfig registerInput;
    SauOutputCsrConfig output;

    SauTransMode trans() const
    {
        return static_cast<SauTransMode>(transMode & 0x3);
    }

    SauReuseMode reuse() const
    {
        return static_cast<SauReuseMode>(reuseMode & 0x3);
    }

    SauSaFlowMode saFlow() const
    {
        return static_cast<SauSaFlowMode>(saFlowMode & 0x3);
    }

    SauPeWorkMode peWork() const
    {
        return static_cast<SauPeWorkMode>(peWorkMode & 0x3);
    }
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
    // Raw CSR control state for CSR-replayed commands. Synthetic
    // direct-command runs leave it at the neutral default.
    SauControlFields control;
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
