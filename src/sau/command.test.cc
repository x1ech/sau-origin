#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <stdexcept>

#include "sau/command.hh"

namespace gem5::sau
{
namespace
{

SauCommand
makeCommand()
{
    return {
        1,
        Operation::Gemm,
        Precision::Int8,
        {0x1000, 4, 32, 0x100, 0x1000},
        {0x2000, 4, 32, 0x100, 0x1000},
        {0x3000, 4, 32, 0, 0x1000},
        1,
        1,
        4,
    };
}

TEST(SauCommand, AcceptsAlignedInt8Gemm)
{
    EXPECT_NO_THROW(validateCommand(makeCommand(), 32));
}

TEST(SauCommand, DefaultsTo32ByteStreamStride)
{
    EXPECT_EQ(StreamDesc{}.strideBytes, 32);
}

TEST(SauCommand, RejectsLegacy16ByteBeatSize)
{
    EXPECT_THROW(validateCommand(makeCommand(), 16), std::invalid_argument);
}

TEST(SauCommand, RejectsZeroBeatStream)
{
    auto command = makeCommand();
    command.operandA.beats = 0;

    EXPECT_THROW(validateCommand(command, 32), std::invalid_argument);
}

TEST(SauCommand, RejectsZeroStreamStride)
{
    auto command = makeCommand();
    command.operandB.strideBytes = 0;

    EXPECT_THROW(validateCommand(command, 32), std::invalid_argument);
}

TEST(SauCommand, RejectsMisalignedAddress)
{
    auto command = makeCommand();
    command.output.base = 0x3004;

    EXPECT_THROW(validateCommand(command, 32), std::invalid_argument);
}

TEST(SauCommand, RejectsZeroFlowLoops)
{
    auto command = makeCommand();
    command.flowLoops = 0;

    EXPECT_THROW(validateCommand(command, 32), std::invalid_argument);
}

TEST(SauCommand, RejectsZeroInstructionLoops)
{
    auto command = makeCommand();
    command.instructionLoops = 0;

    EXPECT_THROW(validateCommand(command, 32), std::invalid_argument);
}

TEST(SauCommand, RejectsZeroWorkItems)
{
    auto command = makeCommand();
    command.workItems = 0;

    EXPECT_THROW(validateCommand(command, 32), std::invalid_argument);
}

TEST(SauCommand, RejectsInconsistentWorkItems)
{
    auto command = makeCommand();
    command.workItems = 3;

    EXPECT_THROW(validateCommand(command, 32), std::invalid_argument);
}

TEST(SauCommand, AcceptsReducedOutputBeatCount)
{
    auto command = makeCommand();
    command.operandA.beats = 4;
    command.flowLoops = 2;
    command.workItems = 8;
    command.output.beats = 4;

    EXPECT_NO_THROW(validateCommand(command, 32));
}

TEST(SauCommand, AcceptsBDrivenWorkItemsWithUnequalStreams)
{
    auto command = makeCommand();
    command.operandA.beats = 1;
    command.operandB.beats = 4;
    command.output.beats = 4;
    command.workItems = 4;

    EXPECT_NO_THROW(validateCommand(command, 32));
}

TEST(SauCommand, AcceptsIndependentSchedulerInstructionExtent)
{
    auto command = makeCommand();
    command.operandB.beats = 8;
    command.flowLoops = 1;
    command.workItems = 8;
    command.output.beats = 8;
    command.scheduleInstructions = 2;

    EXPECT_EQ(effectiveScheduleInstructions(command), 2U);
    EXPECT_NO_THROW(validateCommand(command, 32));
}

TEST(SauCommand, RejectsMismatchedNestedOperandBAddressExtent)
{
    auto command = makeCommand();
    command.operandBAddress = {
        true, 1, 2, 1, 1, 0, 32, 64, 128};

    EXPECT_THROW(validateCommand(command, 32), std::invalid_argument);
}

TEST(SauCommand, RejectsMoreOutputBeatsThanWorkItems)
{
    auto command = makeCommand();
    command.output.beats = 5;

    EXPECT_THROW(validateCommand(command, 32), std::invalid_argument);
}

TEST(SauCommand, RejectsWorkItemOverflow)
{
    auto command = makeCommand();
    command.operandB.beats = std::numeric_limits<uint32_t>::max();
    command.flowLoops = 2;

    EXPECT_THROW(validateCommand(command, 32), std::invalid_argument);
}

TEST(SauCommand, RejectsUnsupportedOperation)
{
    auto command = makeCommand();
    command.operation = static_cast<Operation>(0xff);

    EXPECT_THROW(validateCommand(command, 32), std::invalid_argument);
}

TEST(SauCommand, RejectsUnsupportedPrecision)
{
    auto command = makeCommand();
    command.precision = static_cast<Precision>(0xff);

    EXPECT_THROW(validateCommand(command, 32), std::invalid_argument);
}

} // anonymous namespace
} // namespace gem5::sau
