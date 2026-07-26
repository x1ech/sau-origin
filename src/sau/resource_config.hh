#ifndef __SAU_RESOURCE_CONFIG_HH__
#define __SAU_RESOURCE_CONFIG_HH__

#include <cstdint>

#include "sau/types.hh"

namespace gem5::sau
{

/**
 * Typed per-resource configuration dispatched from one raw
 * SauControlFields block.  Each struct carries exactly the CSR fields the
 * RTL wires to that resource; modules must consume their struct instead
 * of re-interpreting raw CSR state.  The field ownership follows the
 * frozen PLAN3_STEP0 resource abstraction contract.
 */

/** scheduler.sv: command control, input switch and completion counting. */
struct SauControllerResourceConfig
{
    SauTransMode transMode = SauTransMode::ABD;
    SauReuseMode reuseMode = SauReuseMode::None;
    SauSaFlowMode saFlowMode = SauSaFlowMode::CNormal;
    // scheduler.flow_times_i is driven by flow_loop_times and
    // scheduler.ins_times_i by vertical.ins_cycle (PLAN2 Step 5).
    uint8_t flowTimes = 0;
    uint8_t instructionTimes = 0;
};

/** mem_addr.sv: streamed-operand external read address program. */
struct SauStreamAddressResourceConfig
{
    Addr baseAddress = 0;
    SauVerticalCsrConfig counters;
    // mem_addr.sv consumes conv_kernal directly: conv==0 selects the
    // fixed one-beat instruction step.
    uint8_t convKernal = 0;
};

/** register_addr.sv: resident-operand external read address program. */
struct SauResidentAddressResourceConfig
{
    Addr baseAddress = 0;
    uint8_t xBurst = 0;
    uint8_t yStep = 0;
    uint8_t yCycle = 0;
    uint8_t cStep = 0;
    uint8_t cCycle = 0;
    // register_addr.sv consumes the padding/valid window itself: padded
    // positions freeze the address while the write pointer advances.
    uint8_t padding = 0;
    uint8_t validYStart = 0;
    uint8_t validYEnd = 0;
    uint8_t validXStart = 0;
    uint8_t validXEnd = 0;
};

/** register_file_in / padding_shifter / feeder input-side state. */
struct SauInputResourceConfig
{
    // Raw register_mode; 00/01/11 share the RTL non-depthwise guard.
    uint8_t registerMode = 0;
    uint8_t padding = 0;
    uint8_t validYStart = 0;
    uint8_t validYEnd = 0;
    uint8_t validXStart = 0;
    uint8_t validXEnd = 0;
    SauInputCsrConfig streamedCounters;
};

/** transposer_tiny banks plus the scheduler reuse selection. */
struct SauTransposeReuseResourceConfig
{
    SauTransMode transMode = SauTransMode::ABD;
    // trans_mode selects which operand loads the T0/T1 banks and whether
    // the result passes the transpose path (PLAN3_STEP0 path table).
    bool loadOperandA = false;
    bool loadOperandB = false;
    bool transposedResult = false;
    // reuse_mode bit0 is A_reuse_flag, bit1 is B_reuse_flag; 11 asserts
    // both bits per the Step 0 probe.
    bool reuseA = false;
    bool reuseB = false;
    // sa_flow_mode[1] retains bank contents/ownership across commands.
    bool retainBanks = false;
};

/** SA_ENGINE/SA_ROW/SA_PE fixed-point compute resource. */
struct SauArrayResourceConfig
{
    // SA_ENGINE.keep_mode = sa_flow_mode[1] retains accumulator state.
    bool keepMode = false;
    // Raw 5-bit arithmetic shift for SA_pkg::sat_truncate_func.
    uint8_t cutbit = 0;
};

/** register_file_out serializer, accumulation and unload ordering. */
struct SauOutputResourceConfig
{
    // sa_flow_mode[1]: add against the existing 16-bit value instead of
    // zero; sa_flow_mode[0]: transposed result/unload ordering.
    bool accumulateExisting = false;
    bool transposedOrder = false;
    uint8_t registerXBurst = 0;
    uint8_t registerYStep = 0;
    uint8_t registerYCycle = 0;
    uint8_t registerCStep = 0;
    uint8_t registerCCycle = 0;
};

/** Output writeback external address program (mem-side counters). */
struct SauWritebackResourceConfig
{
    Addr baseAddress = 0;
    uint8_t xStep = 0;
    uint8_t xBurst = 0;
    uint8_t yStep = 0;
    uint8_t yBurst = 0;
    uint8_t flowStep = 0;
    uint8_t flowBurst = 0;
    uint8_t instructionStep = 0;
    uint8_t instructionBurst = 0;
};

struct SauResourceConfigs
{
    SauControllerResourceConfig controller;
    SauStreamAddressResourceConfig streamAddress;
    SauResidentAddressResourceConfig residentAddress;
    SauInputResourceConfig input;
    SauTransposeReuseResourceConfig transposeReuse;
    SauArrayResourceConfig array;
    SauOutputResourceConfig output;
    SauWritebackResourceConfig writeback;
};

/**
 * Dispatch the raw control block into per-resource configurations.  The
 * dispatch is total over the decoded int8-GEMM domain and contains no
 * fixture, matrix-size, or 01/01 special case.
 */
SauResourceConfigs deriveResourceConfigs(const SauControlFields &control);

/**
 * The PLAN3_STEP0 structural path ids selected by a raw configuration.
 * Every legal CSR combination maps onto one trans, one reuse and one
 * flow path row of the frozen table.
 */
struct SauRtlPathSelection
{
    const char *transPath = nullptr;
    const char *reusePath = nullptr;
    const char *flowPath = nullptr;
};

SauRtlPathSelection selectRtlPaths(const SauControlFields &control);

} // namespace gem5::sau

#endif // __SAU_RESOURCE_CONFIG_HH__
