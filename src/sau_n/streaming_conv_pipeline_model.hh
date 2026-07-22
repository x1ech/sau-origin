#ifndef __SAU_N_STREAMING_CONV_PIPELINE_MODEL_HH__
#define __SAU_N_STREAMING_CONV_PIPELINE_MODEL_HH__

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "sau_n/pipelined_im2col_model.hh"
#include "sau_n/sau_model.hh"

namespace gem5::sau_n
{

struct StreamingConvPipelineStats
{
    uint64_t fifoOccupancySamples = 0;
    uint64_t fifoOccupancySum = 0;
    uint64_t fifoPeakOccupancy = 0;
    uint64_t fifoFullCycles = 0;
    uint64_t fifoPushes = 0;
    uint64_t fifoPops = 0;
    uint64_t peLaunches = 0;
    uint64_t peInputCycles = 0;
    uint64_t peInputBubbleCycles = 0;
    uint64_t peBusyNotAcceptingCycles = 0;
    uint64_t tilesGenerated = 0;
    uint64_t tilesLaunched = 0;
    uint64_t tilesCompleted = 0;
    uint64_t outputRows = 0;
    uint64_t outputElements = 0;
};

struct StreamingConvPipelineCycle
{
    uint64_t cycle = 0;
    StreamingConsumerState consumerState = StreamingConsumerState::Idle;
    uint64_t acceptedK = 0;
    uint64_t activeTile = 0;
    uint64_t fifoCount = 0;
    uint64_t fifoReadPointer = 0;
    uint64_t fifoWritePointer = 0;
    bool fifoHeadValid = false;
    StreamingFifoEntry fifoHead{};
    bool fifoPushReady = false;
    bool fifoPush = false;
    bool fifoPop = false;
    StreamingConsumerDecision consumer{};
    PipelinedIm2ColCycle producer{};
    SauCycleInputs sauInputs{};
    SauCycleObservation sau{};
    bool outputCollected = false;
    bool drained = false;
};

class StreamingConvPipelineModel
{
  public:
    static constexpr SauInputProtocol InputProtocol =
        SauInputProtocol::ElasticBubbleEnabled;

    explicit StreamingConvPipelineModel(
        const PipelineResolvedConfig &config,
        const OutputReadyConfig &ready = {});

    StreamingConvPipelineCycle tick();

    const PipelineResolvedConfig &config() const { return resolved; }
    const PipelineDerivedConfig &derived() const { return dimensions; }
    const StreamingConvPipelineStats &stats() const { return counters; }
    const PipelinedIm2ColStats &producerStats() const
    {
        return producer.stats();
    }
    const std::vector<int8_t> &outputs() const { return collectedOutputs; }
    uint64_t nextCycle() const { return cycleNumber; }
    bool hasDrained() const { return drainedAt.has_value(); }
    std::optional<uint64_t> drainedCycle() const { return drainedAt; }

  private:
    SauCycleInputs buildSauInputs(
        const StreamingConsumerDecision &decision,
        const StreamingFifoEntry &head) const;
    void collectOutput(const SauCycleObservation &observation);
    void completeActiveTile();
    uint64_t outputIndex(
        const StreamingSpatialCoordinate &coordinate,
        uint64_t outputChannel) const;
    void checkInvariants() const;

    PipelineResolvedConfig resolved;
    PipelineDerivedConfig dimensions;
    OutputReadyConfig readyConfig;
    PipelinedIm2ColModel producer;
    SauCycleModel sauModel;
    std::array<StreamingFifoEntry, StreamingFifoDepth> fifo{};
    uint64_t fifoCount = 0;
    uint64_t fifoReadPointer = 0;
    uint64_t fifoWritePointer = 0;
    StreamingConsumerState consumerState = StreamingConsumerState::Idle;
    uint64_t activeTile = 0;
    uint64_t acceptedK = 0;
    uint64_t tileOutputRows = 0;
    std::optional<StreamingFifoEntry> activeEntry;
    StreamingConvPipelineStats counters{};
    std::vector<int8_t> collectedOutputs;
    std::vector<bool> outputWritten;
    uint64_t cycleNumber = 0;
    std::optional<uint64_t> drainedAt;
};

} // namespace gem5::sau_n

#endif // __SAU_N_STREAMING_CONV_PIPELINE_MODEL_HH__
