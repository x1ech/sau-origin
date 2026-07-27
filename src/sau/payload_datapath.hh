#ifndef __SAU_PAYLOAD_DATAPATH_HH__
#define __SAU_PAYLOAD_DATAPATH_HH__

#include <cstdint>
#include <deque>
#include <optional>

#include "sau/data_beat.hh"
#include "sau/input_datapath.hh"
#include "sau/resource_config.hh"
#include "sau/transposer.hh"

namespace gem5::sau
{

struct PayloadBoundaryTransfer
{
    uint64_t edge = 0;
    MemoryBeat256 data;
};

struct TransposerBoundaryTransfer : public PayloadBoundaryTransfer
{
    unsigned bank = 0;
};

struct PayloadBoundaryEvents
{
    std::optional<PayloadBoundaryTransfer> operandA;
    std::optional<PayloadBoundaryTransfer> operandB;
    std::optional<TransposerBoundaryTransfer> transposerInput;
    std::optional<TransposerBoundaryTransfer> transposerOutput;
    std::optional<TransposerBoundaryTransfer> transposerPrefetch;
};

/**
 * PLAN3 Step 3 strict payload-side input datapath.  The strict per-tick
 * command driver owns every control edge; this composition moves the
 * real 256-bit payloads along those edges and owns the payload-side
 * statistics:
 *
 * - a mem_ctrl-visible resident beat enters the input register file
 *   through the write path at the sequential write pointer;
 * - a mem_ctrl-visible streamed beat queues for the feeder operand-B
 *   chain (payloads pair with driver pulses in stream order; the
 *   four-register chain delay stays owned by the driver);
 * - a register-file read-valid edge advances the read-pointer program
 *   (replaying it when exhausted, the reuse readout) and queues the
 *   readout payload for the operand-A path;
 * - an operand edge pops its queue; when the transpose/reuse config
 *   loads that operand, the row enters the T0/T1 arbiter with the
 *   RTL steering and ping-pong;
 * - an accepted SA-enable edge consumes one operand-bank column when
 *   the transposer is in the datapath.
 *
 * Pulse/payload mismatches (an operand edge with an empty queue, a row
 * with no bank able to take input, an SA edge with no drainable
 * column) are counted, not fatal: they are bring-up divergence
 * evidence for the statistics, and the RTL itself latches an error
 * flag rather than corrupting state on a not-ready bank write.
 */
class StrictPayloadDatapath
{
  public:
    explicit StrictPayloadDatapath(const SauResourceConfigs &configs);

    /** Clear the per-edge observable boundary events. */
    void beginCycle();
    void onMemoryDataVisible(uint64_t edge, const MemoryBeat256 &payload,
                             bool streamed);
    void onRegisterFileReadValid(uint64_t edge);
    void onOperandAValid(uint64_t edge);
    void onOperandBValid(uint64_t edge);
    void onSaEnable(uint64_t edge);
    /** Per-cycle occupancy/busy sampling, once per strict tick. */
    void sampleCycle();

    uint64_t residentBeats() const { return residentCount; }
    uint64_t streamedBeats() const { return streamedCount; }
    uint64_t operandATokens() const { return operandACount; }
    uint64_t operandBTokens() const { return operandBCount; }
    uint64_t transposerInputRows() const { return inputRows; }
    uint64_t transposerOutputColumns() const { return outputColumns; }
    uint64_t transposerInputStalls() const { return inputStalls; }
    uint64_t transposerOutputStalls() const { return outputStalls; }
    uint64_t payloadUnderflows() const { return underflows; }
    uint64_t transposerBusyCycles() const { return busyCycles; }
    unsigned transposerMaxOccupancy() const { return maxOccupancy; }
    std::optional<uint64_t> firstRowEdge() const { return firstRowAt; }
    std::optional<uint64_t> firstColumnEdge() const
    {
        return firstColumnAt;
    }

    const InputRegisterFile &registerFileState() const { return file; }
    const TransposerArbiter &arbiterState() const { return arbiter; }
    /** The most recent column consumed at an SA-enable edge. */
    const MemoryBeat256 &lastColumn() const { return lastColumnData; }
    const PayloadBoundaryEvents &boundaryEvents() const { return events; }

  private:
    unsigned bankOccupancy(const TransposerTinyBank &bank) const;
    void restartReadoutProgram();

    const SauResourceConfigs configs;
    InputRegisterFile file;
    InputWritePath writePath;
    std::optional<InputReadPointerProgram> readout;
    TransposerArbiter arbiter;
    bool outputPhaseStarted = false;

    uint8_t writePointer = 0;
    std::deque<MemoryBeat256> readoutQueue;
    std::deque<MemoryBeat256> streamQueue;

    uint64_t residentCount = 0;
    uint64_t streamedCount = 0;
    uint64_t operandACount = 0;
    uint64_t operandBCount = 0;
    uint64_t inputRows = 0;
    uint64_t outputColumns = 0;
    uint64_t inputStalls = 0;
    uint64_t outputStalls = 0;
    uint64_t underflows = 0;
    uint64_t busyCycles = 0;
    unsigned maxOccupancy = 0;
    std::optional<uint64_t> firstRowAt;
    std::optional<uint64_t> firstColumnAt;
    MemoryBeat256 lastColumnData;
    PayloadBoundaryEvents events;
};

} // namespace gem5::sau

#endif // __SAU_PAYLOAD_DATAPATH_HH__
