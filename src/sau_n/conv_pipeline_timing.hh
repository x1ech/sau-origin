#ifndef __SAU_N_CONV_PIPELINE_TIMING_HH__
#define __SAU_N_CONV_PIPELINE_TIMING_HH__

#include <string>

#include "base/statistics.hh"
#include "params/ConvPipelineTiming.hh"
#include "sau_n/conv_pipeline_io.hh"
#include "sau_n/conv_pipeline_model.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"

namespace gem5::sau_n
{

class ConvPipelineTiming : public ClockedObject
{
  public:
    PARAMS(ConvPipelineTiming);

    explicit ConvPipelineTiming(const Params &params);
    void startup() override;

  private:
    struct PipelineStats : public statistics::Group
    {
        explicit PipelineStats(statistics::Group *parent);

        statistics::Scalar im2colDoneCycle;
        statistics::Scalar sauLastResultCycle;
        statistics::Scalar drainedCycle;
        statistics::Scalar totalCycles;
        statistics::Scalar postIm2colDrainCycles;
    } pipelineStats;

    struct SauStats : public statistics::Group
    {
        explicit SauStats(statistics::Group *parent);

        statistics::Scalar tilesCollected;
        statistics::Scalar tilesLaunched;
        statistics::Scalar tilesCompleted;
        statistics::Scalar tileBufferAverageOccupancy;
        statistics::Scalar tileBufferPeakOccupancy;
        statistics::Scalar activationHandshakes;
        statistics::Scalar engineInputCycles;
        statistics::Scalar im2colBackpressureCycles;
        statistics::Scalar usefulMacs;
        statistics::Scalar arrayActiveCycles;
        statistics::Scalar activeCycleMacUtilization;
        statistics::Scalar endToEndMacUtilization;
        statistics::Scalar outputRows;
        statistics::Scalar outputElements;
        statistics::Scalar positiveSaturations;
        statistics::Scalar negativeSaturations;
        statistics::Scalar outputBackpressureCycles;
    } sauStats;

    void tick();
    void updateFinalStats();

    const PipelineResolvedConfig resolved;
    ConvPipelineModel model;
    ConvPipelineTraceWriter traceWriter;
    const std::string outputFile;
    EventFunctionWrapper tickEvent;
};

} // namespace gem5::sau_n

#endif // __SAU_N_CONV_PIPELINE_TIMING_HH__
