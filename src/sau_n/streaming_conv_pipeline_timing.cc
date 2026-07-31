#include "sau_n/streaming_conv_pipeline_timing.hh"

#include "base/logging.hh"
#include "sau_n/conv_pipeline_io.hh"
#include "sim/sim_exit.hh"

namespace gem5::sau_n
{
namespace
{

PipelineResolvedConfig
buildResolvedConfig(const StreamingConvPipelineTimingParams &params)
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
    config.sharedSpad.configured = true;
    config.sharedSpad.aBase = params.spad_a_base;
    config.sharedSpad.aRows = params.spad_a_rows;
    config.sharedSpad.bBase = params.spad_b_base;
    config.sharedSpad.bRows = params.spad_b_rows;
    config.sharedSpad.cBase = params.spad_c_base;
    config.sharedSpad.cRows = params.spad_c_rows;
    config.sharedSpad.dBase = params.spad_d_base;
    config.sharedSpad.dRows = params.spad_d_rows;
    config.sharedSpad.bBufferDepth = params.b_buffer_depth;
    config.sharedSpad.dPendingRows = params.d_pending_rows;
    config.sharedSpad.weightReuse = params.weight_reuse;
    if (params.bank_arbitration != "a_d_b") {
        fatal(
            "Unsupported shared scratchpad bank arbitration policy '%s'",
            params.bank_arbitration.c_str());
    }
    config.sharedSpad.arbitration = BankArbitrationPolicy::ADB;
    return config;
}

double
average(uint64_t numerator, uint64_t denominator)
{
    return denominator == 0 ? 0.0 :
        static_cast<double>(numerator) / denominator;
}

} // anonymous namespace

StreamingConvPipelineTiming::StreamingConvPipelineTiming(
    const Params &params)
    : ClockedObject(params), stats(this),
      resolved(buildResolvedConfig(params)),
      model(
          resolved,
          {params.output_ready_period, params.output_ready_high_cycles}),
      traceWriter(
          params.trace_file, params.resolved_config_sha256,
          params.detailed_pe_trace),
      outputFile(params.output_file),
      tickEvent([this] { tick(); }, name() + ".tick")
{
    if (outputFile.empty()) {
        fatal("StreamingConvPipelineTiming output_file must not be empty");
    }
}

void
StreamingConvPipelineTiming::startup()
{
    schedule(tickEvent, clockEdge(Cycles(0)));
}

void
StreamingConvPipelineTiming::tick()
{
    const auto observation = model.tick();
    traceWriter.emit(observation);
    if (observation.drained) {
        writeConvPipelineOutput(outputFile, resolved, model.outputs());
        updateFinalStats();
        inform(
            "Streaming conv pipeline fixture '%s' drained at cycle %llu",
            resolved.name,
            static_cast<unsigned long long>(*model.drainedCycle()));
        exitSimLoop("streaming conv pipeline drained");
        return;
    }
    schedule(tickEvent, clockEdge(Cycles(1)));
}

