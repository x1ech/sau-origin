#ifndef __SAU_N_STREAMING_CONV_PIPELINE_TIMING_HH__
#define __SAU_N_STREAMING_CONV_PIPELINE_TIMING_HH__

#include <string>

#include "base/statistics.hh"
#include "params/StreamingConvPipelineTiming.hh"
#include "sau_n/streaming_conv_pipeline_io.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"

namespace gem5::sau_n
{

class StreamingConvPipelineTiming : public ClockedObject
{
  public:
    PARAMS(StreamingConvPipelineTiming);

    explicit StreamingConvPipelineTiming(const Params &params);
    void startup() override;

  private:
    struct StreamingStats : public statistics::Group
    {
        explicit StreamingStats(statistics::Group *parent);

        statistics::Scalar totalCycles;
        statistics::Scalar drainedCycle;
        statistics::Scalar pipelineInputVectors;
        statistics::Scalar pipelineOutputVectors;
        statistics::Scalar pipelineFillCycles;
        statistics::Scalar producerInitiationInterval;
        statistics::Scalar producerInputPairs;
        statistics::Scalar producerInputGapCycles;
        statistics::Scalar im2colOutputInterval;
        statistics::Scalar im2colOutputPairs;
        statistics::Scalar im2colOutputGapCycles;
        statistics::Scalar conflictFreeOutputII;
        statistics::Scalar conflictFreeOutputPairs;
        statistics::Scalar conflictFreeOutputGapCycles;
        statistics::Scalar conflictFreeOutputMaxGap;
        statistics::Scalar endToEndVectorRate;
        statistics::Scalar bankConflictVectors;
        statistics::Scalar bankConflictExtraRounds;
        statistics::Scalar bankConflictStallCycles;
        statistics::Scalar rawScatteredMaskVectors;
        statistics::Scalar compactedSpatialVectors;
        statistics::Scalar downstreamStallCycles;
        statistics::Scalar s0StallCycles;
        statistics::Scalar s1StallCycles;
        statistics::Scalar s2StallCycles;
        statistics::Scalar fifoAverageOccupancy;
        statistics::Scalar fifoPeakOccupancy;
        statistics::Scalar fifoFullCycles;
        statistics::Scalar fifoPushes;
        statistics::Scalar fifoPops;
        statistics::Scalar peLaunches;
        statistics::Scalar peInputCycles;
        statistics::Scalar peInputBubbleCycles;
        statistics::Scalar peBusyNotAcceptingCycles;
        statistics::Scalar tilesGenerated;
        statistics::Scalar tilesLaunched;
        statistics::Scalar tilesCompleted;
        statistics::Scalar outputRows;
        statistics::Scalar outputElements;
    } stats;

    void tick();
    void updateFinalStats();

    const PipelineResolvedConfig resolved;
    StreamingConvPipelineModel model;
    StreamingConvPipelineTraceWriter traceWriter;
    const std::string outputFile;
    EventFunctionWrapper tickEvent;
};

} // namespace gem5::sau_n

#endif // __SAU_N_STREAMING_CONV_PIPELINE_TIMING_HH__
