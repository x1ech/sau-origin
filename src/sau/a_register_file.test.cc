#include <gtest/gtest.h>

#include "sau/a_register_file.hh"

namespace gem5::sau
{
namespace
{

SauCommand
makeCommand(uint32_t flowLoops = 2, uint32_t instructionLoops = 1)
{
    return {
        1,
        Operation::Gemm,
        Precision::Int8,
        {0x1000, 4, 32, 0x100, 0x1000},
        {0x2000, 4, 32, 0x100, 0x1000},
        {0x3000, 8, 32, 0, 0x1000},
        flowLoops,
        instructionLoops,
        4 * flowLoops * instructionLoops,
    };
}

TEST(ARegisterFileIn, TracksAReadPreloadPerInstruction)
{
    ARegisterFileIn registerFile(makeCommand());

    EXPECT_FALSE(registerFile.instructionReady(0));
    EXPECT_EQ(registerFile.loadedBeats(0), 0);

    registerFile.load(Beat{StreamKind::OperandA, 0x1000, 0, false});
    registerFile.load(Beat{StreamKind::OperandA, 0x1020, 1, false});
    registerFile.load(Beat{StreamKind::OperandA, 0x1040, 2, false});

    EXPECT_FALSE(registerFile.instructionReady(0));
    EXPECT_EQ(registerFile.loadedBeats(0), 3);

    registerFile.load(Beat{StreamKind::OperandA, 0x1060, 3, true});

    EXPECT_TRUE(registerFile.instructionReady(0));
    EXPECT_EQ(registerFile.loadedBeats(0), 4);
}

TEST(ARegisterFileIn, ReportsArrayReuseMoreThanExternalReads)
{
    ARegisterFileIn registerFile(makeCommand(2, 1));

    EXPECT_EQ(registerFile.totalExternalLoadBeats(), 4);
    EXPECT_EQ(registerFile.totalArrayInputBeats(), 8);
}

TEST(ARegisterFileIn, ProducesVirtualAArrayInputBeatsAfterPreload)
{
    ARegisterFileIn registerFile(makeCommand(2, 1));
    registerFile.load(Beat{StreamKind::OperandA, 0x1000, 0, false});
    registerFile.load(Beat{StreamKind::OperandA, 0x1020, 1, false});
    registerFile.load(Beat{StreamKind::OperandA, 0x1040, 2, false});
    registerFile.load(Beat{StreamKind::OperandA, 0x1060, 3, true});

    EXPECT_EQ(
        registerFile.arrayInputBeat(0, 0, 0, 0),
        (Beat{StreamKind::OperandA, 0, 0, false}));
    EXPECT_EQ(
        registerFile.arrayInputBeat(0, 1, 3, 7),
        (Beat{StreamKind::OperandA, 0, 7, true}));
}

TEST(ARegisterFileIn, RejectsNonAExternalLoad)
{
    ARegisterFileIn registerFile(makeCommand());

    EXPECT_THROW(
        registerFile.load(Beat{StreamKind::OperandB, 0x2000, 0, false}),
        std::invalid_argument);
}

} // anonymous namespace
} // namespace gem5::sau
