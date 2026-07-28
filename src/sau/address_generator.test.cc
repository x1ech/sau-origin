#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "sau/address_generator.hh"

namespace gem5::sau
{
namespace
{

using testing::ElementsAre;

SauCommand
makeCommand(uint32_t flowLoops = 1, uint32_t instructionLoops = 1)
{
    return {
        1,
        Operation::Gemm,
        Precision::Int8,
        {0x1000, 4, 32, 0x100, 0x1000},
        {0x2000, 4, 32, 0x100, 0x1000},
        {0x3000, 4, 32, 0, 0x1000},
        flowLoops,
        instructionLoops,
        4 * flowLoops * instructionLoops,
    };
}

std::vector<Beat>
collect(AddressGenerator &generator)
{
    std::vector<Beat> beats;
    while (!generator.empty()) {
        beats.push_back(generator.front());
        generator.pop();
    }
    return beats;
}

TEST(AddressGenerator, PreloadsAThenStreamsB)
{
    const SauCommand command = makeCommand();
    AddressGenerator generator(command);

    EXPECT_EQ(generator.totalReadBeats(), 8);
    EXPECT_THAT(collect(generator), ElementsAre(
        Beat{StreamKind::OperandA, 0x1000, 0, false},
        Beat{StreamKind::OperandA, 0x1020, 1, false},
        Beat{StreamKind::OperandA, 0x1040, 2, false},
        Beat{StreamKind::OperandA, 0x1060, 3, true},
        Beat{StreamKind::OperandB, 0x2000, 0, false},
        Beat{StreamKind::OperandB, 0x2020, 1, false},
        Beat{StreamKind::OperandB, 0x2040, 2, false},
        Beat{StreamKind::OperandB, 0x2060, 3, true}));
    EXPECT_TRUE(generator.empty());
}

TEST(AddressGenerator, PreloadsAOnceThenStreamsBForEachFlow)
{
    const SauCommand command = makeCommand(2);
    AddressGenerator generator(command);

    EXPECT_EQ(generator.totalReadBeats(), 12);
    EXPECT_THAT(collect(generator), ElementsAre(
        Beat{StreamKind::OperandA, 0x1000, 0, false},
        Beat{StreamKind::OperandA, 0x1020, 1, false},
        Beat{StreamKind::OperandA, 0x1040, 2, false},
        Beat{StreamKind::OperandA, 0x1060, 3, true},
        Beat{StreamKind::OperandB, 0x2000, 0, false},
        Beat{StreamKind::OperandB, 0x2020, 1, false},
        Beat{StreamKind::OperandB, 0x2040, 2, false},
        Beat{StreamKind::OperandB, 0x2060, 3, true},
        Beat{StreamKind::OperandB, 0x2100, 0, false},
        Beat{StreamKind::OperandB, 0x2120, 1, false},
        Beat{StreamKind::OperandB, 0x2140, 2, false},
        Beat{StreamKind::OperandB, 0x2160, 3, true}));
}

TEST(AddressGenerator, AppliesInstructionFlowAndBeatStrides)
{
    const SauCommand command = makeCommand(2, 2);
    AddressGenerator generator(command);
    const auto beats = collect(generator);

    ASSERT_EQ(beats.size(), 24);
    EXPECT_EQ(generator.totalReadBeats(), beats.size());

    size_t index = 0;
    for (uint32_t instruction = 0; instruction < 2; ++instruction) {
        for (uint32_t beat = 0; beat < command.operandA.beats; ++beat) {
            const Addr expectedAddress =
                command.operandA.base +
                instruction * command.operandA.instructionStrideBytes +
                beat * command.operandA.strideBytes;
            EXPECT_EQ(beats[index], (Beat{
                StreamKind::OperandA, expectedAddress, beat,
                beat + 1 == command.operandA.beats}));
            ++index;
        }

        for (uint32_t flow = 0; flow < 2; ++flow) {
            for (uint32_t beat = 0; beat < command.operandB.beats; ++beat) {
                const Addr expectedAddress =
                    command.operandB.base +
                    instruction * command.operandB.instructionStrideBytes +
                    flow * command.operandB.flowStrideBytes +
                    beat * command.operandB.strideBytes;
                EXPECT_EQ(beats[index], (Beat{
                    StreamKind::OperandB, expectedAddress, beat,
                    beat + 1 == command.operandB.beats}));
                ++index;
            }
        }
    }
}

TEST(AddressGenerator, AppliesRtlNestedOperandBAddressProgram)
{
    auto command = makeCommand(2, 1);
    command.operandB.beats = 4;
    command.workItems = 8;
    command.operandBAddress = {
        true,
        2, 2, 2, 1,
        0x100, 0x20, 0x400, 0x1000,
    };
    AddressGenerator generator(command);
    const auto beats = collect(generator);

    ASSERT_EQ(beats.size(), 12U);
    EXPECT_EQ(beats[4].address, 0x2000U);
    EXPECT_EQ(beats[5].address, 0x2100U);
    EXPECT_EQ(beats[6].address, 0x2020U);
    EXPECT_EQ(beats[7].address, 0x2120U);
    EXPECT_EQ(beats[8].address, 0x2400U);
    EXPECT_EQ(beats[9].address, 0x2500U);
    EXPECT_EQ(beats[10].address, 0x2420U);
    EXPECT_EQ(beats[11].address, 0x2520U);
}

TEST(AddressGenerator, AppliesRtlNestedResidentAddressProgram)
{
    auto command = makeCommand();
    command.operandA.base = 0x1000;
    command.operandA.beats = 16;
    command.control.horizontalAddress = command.operandA.base;
    command.control.registerInput.xBurst = 8;
    command.control.registerInput.yStep = 16;
    command.control.registerInput.yCycle = 2;
    command.control.registerInput.cCycle = 1;

    AddressGenerator generator(command);
    const auto beats = collect(generator);

    ASSERT_EQ(beats.size(), 20U);
    for (uint32_t x = 0; x < 8; ++x) {
        EXPECT_EQ(beats[x].address, 0x1000U + x * 32);
        EXPECT_EQ(beats[8 + x].address, 0x1200U + x * 32);
    }
    EXPECT_FALSE(beats[14].last);
    EXPECT_TRUE(beats[15].last);
}

} // anonymous namespace
} // namespace gem5::sau
