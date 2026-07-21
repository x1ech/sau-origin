#include "sau_n/conv_pipeline_timing.hh"

#include "base/logging.hh"
#include "sim/sim_exit.hh"

namespace gem5::sau_n
{
namespace
{

PipelineResolvedConfig
buildResolvedConfig(const ConvPipelineTimingParams &params)
{
    PipelineResolvedConfig config;
    config.schemaVersion = params.schema_version;
    config.name = params.fixture_name;
    config.im2col.schemaVersion = params.schema_version;
    config.im2col.name = params.im2col_name;
    config.im2col.n = params.n;
    config.im2col.c = params.c;
    config.im2col.h = params.h;
    config.im2col.w = params.w;
    config.im2col.outH = params.out_h;
    config.im2col.outW = params.out_w;
    config.im2col.kernelH = params.kernel_h;
    config.im2col.kernelW = params.kernel_w;
    config.im2col.strideH = params.stride_h;
    config.im2col.strideW = params.stride_w;
    config.im2col.dilationH = params.dilation_h;
    config.im2col.dilationW = params.dilation_w;
    config.im2col.padTop = params.pad_top;
    config.im2col.padLeft = params.pad_left;
    config.im2col.spadBase = params.spad_base;
    config.im2col.cfgDwMode = params.cfg_dw_mode;
    config.im2col.cfgKernelPattern = params.cfg_kernel_pattern;
    config.im2col.inputGenerator = params.input_generator;
    config.outChannels = params.out_channels;
    config.cutbit = params.cutbit;
    config.weightGenerator = params.weight_generator;
    config.biasGenerator = params.bias_generator;
    return config;
}

} // anonymous namespace

ConvPipelineTiming::ConvPipelineTiming(const Params &params)
    : ClockedObject(params), pipelineStats(this), sauStats(this),
      resolved(buildResolvedConfig(params)),
      model(
          resolved,
          {params.output_ready_period, params.output_ready_high_cycles}),
      traceWriter(params.trace_file, params.resolved_config_sha256),
      outputFile(params.output_file),
      tickEvent([this] { tick(); }, name() + ".tick")
{
    if (outputFile.empty()) {
        fatal("ConvPipelineTiming output_file must not be empty");
    }
}

void
ConvPipelineTiming::startup()
{
    schedule(tickEvent, clockEdge(Cycles(0)));
}

void
ConvPipelineTiming::tick()
{
    const auto observation = model.tick();
    traceWriter.emit(observation);
    if (observation.drained) {
        writeConvPipelineOutput(outputFile, resolved, model.outputs());
        updateFinalStats();
        inform(
            "Conv pipeline fixture '%s' drained at cycle %llu "
            "(im2col_done=%llu, sau_last_result=%llu)",
            resolved.name,
            static_cast<unsigned long long>(*model.drainedCycle()),
            static_cast<unsigned long long>(*model.im2colDoneCycle()),
            static_cast<unsigned long long>(*model.sauLastResultCycle()));
        exitSimLoop("conv pipeline drained");
        return;
    }
    schedule(tickEvent, clockEdge(Cycles(1)));
}

void
ConvPipelineTiming::updateFinalStats()
{
    const uint64_t im2colDone = *model.im2colDoneCycle();
    const uint64_t lastResult = *model.sauLastResultCycle();
    const uint64_t drained = *model.drainedCycle();
    const uint64_t totalCycles = drained + 1;
    const auto &modelStats = model.stats();

    pipelineStats.im2colDoneCycle = im2colDone;
    pipelineStats.sauLastResultCycle = lastResult;
    pipelineStats.drainedCycle = drained;
    pipelineStats.totalCycles = totalCycles;
    pipelineStats.collectTileCycles = modelStats.collectTileCycles;
    pipelineStats.nonCollectCycles =
        totalCycles - modelStats.collectTileCycles;
    pipelineStats.postIm2colDrainCycles = drained - im2colDone;

    sauStats.tilesCollected = modelStats.tilesCollected;
    sauStats.tilesLaunched = modelStats.tilesLaunched;
    sauStats.tilesCompleted = modelStats.tilesCompleted;
    sauStats.tileBufferAverageOccupancy =
        static_cast<double>(modelStats.tileBufferOccupancySum) /
        modelStats.tileBufferOccupancySamples;
    sauStats.tileBufferPeakOccupancy = modelStats.tileBufferPeakOccupancy;
    sauStats.activationHandshakes = modelStats.activationHandshakes;
    sauStats.engineInputCycles = modelStats.engineInputCycles;
    sauStats.im2colBackpressureCycles = modelStats.im2colBackpressureCycles;
    sauStats.usefulMacs = modelStats.usefulMacs;
    sauStats.arrayActiveCycles = modelStats.arrayActiveCycles;
    sauStats.activeCycleMacUtilization = modelStats.arrayActiveCycles == 0 ?
        0.0 : static_cast<double>(modelStats.usefulMacs) /
            (SauRows * SauColumns * modelStats.arrayActiveCycles);
    sauStats.endToEndMacUtilization =
        static_cast<double>(modelStats.usefulMacs) /
        (SauRows * SauColumns * totalCycles);
    sauStats.outputRows = modelStats.outputRows;
    sauStats.outputElements = modelStats.outputElements;
    sauStats.positiveSaturations = modelStats.positiveSaturations;
    sauStats.negativeSaturations = modelStats.negativeSaturations;
    sauStats.outputBackpressureCycles =
        modelStats.outputBackpressureCycles;
}

ConvPipelineTiming::PipelineStats::PipelineStats(
    statistics::Group *parent)
    : statistics::Group(parent, "convPipeline"),
      ADD_STAT(im2colDoneCycle, statistics::units::Cycle::get(),
               "Cycle containing the registered Im2Col done pulse"),
      ADD_STAT(sauLastResultCycle, statistics::units::Cycle::get(),
               "Cycle containing the final registered SA output row"),
      ADD_STAT(drainedCycle, statistics::units::Cycle::get(),
               "Cycle in which every pipeline drain condition holds"),
      ADD_STAT(totalCycles, statistics::units::Cycle::get(),
               "Inclusive cycle count through pipeline drained"),
      ADD_STAT(collectTileCycles, statistics::units::Cycle::get(),
               "Cycles with the pipeline in CollectTile state"),
      ADD_STAT(nonCollectCycles, statistics::units::Cycle::get(),
               "Total pipeline cycles excluding CollectTile state"),
      ADD_STAT(postIm2colDrainCycles, statistics::units::Cycle::get(),
               "Cycles from Im2Col done through pipeline drained")
{
}

ConvPipelineTiming::SauStats::SauStats(statistics::Group *parent)
    : statistics::Group(parent, "sau"),
      ADD_STAT(tilesCollected, statistics::units::Count::get(),
               "Complete activation tiles collected"),
      ADD_STAT(tilesLaunched, statistics::units::Count::get(),
               "Activation tiles launched into the SA"),
      ADD_STAT(tilesCompleted, statistics::units::Count::get(),
               "Activation tiles with every output row collected"),
      ADD_STAT(tileBufferAverageOccupancy, statistics::units::Count::get(),
               "Average cycle-start tile-buffer entry occupancy"),
      ADD_STAT(tileBufferPeakOccupancy, statistics::units::Count::get(),
               "Peak cycle-start tile-buffer entry occupancy"),
      ADD_STAT(activationHandshakes, statistics::units::Count::get(),
               "Im2Col vectors accepted by the tile buffer"),
      ADD_STAT(engineInputCycles, statistics::units::Cycle::get(),
               "Continuous valid input cycles driven into the SA"),
      ADD_STAT(im2colBackpressureCycles, statistics::units::Cycle::get(),
               "Cycles stalling a valid Im2Col feed"),
      ADD_STAT(usefulMacs, statistics::units::Count::get(),
               "MACs for valid spatial rows and output columns"),
      ADD_STAT(arrayActiveCycles, statistics::units::Cycle::get(),
               "Cycles containing at least one PE MAC commit"),
      ADD_STAT(activeCycleMacUtilization, statistics::units::Ratio::get(),
               "Useful MACs divided by 256 times array-active cycles"),
      ADD_STAT(endToEndMacUtilization, statistics::units::Ratio::get(),
               "Useful MACs divided by 256 times total pipeline cycles"),
      ADD_STAT(outputRows, statistics::units::Count::get(),
               "Registered spatial output rows collected"),
      ADD_STAT(outputElements, statistics::units::Count::get(),
               "Logical signed INT8 NCHW output elements collected"),
      ADD_STAT(positiveSaturations, statistics::units::Count::get(),
               "Outputs clamped above signed INT8 maximum"),
      ADD_STAT(negativeSaturations, statistics::units::Count::get(),
               "Outputs clamped below signed INT8 minimum"),
      ADD_STAT(outputBackpressureCycles, statistics::units::Cycle::get(),
               "Cycles stalling an active SA output sequence")
{
}

} // namespace gem5::sau_n
