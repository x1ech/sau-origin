#include "sau_n/im2col_timing.hh"

#include "base/logging.hh"
#include "sim/sim_exit.hh"

namespace gem5::sau_n
{
namespace
{

ResolvedConfig
buildResolvedConfig(const Im2ColTimingParams &params)
{
    ResolvedConfig config;
    config.schemaVersion = params.schema_version;
    config.name = params.fixture_name;
    config.n = params.n;
    config.c = params.c;
    config.h = params.h;
    config.w = params.w;
    config.outH = params.out_h;
    config.outW = params.out_w;
    config.kernelH = params.kernel_h;
    config.kernelW = params.kernel_w;
    config.strideH = params.stride_h;
    config.strideW = params.stride_w;
    config.dilationH = params.dilation_h;
    config.dilationW = params.dilation_w;
    config.padTop = params.pad_top;
    config.padLeft = params.pad_left;
    config.spadBase = params.spad_base;
    config.cfgDwMode = params.cfg_dw_mode;
    config.cfgKernelPattern = params.cfg_kernel_pattern;
    config.inputGenerator = params.input_generator;
    return config;
}

} // anonymous namespace

Im2ColTiming::Im2ColTiming(const Params &params)
    : ClockedObject(params),
      stats(this),
      resolved(buildResolvedConfig(params)),
      model(resolved),
      ready(params.ready_period, params.ready_high_cycles),
      traceWriter(params.trace_file, params.resolved_config_sha256),
      tickEvent([this] { tick(); }, name() + ".tick")
{
}

void
Im2ColTiming::startup()
{
    schedule(tickEvent, clockEdge(Cycles(0)));
}

void
Im2ColTiming::tick()
{
    const uint64_t cycle = model.nextCycle();
    const auto observation = model.tick(ready.at(cycle));
    traceWriter.emit(observation);
    if (observation.drained) {
        updateFinalStats();
        inform(
            "Im2Col fixture '%s' drained at cycle %llu "
            "(done=%llu, post_done_drain=%llu)",
            resolved.name,
            static_cast<unsigned long long>(*model.drainedCycle()),
            static_cast<unsigned long long>(*model.rtlDoneCycle()),
            static_cast<unsigned long long>(*model.postDoneDrainCycles()));
        exitSimLoop("im2col model drained");
        return;
    }
    schedule(tickEvent, clockEdge(Cycles(1)));
}

void
Im2ColTiming::updateFinalStats()
{
    const uint64_t doneCycle = *model.rtlDoneCycle();
    const uint64_t drainCycle = *model.drainedCycle();
    const uint64_t totalCycles = drainCycle + 1;
    const auto &modelStats = model.stats();

    stats.rtlDoneCycle = doneCycle;
    stats.drainedCycle = drainCycle;
    stats.postDoneDrainCycles = *model.postDoneDrainCycles();
    stats.totalDoneCycles = doneCycle + 1;
    stats.totalDrainedCycles = totalCycles;
    stats.feedVectors = modelStats.pushCount;
    stats.handshakes = modelStats.handshakeCount;
    stats.feedVectorsPerCycle =
        static_cast<double>(modelStats.pushCount) / totalCycles;
    stats.presentedLanes = modelStats.presentedLanes;
    stats.sramReadLanes = modelStats.sramReadLanes;
    stats.paddingZeroLanes = modelStats.paddingZeroLanes;
    stats.invalidLanes = modelStats.invalidLanes;
    for (std::size_t bank = 0; bank < ScratchpadBanks; ++bank) {
        stats.bankRequestCycles[bank] = modelStats.bankRequestCycles[bank];
        stats.bankUtilization[bank] =
            static_cast<double>(modelStats.bankRequestCycles[bank]) /
            totalCycles;
    }
    stats.bankRowConflicts = modelStats.bankRowConflicts;
    stats.extraCollectCycles = modelStats.extraCollectCycles;
    stats.fifoAverageOccupancy =
        static_cast<double>(modelStats.fifoOccupancySum) /
        modelStats.fifoOccupancySamples;
    stats.fifoPeakOccupancy = modelStats.fifoPeak;
    stats.fifoFullStallCycles = modelStats.fifoFullStallCycles;
    stats.backpressureCycles = modelStats.backpressureCycles;
}

Im2ColTiming::Im2ColStats::Im2ColStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(rtlDoneCycle, statistics::units::Cycle::get(),
               "Cycle containing the registered RTL done pulse"),
      ADD_STAT(drainedCycle, statistics::units::Cycle::get(),
               "First post-done cycle with an empty FIFO"),
      ADD_STAT(postDoneDrainCycles, statistics::units::Cycle::get(),
               "Cycles from done to drained"),
      ADD_STAT(totalDoneCycles, statistics::units::Cycle::get(),
               "Inclusive cycle count through done"),
      ADD_STAT(totalDrainedCycles, statistics::units::Cycle::get(),
               "Inclusive cycle count through drained"),
      ADD_STAT(feedVectors, statistics::units::Count::get(),
               "Vectors successfully pushed into the FIFO"),
      ADD_STAT(handshakes, statistics::units::Count::get(),
               "Vectors accepted by the downstream interface"),
      ADD_STAT(feedVectorsPerCycle, statistics::units::Ratio::get(),
               "Feed vectors per inclusive drained cycle"),
      ADD_STAT(presentedLanes, statistics::units::Count::get(),
               "SRAM-read plus padding-zero lanes in pushed vectors"),
      ADD_STAT(sramReadLanes, statistics::units::Count::get(),
               "Presented lanes populated by SRAM reads"),
      ADD_STAT(paddingZeroLanes, statistics::units::Count::get(),
               "Presented lanes populated by padding zeros"),
      ADD_STAT(invalidLanes, statistics::units::Count::get(),
               "Lanes excluded from feed masks"),
      ADD_STAT(bankRequestCycles, statistics::units::Cycle::get(),
               "Request cycles by physical SRAM bank"),
      ADD_STAT(bankUtilization, statistics::units::Ratio::get(),
               "Request cycles divided by total drained cycles by bank"),
      ADD_STAT(bankRowConflicts, statistics::units::Count::get(),
               "Distinct same-bank row conflicts across pushed vectors"),
      ADD_STAT(extraCollectCycles, statistics::units::Cycle::get(),
               "COLLECT request cycles added by bank row conflicts"),
      ADD_STAT(fifoAverageOccupancy, statistics::units::Count::get(),
               "Average cycle-start FIFO occupancy through drained"),
      ADD_STAT(fifoPeakOccupancy, statistics::units::Count::get(),
               "Peak cycle-start FIFO occupancy through drained"),
      ADD_STAT(fifoFullStallCycles, statistics::units::Cycle::get(),
               "PUSH cycles stalled by an old full FIFO"),
      ADD_STAT(backpressureCycles, statistics::units::Cycle::get(),
               "Cycles with feed_valid and without feed_ready")
{
    bankRequestCycles.init(ScratchpadBanks);
    bankUtilization.init(ScratchpadBanks);
    for (std::size_t bank = 0; bank < ScratchpadBanks; ++bank) {
        const std::string name = bank < 10 ?
            "b0" + std::to_string(bank) : "b" + std::to_string(bank);
        bankRequestCycles.subname(bank, name);
        bankUtilization.subname(bank, name);
    }
}

} // namespace gem5::sau_n
