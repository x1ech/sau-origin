#ifndef __SAU_ADDRESS_PROGRAM_HH__
#define __SAU_ADDRESS_PROGRAM_HH__

#include <cstdint>

#include "sau/resource_config.hh"
#include "sau/types.hh"

namespace gem5::sau
{

/**
 * Raw-counter address programs copied from the frozen RTL address
 * generators (mem_addr.sv and register_addr.sv, hashes in
 * RTL_TIMING_PROVENANCE.md).  They advance x/y/flow/instruction (or
 * x/y/channel) counters exactly as the source state machines do,
 * including the composite step registers, zero-count guards, wrap
 * arithmetic, and padding freezes.  They model the accepted-beat
 * address/index/last sequence only; request timing stays owned by the
 * strict per-tick skeletons.
 */

/** mem_addr.sv streamed-operand address sequence. */
class RtlStreamAddressProgram
{
  public:
    explicit RtlStreamAddressProgram(
        const SauStreamAddressResourceConfig &config);

    bool done() const;
    Addr address() const;
    /** vertical_cnt_last: raised on the final accepted row of a flow. */
    bool lastOfFlow() const;
    /** Final accepted beat of the whole program. */
    bool last() const;
    uint32_t x() const;
    uint32_t y() const;
    uint32_t flow() const;
    uint32_t instruction() const;
    void advance();

  private:
    uint32_t xLimit = 0;
    uint32_t yLimit = 0;
    uint32_t flowLimit = 0;
    uint32_t instructionLimit = 0;
    uint32_t stepXBytes = 0;
    uint32_t stepYBytes = 0;
    uint32_t stepFlowBytes = 0;
    uint32_t stepInstructionBytes = 0;
    uint32_t cntX = 0;
    uint32_t cntY = 0;
    uint32_t cntFlow = 0;
    uint32_t cntInstruction = 0;
    uint32_t currentAddr = 0;
    uint32_t rowStartAddr = 0;
    uint32_t flowStartAddr = 0;
    uint32_t instructionStartAddr = 0;
    bool exhausted = false;
};

/**
 * register_addr.sv resident-load address sequence.  The same RTL module
 * also generates the output-RF unload addresses (IS_OUTPUT elaboration),
 * so this program serves both with their respective counter groups.
 */
class RtlResidentAddressProgram
{
  public:
    explicit RtlResidentAddressProgram(
        const SauResidentAddressResourceConfig &config);

    bool done() const;
    Addr address() const;
    /** register_wrptr: advances every accepted beat, padding included. */
    uint32_t writePointer() const;
    /** register_padding_flag: current beat is x- or y-padded. */
    bool paddingBeat() const;
    /** register_rdaddr_last: all three counters at their limits. */
    bool last() const;
    uint32_t x() const;
    uint32_t y() const;
    uint32_t channel() const;
    void advance();

  private:
    bool rawYPadding() const;
    bool xPadding() const;

    uint32_t xLimit = 0;
    uint32_t yLimit = 0;
    uint32_t channelLimit = 0;
    uint32_t stepYBytes = 0;
    uint32_t stepChannelBytes = 0;
    bool paddingEnabled = false;
    uint32_t validYStart = 0;
    uint32_t validYEnd = 0;
    uint32_t validXStart = 0;
    uint32_t validXEnd = 0;
    uint32_t cntX = 0;
    uint32_t cntY = 0;
    uint32_t cntChannel = 0;
    uint32_t pointer = 0;
    uint32_t currentAddr = 0;
    uint32_t rowStartAddr = 0;
    uint32_t channelStartAddr = 0;
    bool exhausted = false;
};

} // namespace gem5::sau

#endif // __SAU_ADDRESS_PROGRAM_HH__
