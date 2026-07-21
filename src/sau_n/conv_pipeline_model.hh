#ifndef __SAU_N_CONV_PIPELINE_MODEL_HH__
#define __SAU_N_CONV_PIPELINE_MODEL_HH__

#include <cstdint>
#include <optional>
#include <vector>

#include "sau_n/im2col_model.hh"
#include "sau_n/sau_model.hh"
#include "sau_n/sau_tile_buffer.hh"
#include "sau_n/sau_types.hh"

namespace gem5::sau_n
{

struct ConvPipelineModelStats
{
    uint64_t collectTileCycles = 0;
    uint64_t tilesCollected = 0;
    uint64_t tilesLaunched = 0;
    uint64_t tilesCompleted = 0;
    uint64_t activationHandshakes = 0;
    uint64_t engineInputCycles = 0;
    uint64_t tileBufferOccupancySamples = 0;
    uint64_t tileBufferOccupancySum = 0;
    uint64_t tileBufferPeakOccupancy = 0;
    uint64_t im2colBackpressureCycles = 0;
    uint64_t usefulMacs = 0;
    uint64_t arrayActiveCycles = 0;
    uint64_t outputRows = 0;
    uint64_t outputElements = 0;
    uint64_t positiveSaturations = 0;
    uint64_t negativeSaturations = 0;
    uint64_t outputBackpressureCycles = 0;
};

struct ConvPipelineCycle
{
    uint64_t cycle = 0;
    PipelineState state = PipelineState::Idle;
    uint64_t tileIndex = 0;
    uint64_t tileBufferCount = 0;
    uint64_t collectK = 0;
    uint64_t streamK = 0;
    bool im2colFeedHandshake = false;
    bool saInputValid = false;
    SauCycleInputs sauInputs{};
    bool outputReady = true;
    bool outputCollected = false;
    bool sauLastResult = false;
    bool drained = false;
    Im2ColCycle im2col;
    SauCycleObservation sau;
};

class ConvPipelineModel
{
  public:
    explicit ConvPipelineModel(
        const PipelineResolvedConfig &config,
        const OutputReadyConfig &ready = {});

    ConvPipelineCycle tick();

    const PipelineResolvedConfig &config() const { return resolved; }
    const PipelineDerivedConfig &derived() const { return dimensions; }
    const ConvPipelineModelStats &stats() const { return counters; }
    const std::vector<int8_t> &outputs() const { return collectedOutputs; }
    uint64_t nextCycle() const { return cycleNumber; }
    bool hasDrained() const { return drainedAt.has_value(); }
    std::optional<uint64_t> im2colDoneCycle() const { return im2colDoneAt; }
    std::optional<uint64_t> sauLastResultCycle() const
    {
        return sauLastResultAt;
    }
    std::optional<uint64_t> drainedCycle() const { return drainedAt; }

  private:
    SauCycleInputs saInputsForState(PipelineState oldState) const;
    void acceptIm2ColFeed(const FeedVector &feed);
    void collectOutput(const SauCycleObservation &observation);
    void completeTile();
    uint64_t outputIndex(
        const SpatialCoordinate &coordinate, uint64_t outputChannel) const;
    void checkInvariants() const;

    PipelineResolvedConfig resolved;
    PipelineDerivedConfig dimensions;
    OutputReadyConfig readyConfig;
    Im2ColModel im2colModel;
    SauTileBuffer tileBuffer;
    SauCycleModel sauModel;
    PipelineState pipelineState = PipelineState::CollectTile;
    ConvPipelineModelStats counters;
    std::vector<int8_t> collectedOutputs;
    std::vector<bool> outputWritten;
    uint64_t collectK = 0;
    uint64_t streamK = 0;
    uint64_t tileOutputRows = 0;
    uint64_t cycleNumber = 0;
    std::optional<uint64_t> im2colDoneAt;
    std::optional<uint64_t> sauLastResultAt;
    std::optional<uint64_t> drainedAt;
};

} // namespace gem5::sau_n

#endif // __SAU_N_CONV_PIPELINE_MODEL_HH__
