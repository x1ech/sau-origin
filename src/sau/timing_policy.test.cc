#include <gtest/gtest.h>

#include <string>

#include "sau/timing_policy.hh"

namespace gem5::sau
{
namespace
{

SauCommand
command(uint32_t residentBeats, uint32_t inputBeats, uint32_t outputBeats,
        uint32_t flowLoops)
{
    SauCommand result;
    result.operandA.beats = residentBeats;
    result.operandB.beats = inputBeats;
    result.output.beats = outputBeats;
    result.flowLoops = flowLoops;
    result.instructionLoops = 1;
    result.workItems = inputBeats * flowLoops;
    return result;
}

const TimingLedgerEntry &
entry(const TimingPolicy &policy, const char *term)
{
    for (const auto &candidate : policy.ledger) {
        if (candidate.term == term) {
            return candidate;
        }
    }
    ADD_FAILURE() << "missing ledger term: " << term;
    static const TimingLedgerEntry missing;
    return missing;
}

uint64_t
cycleValue(Cycles value)
{
    return static_cast<uint64_t>(value);
}

TEST(TimingPolicy, DerivesBaselineFromRtlStructureAndCsrExtents)
{
    const auto policy = TimingPolicy::derive(
        command(256, 256, 256, 8), 1, 1, RtlTimingParameters{});

    EXPECT_EQ(policy.storage.beatBytes, 32U);
    EXPECT_EQ(policy.storage.issueWidth, 1U);
    EXPECT_TRUE(policy.storage.sharedReadWritePort);
    EXPECT_TRUE(policy.storage.readPriority);
    EXPECT_TRUE(policy.storage.inOrderResponses);
    EXPECT_EQ(cycleValue(policy.storage.readVisibleLatencyCycles), 4U);
    EXPECT_EQ(cycleValue(policy.commandStartCycles), 3U);
    EXPECT_EQ(cycleValue(policy.arrayInputStartDelayCycles), 269U);
    EXPECT_EQ(policy.arrayInputBurstBeats, 32U);
    EXPECT_EQ(policy.arrayInputABeats, 2048U);
    EXPECT_EQ(policy.arrayInputBBeats, 2048U);
    EXPECT_EQ(policy.scheduleInstructions, 8U);
    EXPECT_FALSE(policy.shortDirectDOutPath);
    EXPECT_FALSE(policy.earlyFinalUnload);
    EXPECT_EQ(cycleValue(policy.arrayInputBurstGapCycles), 1U);
    EXPECT_EQ(cycleValue(policy.arrayInputFlowGapCycles), 3U);
    EXPECT_EQ(policy.arrayInputSkewCycles, 32U);
    EXPECT_EQ(policy.bReadStartAheadBeats, 24U);
    EXPECT_EQ(policy.bStagingBeats, 8U);
    EXPECT_EQ(cycleValue(policy.arrayFillCycles), 343U);
    EXPECT_EQ(cycleValue(policy.inputSwitchVisibleDelayCycles), 12U);
    EXPECT_EQ(cycleValue(policy.inputSwitchResetVisibleDelayCycles), 14U);
    EXPECT_EQ(cycleValue(policy.flowExecuteCycles), 232U);
    EXPECT_EQ(cycleValue(policy.resultFlowGapCycles), 234U);
    EXPECT_EQ(cycleValue(policy.writebackStartDelayCycles), 8U);
    EXPECT_EQ(cycleValue(policy.earlyUnloadWritebackStartDelayCycles), 7U);
    EXPECT_EQ(cycleValue(policy.finalDrainToInputSwitchResetCycles), 46U);
    EXPECT_EQ(cycleValue(policy.completionDelayCycles), 4U);
    EXPECT_EQ(cycleValue(entry(policy, "resident_load").cycles), 256U);
    EXPECT_NE(entry(policy, "array_fill").source.find("CSR"),
              std::string::npos);
    EXPECT_NE(entry(policy, "input_switch_visible").source.find(
                  "STATE_DELAY"), std::string::npos);
    EXPECT_NE(entry(policy, "input_switch_reset_visible").source.find(
                  "REGISTER_UNLOAD"), std::string::npos);
    EXPECT_EQ(cycleValue(entry(policy, "storage_read_visible").cycles), 4U);
    EXPECT_NE(entry(policy, "storage_read_visible").source.find(
                  "SRAM_DELAY + 1"), std::string::npos);
    EXPECT_EQ(cycleValue(entry(policy, "b_staging").cycles), 8U);
}

TEST(TimingPolicy, HandlesSingleFlowAndPartialOutputWithoutFixtureProfile)
{
    const auto policy = TimingPolicy::derive(
        command(32, 32, 32, 1), 1, 1, RtlTimingParameters{});

    EXPECT_EQ(cycleValue(policy.arrayInputStartDelayCycles), 45U);
    EXPECT_EQ(cycleValue(policy.arrayFillCycles), 112U);
    EXPECT_EQ(cycleValue(policy.resultFlowGapCycles), 0U);
    EXPECT_EQ(policy.arrayInputABeats, 64U);
    EXPECT_EQ(policy.arrayInputBBeats, 32U);
    EXPECT_TRUE(policy.shortDirectDOutPath);
    EXPECT_FALSE(policy.earlyFinalUnload);
    EXPECT_EQ(cycleValue(policy.firstArrayInputFlowGapCycles), 17U);
    EXPECT_EQ(cycleValue(policy.secondArrayInputFlowGapCycles), 44U);
    EXPECT_EQ(cycleValue(policy.steadyArrayInputFlowGapCycles), 36U);
    EXPECT_EQ(cycleValue(policy.firstShortExecuteCycles), 33U);
    EXPECT_EQ(cycleValue(policy.steadyShortExecuteCycles), 34U);
    EXPECT_EQ(cycleValue(policy.firstShortDrainCycles), 15U);
    EXPECT_EQ(cycleValue(policy.secondShortDrainCycles), 42U);
    EXPECT_EQ(cycleValue(policy.steadyShortDrainCycles), 34U);
    EXPECT_EQ(cycleValue(entry(policy, "transpose_burst").cycles), 32U);

    const auto partialOutput = TimingPolicy::derive(
        command(256, 32, 32, 8), 1, 1, RtlTimingParameters{});
    EXPECT_EQ(cycleValue(partialOutput.arrayFillCycles), 343U);
    EXPECT_EQ(cycleValue(partialOutput.resultFlowGapCycles), 0U);
}

TEST(TimingPolicy, ChangesOnlyWithNamedRtlOrCsrInputs)
{
    auto rtl = RtlTimingParameters{};
    rtl.saSize = 16;
    rtl.registerDepth = 128;
    rtl.sramDelay = 5;
    const auto policy = TimingPolicy::derive(
        command(64, 64, 128, 2), 1, 1, rtl);

    EXPECT_EQ(cycleValue(policy.storage.readVisibleLatencyCycles), 6U);
    EXPECT_EQ(policy.arrayInputBurstBeats, 16U);
    EXPECT_EQ(policy.arrayInputSkewCycles, 16U);
    EXPECT_EQ(policy.bStagingBeats, 8U);
    EXPECT_EQ(cycleValue(policy.arrayInputFlowGapCycles), 5U);
    EXPECT_EQ(cycleValue(policy.commandStartCycles), 3U);
    EXPECT_EQ(cycleValue(policy.arrayInputStartDelayCycles), 81U);
    EXPECT_EQ(cycleValue(policy.arrayFillCycles), 115U);
    EXPECT_EQ(cycleValue(policy.inputSwitchVisibleDelayCycles), 14U);
    EXPECT_EQ(cycleValue(policy.inputSwitchResetVisibleDelayCycles), 16U);
    EXPECT_EQ(cycleValue(policy.resultFlowGapCycles), 52U);
}

TEST(TimingPolicy, SeparatesSchedulerInstructionAndFlowCounters)
{
    auto kSweep = command(32, 256, 256, 1);
    kSweep.scheduleInstructions = 8;
    const auto kPolicy = TimingPolicy::derive(
        kSweep, 1, 1, RtlTimingParameters{});
    EXPECT_EQ(kPolicy.inputBeatsPerInstruction, 32U);
    EXPECT_EQ(kPolicy.scheduleInstructions, 8U);
    EXPECT_EQ(kPolicy.arrayInputABeats, 288U);
    EXPECT_EQ(kPolicy.arrayInputBBeats, 256U);
    EXPECT_TRUE(kPolicy.shortDirectDOutPath);
    EXPECT_TRUE(kPolicy.earlyFinalUnload);
    EXPECT_EQ(cycleValue(kPolicy.arrayFillCycles), 112U);
    EXPECT_EQ(cycleValue(kPolicy.resultFlowGapCycles), 36U);

    auto nSweep = command(256, 32, 32, 8);
    nSweep.scheduleInstructions = 1;
    const auto nPolicy = TimingPolicy::derive(
        nSweep, 1, 1, RtlTimingParameters{});
    EXPECT_EQ(nPolicy.inputBeatsPerInstruction, 256U);
    EXPECT_EQ(nPolicy.scheduleInstructions, 1U);
    EXPECT_EQ(nPolicy.arrayInputABeats, 256U);
    EXPECT_EQ(nPolicy.arrayInputBBeats, 256U);
    EXPECT_FALSE(nPolicy.shortDirectDOutPath);
    EXPECT_TRUE(nPolicy.earlyFinalUnload);
    EXPECT_EQ(cycleValue(nPolicy.arrayFillCycles), 343U);
    EXPECT_EQ(cycleValue(nPolicy.resultFlowGapCycles), 0U);
}

TEST(TimingPolicy, DerivesFlowExecutionFromTheRtlFlowCounter)
{
    auto kHoldoutShape = command(128, 256, 256, 4);
    kHoldoutShape.scheduleInstructions = 8;

    const auto policy = TimingPolicy::derive(
        kHoldoutShape, 1, 1, RtlTimingParameters{});

    EXPECT_EQ(policy.inputBeatsPerInstruction, 128U);
    EXPECT_EQ(cycleValue(policy.flowExecuteCycles), 100U);
    EXPECT_NE(entry(policy, "flow_execute").source.find("flow_times_i"),
              std::string::npos);
}

TEST(TimingPolicy, RejectsUnsupportedRtlSramWidth)
{
    auto rtl = RtlTimingParameters{};
    rtl.sramDataWidthBits = 128;

    EXPECT_THROW(
        TimingPolicy::derive(command(32, 32, 32, 1), 1, 1, rtl),
        std::invalid_argument);
}

} // anonymous namespace
} // namespace gem5::sau