void
StreamingConvPipelineTiming::updateFinalStats()
{
    const uint64_t drained = *model.drainedCycle();
    const uint64_t total = drained + 1;
    const auto &pipeline = model.stats();
    const auto &producer = model.producerStats();

    stats.totalCycles = total;
    stats.drainedCycle = drained;
    stats.pipelineInputVectors = producer.inputVectors;
    stats.pipelineOutputVectors = producer.outputVectors;
    stats.pipelineFillCycles = producer.pipelineFillCycles;
    stats.producerInitiationInterval = average(
        producer.producerInputGapCycles, producer.producerInputPairs);
    stats.producerInputPairs = producer.producerInputPairs;
    stats.producerInputGapCycles = producer.producerInputGapCycles;
    stats.im2colOutputInterval = average(
        producer.im2colOutputGapCycles, producer.im2colOutputPairs);
    stats.im2colOutputPairs = producer.im2colOutputPairs;
    stats.im2colOutputGapCycles = producer.im2colOutputGapCycles;
    stats.conflictFreeOutputII = average(
        producer.conflictFreeOutputGapCycles,
        producer.conflictFreeOutputPairs);
    stats.conflictFreeOutputPairs = producer.conflictFreeOutputPairs;
    stats.conflictFreeOutputGapCycles =
        producer.conflictFreeOutputGapCycles;
    stats.conflictFreeOutputMaxGap = producer.conflictFreeOutputMaxGap;
    stats.endToEndVectorRate = average(pipeline.fifoPops, total);
    stats.bankConflictVectors = producer.bankConflictVectors;
    stats.bankConflictExtraRounds = producer.bankConflictExtraRounds;
    stats.bankConflictStallCycles = producer.bankConflictStallCycles;
    stats.rawScatteredMaskVectors = producer.rawScatteredMaskVectors;
    stats.compactedSpatialVectors = producer.compactedSpatialVectors;
    stats.downstreamStallCycles = producer.s2StallCycles;
    stats.s0StallCycles = producer.s0StallCycles;
    stats.s1StallCycles = producer.s1StallCycles;
    stats.s2StallCycles = producer.s2StallCycles;
    stats.fifoAverageOccupancy = average(
        pipeline.fifoOccupancySum, pipeline.fifoOccupancySamples);
    stats.fifoPeakOccupancy = pipeline.fifoPeakOccupancy;
    stats.fifoFullCycles = pipeline.fifoFullCycles;
    stats.fifoPushes = pipeline.fifoPushes;
    stats.fifoPops = pipeline.fifoPops;
    stats.peLaunches = pipeline.peLaunches;
    stats.peInputCycles = pipeline.peInputCycles;
    stats.peInputBubbleCycles = pipeline.peInputBubbleCycles;
    stats.peBusyNotAcceptingCycles = pipeline.peBusyNotAcceptingCycles;
    stats.tilesGenerated = pipeline.tilesGenerated;
    stats.tilesLaunched = pipeline.tilesLaunched;
    stats.tilesCompleted = pipeline.tilesCompleted;
    stats.outputRows = pipeline.outputRows;
    stats.outputElements = pipeline.outputElements;
    stats.spadReadRequestsA = pipeline.spadReadRequestsA;
    stats.spadReadGrantsA = pipeline.spadReadGrantsA;
    stats.spadReadResponsesA = pipeline.spadReadResponsesA;
    stats.spadReadRequestsC = pipeline.spadReadRequestsC;
    stats.spadReadGrantsC = pipeline.spadReadGrantsC;
    stats.spadReadResponsesC = pipeline.spadReadResponsesC;
    stats.spadReadRequestsB = pipeline.spadReadRequestsB;
    stats.spadReadGrantsB = pipeline.spadReadGrantsB;
    stats.spadReadResponsesB = pipeline.spadReadResponsesB;
    stats.bBufferFillVectors = pipeline.bBufferFillVectors;
    stats.bBufferConsumedVectors = pipeline.bBufferConsumedVectors;
    stats.bBufferHitVectors = pipeline.bBufferHitVectors;
    stats.bBufferEmptyCycles = pipeline.bBufferEmptyCycles;
    stats.bBufferSwitches = pipeline.bBufferSwitches;
    stats.bPrefetchStallCycles = pipeline.bPrefetchStallCycles;
    stats.weightReuseHits = pipeline.weightReuseHits;
    stats.spadWriteRequestsD = pipeline.spadWriteRequestsD;
    stats.spadWriteGrantsD = pipeline.spadWriteGrantsD;
    stats.dPendingPeak = pipeline.dPendingPeak;
    stats.dWriteStallCycles = pipeline.dWriteStallCycles;
    stats.bBufferAverageOccupancy = average(
        pipeline.bBufferOccupancySum,
        pipeline.bBufferOccupancySamples);
    stats.bBufferPeakOccupancy = pipeline.bBufferPeakOccupancy;
    for (uint64_t bank = 0; bank < SpBanks; ++bank) {
        stats.perBankReadCycles[bank] =
            pipeline.perBankReadCycles[bank];
        stats.perBankWriteCycles[bank] =
            pipeline.perBankWriteCycles[bank];
        stats.perBankReadWriteConflicts[bank] =
            pipeline.perBankReadWriteConflicts[bank];
    }
}

