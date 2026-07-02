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
        {0x1000, 4, 16, 0x100, 0x1000},
        {0x2000, 4, 16, 0x100, 0x1000},
        {0x3000, 4, 16, 0, 0x1000},
        1,
        1,
        4,
    };
}

TEST(SauCommand, AcceptsAlignedInt8Gemm)
{
    EXPECT_NO_THROW(validateCommand(makeCommand(), 16));
}

TEST(SauCommand, RejectsUnsupportedBeatSize)
{
    EXPECT_THROW(validateCommand(makeCommand(), 32), std::invalid_argument);
}

TEST(SauCommand, RejectsZeroBeatStream)
{
    auto command = makeCommand();
    command.operandA.beats = 0;

    EXPECT_THROW(validateCommand(command, 16), std::invalid_argument);
}

TEST(SauCommand, RejectsZeroStreamStride)
{
    auto command = makeCommand();
    command.operandB.strideBytes = 0;

    EXPECT_THROW(validateCommand(command, 16), std::invalid_argument);
}

TEST(SauCommand, RejectsMisalignedAddress)
{
    auto command = makeCommand();
    command.output.base = 0x3004;

    EXPECT_THROW(validateCommand(command, 16), std::invalid_argument);
}

TEST(SauCommand, RejectsZeroFlowLoops)
{
    auto command = makeCommand();
    command.flowLoops = 0;

    EXPECT_THROW(validateCommand(command, 16), std::invalid_argument);
}

TEST(SauCommand, RejectsZeroInstructionLoops)
{
    auto command = makeCommand();
    command.instructionLoops = 0;

    EXPECT_THROW(validateCommand(command, 16), std::invalid_argument);
}

TEST(SauCommand, RejectsZeroWorkItems)
{
    auto command = makeCommand();
    command.workItems = 0;

    EXPECT_THROW(validateCommand(command, 16), std::invalid_argument);
}

TEST(SauCommand, RejectsInconsistentWorkItems)
{
    auto command = makeCommand();
    command.workItems = 3;

    EXPECT_THROW(validateCommand(command, 16), std::invalid_argument);
}

TEST(SauCommand, RejectsInconsistentOutputBeats)
{
    auto command = makeCommand();
    command.output.beats = 3;

    EXPECT_THROW(validateCommand(command, 16), std::invalid_argument);
}

TEST(SauCommand, RejectsWorkItemOverflow)
{
    auto command = makeCommand();
    command.operandA.beats = std::numeric_limits<uint32_t>::max();
    command.flowLoops = 2;

    EXPECT_THROW(validateCommand(command, 16), std::invalid_argument);
}

TEST(SauCommand, RejectsUnsupportedOperation)
{
    auto command = makeCommand();
    command.operation = static_cast<Operation>(0xff);

    EXPECT_THROW(validateCommand(command, 16), std::invalid_argument);
}

TEST(SauCommand, RejectsUnsupportedPrecision)
{
    auto command = makeCommand();
    command.precision = static_cast<Precision>(0xff);

    EXPECT_THROW(validateCommand(command, 16), std::invalid_argument);
}

} // anonymous namespace
} // namespace gem5::sau
