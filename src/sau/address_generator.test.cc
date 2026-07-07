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

} // anonymous namespace
} // namespace gem5::sau
