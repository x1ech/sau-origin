#include <gtest/gtest.h>

#include <stdexcept>
#include <vector>

#include "sau/command.hh"
#include "sau/csr_config.hh"

namespace gem5::sau
{
namespace
{

SauCsrWrite
write(uint64_t cycle, uint16_t address, uint64_t data, bool accepted = true,
      uint8_t operation = 1)
{
    return {cycle, address, operation, data, accepted};
}

uint64_t
field(uint64_t value, unsigned low)
{
    return value << low;
}

std::vector<SauCsrWrite>
smallFixtureWrites()
{
    return {
        write(29079, 0x200, 0x2912040029120000),
        write(29095, 0x202, 0x0008004101000000),
        write(29100, 0x204, 0x0040010108004100),
        write(29103, 0x206, 0x0000000000140011),
        write(29106, 0x208, 0x0101012008004100),
        write(29109, 0x20a, 0x0104180101010100),
        write(29112, 0x20c, 0x29120c0080180041),
    };
}

std::vector<SauCsrWrite>
baselineFixtureWrites()
{
    return {
        write(28881, 0x200, 0x2912400029120000),
        write(28897, 0x202, 0x0008020801000000),
        write(28902, 0x204, 0x0200080108020100),
        write(28905, 0x206, 0x0000000000868011),
        write(28908, 0x208, 0x0801082008020100),
        write(28911, 0x20a, 0x0120180101010800),
        write(28914, 0x20c, 0x2913800080180208),
    };
}

std::vector<SauCsrWrite>
kSweepFixtureWrites()
{
    return {
        write(28881, 0x200, 0x2912080029120000),
        write(28897, 0x202, 0x0008004101000000),
        write(28902, 0x204, 0x0200010108004100),
        write(28905, 0x206, 0x0000000000140011),
        write(28908, 0x208, 0x0801012008020100),
        write(28911, 0x20a, 0x0120180101010800),
        write(28914, 0x20c, 0x2912680080180208),
    };
}

std::vector<SauCsrWrite>
nSweepFixtureWrites()
{
    return {
        write(28881, 0x200, 0x2912400029120000),
        write(28897, 0x202, 0x0008020801000000),
        write(28902, 0x204, 0x0040080108020100),
        write(28905, 0x206, 0x0000000000868011),
        write(28908, 0x208, 0x0101082008004100),
        write(28911, 0x20a, 0x0104180101010100),
        write(28914, 0x20c, 0x2912680080180041),
    };
}

TEST(SauCsrConfig, ReplaysTheSmallCoverageFixture)
{
    const auto commands = replayCsrWrites(smallFixtureWrites());

    ASSERT_EQ(commands.size(), 1);
    EXPECT_EQ(commands[0].startCycle, 29112);
    const auto &command = commands[0].decoded.command;
    EXPECT_EQ(command.id, 1);
    EXPECT_EQ(command.operandA.base, 0x29120000);
    EXPECT_EQ(command.operandA.beats, 32);
    EXPECT_EQ(command.operandB.base, 0x29120400);
    EXPECT_EQ(command.operandB.beats, 32);
    EXPECT_EQ(command.operandB.strideBytes, 32);
    EXPECT_EQ(command.operandB.flowStrideBytes, 0);
    EXPECT_EQ(command.output.base, 0x29120c00);
    EXPECT_EQ(command.output.beats, 32);
    EXPECT_EQ(command.flowLoops, 1);
    EXPECT_EQ(command.instructionLoops, 1);
    EXPECT_EQ(command.workItems, 32);
    EXPECT_EQ(command.scheduleInstructions, 1);
    EXPECT_TRUE(command.operandBAddress.enabled);
    EXPECT_EQ(command.operandBAddress.xCount, 1);
    EXPECT_EQ(command.operandBAddress.yCount, 32);
    EXPECT_EQ(command.operandBAddress.yStepBytes, 32);
    EXPECT_NO_THROW(validateCommand(command, 32));

    const auto &policy = commands[0].decoded.timingPolicy;
    EXPECT_EQ(policy.transMode, 1);
    EXPECT_EQ(policy.reuseMode, 1);
    EXPECT_EQ(policy.residentLoadBeats, 32);
    EXPECT_EQ(policy.inputBeatsPerInstruction, 32);
    EXPECT_EQ(policy.outputBeats, 32);
    EXPECT_EQ(policy.arrayInputABeats, 64);
    EXPECT_EQ(policy.arrayInputBBeats, 32);
}

TEST(SauCsrConfig, DecodesAllCsrFieldsUsingTheRtlLayout)
{
    SauCsrConfig config;
    for (const auto &entry : smallFixtureWrites()) {
        config.apply(entry);
    }

    EXPECT_EQ(config.verticalAddress, 0x29120400);
    EXPECT_EQ(config.horizontalAddress, 0x29120000);
    EXPECT_EQ(config.outputAddress, 0x29120c00);
    EXPECT_EQ(config.flowLoopTimes, 1);
    EXPECT_EQ(config.transMode, 1);
    EXPECT_EQ(config.reuseMode, 1);
    EXPECT_EQ(config.cutbit, 8);
    EXPECT_EQ(config.input.xBurst, 1);
    EXPECT_EQ(config.input.yStep, 1);
    EXPECT_EQ(config.input.yBurst, 32);
    EXPECT_EQ(config.input.flowStep, 1);
    EXPECT_EQ(config.input.instructionBurst, 1);
    EXPECT_EQ(config.vertical.yCycle, 32);
    EXPECT_EQ(config.vertical.flowCycle, 1);
    EXPECT_EQ(config.registerInput.xBurst, 1);
    EXPECT_EQ(config.registerInput.yCycle, 32);
    EXPECT_EQ(config.output.xBurst, 1);
    EXPECT_EQ(config.output.yBurst, 32);
    EXPECT_EQ(config.output.instructionBurst, 1);
    EXPECT_EQ(config.output.registerXBurst, 1);
    EXPECT_EQ(config.output.registerYCycle, 32);
}

TEST(SauCsrConfig, AppliesEveryWritableFieldFromTheRtlRegisterLayout)
{
    SauCsrConfig config;
    EXPECT_FALSE(config.apply(write(1, 0x200, 0xaabbccdd11223344)));
    EXPECT_FALSE(config.apply(write(2, 0x202,
        field(0x3f, 0) | field(0x2a, 6) | field(0x15, 12) |
        field(0x2b, 18) | field(0xcd, 24) | field(0x2d, 32) |
        field(0x9a, 38) | field(0x35, 46) | field(0x67, 52) |
        field(0xb, 60))));
    EXPECT_FALSE(config.apply(write(3, 0x204,
        field(0x12, 0) | field(0x2d, 8) | field(0x34, 14) |
        field(0x2a, 22) | field(0x56, 32) | field(0x23, 40) |
        field(0x78, 46) | field(0x19, 54))));
    EXPECT_FALSE(config.apply(write(4, 0x206,
        field(0x2, 0) | field(0x1, 2) | field(0x3, 4) |
        field(0x2, 6) | field(0x1, 8) | field(0x5, 10) |
        field(0x1, 13) | field(0x1, 14) | field(0x1a, 15) |
        field(0x2b, 20) | field(0x55667788, 32))));
    EXPECT_FALSE(config.apply(write(5, 0x208,
        field(0x12, 0) | field(0x2d, 8) | field(0x34, 14) |
        field(0x2a, 22) | field(0x56, 32) | field(0x78, 40) |
        field(0x9a, 48) | field(0x19, 56))));
    EXPECT_FALSE(config.apply(write(6, 0x20a,
        field(0x12, 0) | field(0x34, 8) | field(0x56, 16) |
        field(0x78, 24) | field(0x2d, 32) | field(0x2a, 38) |
        field(0x23, 44) | field(0x19, 50) | field(0xcd, 56))));
    EXPECT_FALSE(config.apply(write(7, 0x20c,
        field(0x2d, 0) | field(0x9a, 6) | field(0x35, 14) |
        field(0x67, 20) | field(0xaabbccdd, 32), true, 0)));

    EXPECT_EQ(config.verticalAddress, 0xaabbccdd);
    EXPECT_EQ(config.horizontalAddress, 0x11223344);
    EXPECT_EQ(config.biasAddress, 0x55667788);
    EXPECT_EQ(config.flowLoopTimes, 0x2b);
    EXPECT_EQ(config.convKernal, 0x5);
    EXPECT_EQ(config.reuseMode, 0x3);
    EXPECT_EQ(config.transMode, 0x2);
    EXPECT_EQ(config.peWorkMode, 0x2);
    EXPECT_EQ(config.saFlowMode, 0x1);
    EXPECT_EQ(config.registerMode, 0x1);
    EXPECT_TRUE(config.strideFlag);
    EXPECT_TRUE(config.shiftFlag);
    EXPECT_EQ(config.cutbit, 0x1a);
    EXPECT_EQ(config.registerInput.validYStart, 0x3f);
    EXPECT_EQ(config.registerInput.validYEnd, 0x2a);
    EXPECT_EQ(config.registerInput.validXStart, 0x15);
    EXPECT_EQ(config.registerInput.validXEnd, 0x2b);
    EXPECT_EQ(config.registerInput.cCycle, 0xcd);
    EXPECT_EQ(config.registerInput.xBurst, 0x2d);
    EXPECT_EQ(config.registerInput.yStep, 0x9a);
    EXPECT_EQ(config.registerInput.yCycle, 0x35);
    EXPECT_EQ(config.registerInput.cStep, 0x67);
    EXPECT_EQ(config.registerInput.padding, 0xb);
    EXPECT_EQ(config.input.xStep, 0x12);
    EXPECT_EQ(config.input.xBurst, 0x2d);
    EXPECT_EQ(config.input.yStep, 0x34);
    EXPECT_EQ(config.input.yBurst, 0x2a);
    EXPECT_EQ(config.input.flowStep, 0x56);
    EXPECT_EQ(config.input.flowBurst, 0x23);
    EXPECT_EQ(config.input.instructionStep, 0x78);
    EXPECT_EQ(config.input.instructionBurst, 0x19);
    EXPECT_EQ(config.vertical.xStep, 0x12);
    EXPECT_EQ(config.vertical.xBurst, 0x2d);
    EXPECT_EQ(config.vertical.yStep, 0x34);
    EXPECT_EQ(config.vertical.yCycle, 0x2a);
    EXPECT_EQ(config.vertical.flowStep, 0x56);
    EXPECT_EQ(config.vertical.flowCycle, 0x78);
    EXPECT_EQ(config.vertical.instructionStep, 0x9a);
    EXPECT_EQ(config.vertical.instructionCycle, 0x19);
    EXPECT_EQ(config.output.xStep, 0x12);
    EXPECT_EQ(config.output.xBurst, 0x2d);
    EXPECT_EQ(config.output.yStep, 0x34);
    EXPECT_EQ(config.output.yBurst, 0x2a);
    EXPECT_EQ(config.output.flowStep, 0x56);
    EXPECT_EQ(config.output.flowBurst, 0x23);
    EXPECT_EQ(config.output.instructionStep, 0x78);
    EXPECT_EQ(config.output.instructionBurst, 0x19);
    EXPECT_EQ(config.output.registerXBurst, 0x2d);
    EXPECT_EQ(config.output.registerYStep, 0x9a);
    EXPECT_EQ(config.output.registerYCycle, 0x35);
    EXPECT_EQ(config.output.registerCStep, 0x67);
    EXPECT_EQ(config.output.registerCCycle, 0xcd);
    EXPECT_EQ(config.outputAddress, 0xaabbccdd);
}

TEST(SauCsrConfig, RejectedWritesDoNotChangeHeldConfiguration)
{
    SauCsrConfig config;
    EXPECT_FALSE(config.apply(write(1, 0x200, 0x1111000022220000)));
    EXPECT_FALSE(config.apply(write(2, 0x200, 0x9999000088880000, false)));

    EXPECT_EQ(config.verticalAddress, 0x11110000);
    EXPECT_EQ(config.horizontalAddress, 0x22220000);
}

TEST(SauCsrConfig, StartIsAOneCyclePulseAndUsesTheRtlOperation)
{
    SauCsrConfig config;
    EXPECT_FALSE(config.apply(write(1, 0x20c, 0x0000000080000000, true, 0)));
    EXPECT_TRUE(config.apply(write(2, 0x20c, 0x0000000080000000)));
    EXPECT_FALSE(config.apply(write(2, 0x20c, 0x0000000080000000)));
    EXPECT_TRUE(config.apply(write(3, 0x20c, 0x0000000080000000)));
}

TEST(SauCsrConfig, RejectsUnsupportedControlModesWithRawValues)
{
    auto writes = smallFixtureWrites();
    writes[3].data = 0x0000000000140010;

    EXPECT_THROW(replayCsrWrites(writes), std::invalid_argument);
}

TEST(SauCsrConfig, DerivesDifferentCoverageShapesWithoutProfiles)
{
    const auto baseline = replayCsrWrites(baselineFixtureWrites());
    const auto kSweep = replayCsrWrites(kSweepFixtureWrites());
    const auto nSweep = replayCsrWrites(nSweepFixtureWrites());

    ASSERT_EQ(baseline.size(), 1);
    ASSERT_EQ(kSweep.size(), 1);
    ASSERT_EQ(nSweep.size(), 1);
    const auto &baselineCommand = baseline[0].decoded.command;
    const auto &kSweepCommand = kSweep[0].decoded.command;
    const auto &nSweepCommand = nSweep[0].decoded.command;
    EXPECT_EQ(baselineCommand.operandA.beats, 256);
    EXPECT_EQ(baselineCommand.operandB.beats, 256);
    EXPECT_EQ(baselineCommand.flowLoops, 8);
    EXPECT_EQ(baselineCommand.scheduleInstructions, 8);
    EXPECT_EQ(baselineCommand.workItems, 2048);
    EXPECT_EQ(baselineCommand.output.beats, 256);
    EXPECT_EQ(kSweepCommand.operandA.beats, 32);
    EXPECT_EQ(kSweepCommand.operandB.beats, 256);
    EXPECT_EQ(kSweepCommand.flowLoops, 1);
    EXPECT_EQ(kSweepCommand.scheduleInstructions, 8);
    EXPECT_EQ(kSweepCommand.workItems, 256);
    EXPECT_EQ(kSweepCommand.output.beats, 256);
    EXPECT_EQ(kSweepCommand.operandBAddress.yStepBytes, 256U);
    EXPECT_EQ(kSweepCommand.operandBAddress.instructionStepBytes, 32U);
    EXPECT_NO_THROW(validateCommand(kSweepCommand, 32));
    EXPECT_EQ(nSweepCommand.operandA.beats, 256U);
    EXPECT_EQ(nSweepCommand.operandB.beats, 32U);
    EXPECT_EQ(nSweepCommand.flowLoops, 8U);
    EXPECT_EQ(nSweepCommand.scheduleInstructions, 1U);
    EXPECT_EQ(nSweepCommand.workItems, 256U);
    EXPECT_EQ(nSweepCommand.operandBAddress.yStepBytes, 32U);
    EXPECT_EQ(nSweepCommand.operandBAddress.flowStepBytes, 1024U);
    EXPECT_NO_THROW(validateCommand(nSweepCommand, 32));
}

} // anonymous namespace
} // namespace gem5::sau