#define STREAMING_STAT(member, unit, description) \
    ADD_STAT(member, statistics::units::unit::get(), description)

StreamingConvPipelineTiming::StreamingStats::StreamingStats(
    statistics::Group *parent)
    : statistics::Group(parent, "streamingPipeline"),
      STREAMING_STAT(totalCycles, Cycle,
                     "Inclusive cycle count through drained"),
      STREAMING_STAT(drainedCycle, Cycle, "Cycle satisfying all drains"),
      STREAMING_STAT(pipelineInputVectors, Count, "Vectors accepted by S0"),
      STREAMING_STAT(pipelineOutputVectors, Count, "Vectors pushed by S2"),
      STREAMING_STAT(pipelineFillCycles, Cycle,
                     "Inclusive cycles through first S2 push"),
      STREAMING_STAT(producerInitiationInterval, Ratio,
                     "Average cycle gap between S0 input fires"),
      STREAMING_STAT(producerInputPairs, Count, "Adjacent S0 input pairs"),
      STREAMING_STAT(producerInputGapCycles, Cycle,
                     "Cycle gaps across adjacent S0 input pairs"),
      STREAMING_STAT(im2colOutputInterval, Ratio,
                     "Average cycle gap between S2 pushes"),
      STREAMING_STAT(im2colOutputPairs, Count, "Adjacent S2 output pairs"),
      STREAMING_STAT(im2colOutputGapCycles, Cycle,
                     "Cycle gaps across adjacent S2 output pairs"),
      STREAMING_STAT(conflictFreeOutputII, Ratio,
                     "Average qualified conflict-free S2 output gap"),
      STREAMING_STAT(conflictFreeOutputPairs, Count,
                     "Qualified adjacent conflict-free output pairs"),
      STREAMING_STAT(conflictFreeOutputGapCycles, Cycle,
                     "Qualified conflict-free output gap cycles"),
      STREAMING_STAT(conflictFreeOutputMaxGap, Cycle,
                     "Maximum qualified conflict-free output gap"),
      STREAMING_STAT(endToEndVectorRate, Ratio,
                     "FIFO pops per inclusive total cycle"),
      STREAMING_STAT(bankConflictVectors, Count,
                     "Vectors requiring multiple bank-read rounds"),
      STREAMING_STAT(bankConflictExtraRounds, Count,
                     "Read rounds beyond the first"),
      STREAMING_STAT(bankConflictStallCycles, Cycle,
                     "S1 cycles waiting for additional bank rounds"),
      STREAMING_STAT(rawScatteredMaskVectors, Count,
                     "Vectors with scattered raw spatial masks"),
      STREAMING_STAT(compactedSpatialVectors, Count,
                     "Vectors compacted and pushed by S2"),
      STREAMING_STAT(downstreamStallCycles, Cycle,
                     "S2 valid cycles without FIFO push readiness"),
      STREAMING_STAT(s0StallCycles, Cycle, "S0 valid cycles not retiring"),
      STREAMING_STAT(s1StallCycles, Cycle, "S1 valid cycles not retiring"),
      STREAMING_STAT(s2StallCycles, Cycle, "S2 valid cycles not pushing"),
      STREAMING_STAT(fifoAverageOccupancy, Count,
                     "Average cycle-start FIFO occupancy"),
      STREAMING_STAT(fifoPeakOccupancy, Count,
                     "Peak cycle-start FIFO occupancy"),
      STREAMING_STAT(fifoFullCycles, Cycle,
                     "Cycles starting with a full FIFO"),
      STREAMING_STAT(fifoPushes, Count, "FIFO push handshakes"),
      STREAMING_STAT(fifoPops, Count, "FIFO pop handshakes"),
      STREAMING_STAT(peLaunches, Count, "SA instruction launches"),
      STREAMING_STAT(peInputCycles, Cycle, "SA input-fire cycles"),
      STREAMING_STAT(peInputBubbleCycles, Cycle,
                     "ACCEPT_K cycles without input fire"),
      STREAMING_STAT(peBusyNotAcceptingCycles, Cycle,
                     "WAIT_RESULT and DRAIN_OUTPUT cycles"),
      STREAMING_STAT(tilesGenerated, Count, "Complete tiles generated"),
      STREAMING_STAT(tilesLaunched, Count, "Tiles launched into the SA"),
      STREAMING_STAT(tilesCompleted, Count, "Tiles completing all outputs"),
      STREAMING_STAT(outputRows, Count, "Spatial output rows collected"),
      STREAMING_STAT(outputElements, Count, "NCHW output elements written"),
      STREAMING_STAT(spadReadRequestsA, Count, "A bank read requests"),
      STREAMING_STAT(spadReadGrantsA, Count, "A bank read grants"),
      STREAMING_STAT(spadReadResponsesA, Count, "A bank read responses"),
      STREAMING_STAT(spadReadRequestsC, Count, "C bank read requests"),
      STREAMING_STAT(spadReadGrantsC, Count, "C bank read grants"),
      STREAMING_STAT(spadReadResponsesC, Count, "C bank read responses"),
      STREAMING_STAT(spadReadRequestsB, Count, "B bank read requests"),
      STREAMING_STAT(spadReadGrantsB, Count, "B bank read grants"),
      STREAMING_STAT(spadReadResponsesB, Count, "B bank read responses"),
      STREAMING_STAT(bBufferFillVectors, Count,
                     "Complete vectors filled into B buffers"),
      STREAMING_STAT(bBufferConsumedVectors, Count,
                     "B vectors consumed by SA input fires"),
      STREAMING_STAT(bBufferHitVectors, Count,
                     "Expected K vectors found ready in active B buffer"),
      STREAMING_STAT(bBufferEmptyCycles, Cycle,
                     "ACCEPT_K cycles stalled for B data"),
      STREAMING_STAT(bBufferSwitches, Count,
                     "Completed B0/B1 active-buffer switches"),
      STREAMING_STAT(bPrefetchStallCycles, Cycle,
                     "Cycles with at least one denied B bank request"),
      STREAMING_STAT(weightReuseHits, Count,
                     "B vectors reused by later spatial tiles"),
      STREAMING_STAT(spadWriteRequestsD, Count, "D bank write requests"),
      STREAMING_STAT(spadWriteGrantsD, Count, "D bank write grants"),
      STREAMING_STAT(dPendingPeak, Count, "Peak pending D rows"),
      STREAMING_STAT(dWriteStallCycles, Cycle,
                     "Cycles with at least one denied D bank write"),
      STREAMING_STAT(bBufferAverageOccupancy, Ratio,
                     "Average cycle-start ready B entries"),
      STREAMING_STAT(bBufferPeakOccupancy, Count,
                     "Peak cycle-start ready B entries"),
      STREAMING_STAT(perBankReadCycles, Cycle,
                     "Per-bank cycles granting a shared read"),
      STREAMING_STAT(perBankWriteCycles, Cycle,
                     "Per-bank cycles granting a D write"),
      STREAMING_STAT(perBankReadWriteConflicts, Cycle,
                     "Per-bank cycles with competing read and D write")
{
    perBankReadCycles.init(SpBanks);
    perBankWriteCycles.init(SpBanks);
    perBankReadWriteConflicts.init(SpBanks);
}

#undef STREAMING_STAT

} // namespace gem5::sau_n
