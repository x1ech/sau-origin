#include <gtest/gtest.h>

#include "sau/payload_datapath.hh"

namespace gem5::sau
{
namespace
{

// The small-fixture ATBD raw control block: trans=01 loads operand A
// into the banks, reuse=01 replays the register-file readout, one flow
// and one instruction over the 32x32x32 shape.
SauControlFields
atbdSmallControl()
{
    SauControlFields control;
    control.transMode = 1;
    control.reuseMode = 1;
    control.saFlowMode = 0;
    control.registerInput.xBurst = 1;
    control.registerInput.yStep = 1;
    control.registerInput.yCycle = 32;
    control.registerInput.cCycle = 1;
    control.input.xStep = 1;
    control.input.xBurst = 1;
    control.input.yStep = 1;
    control.input.yBurst = 32;
    control.input.flowStep = 1;
    control.input.flowBurst = 1;
    control.input.instructionStep = 1;
    control.input.instructionBurst = 1;
    return control;
}

MemoryBeat256
patternBeat(unsigned index)
{
    MemoryBeat256 beat;
    for (unsigned lane = 0; lane < BeatBytes; ++lane) {
        beat.bytes[lane] = static_cast<uint8_t>(index * 7 + lane);
    }
    return beat;
}

TEST(StrictPayloadDatapath, MovesAtbdPayloadsAlongDriverEdges)
{
    StrictPayloadDatapath datapath(
        deriveResourceConfigs(atbdSmallControl()));

    // 32 resident beats become register-file payload storage.
    for (unsigned beat = 0; beat < 32; ++beat) {
        datapath.onMemoryDataVisible(10 + beat, patternBeat(beat), false);
    }
    EXPECT_EQ(datapath.residentBeats(), 32u);
    EXPECT_EQ(datapath.registerFileState().read(5), patternBeat(5));

    // The readout program replays for reuse-A: 64 read valids, 64
    // operand-A edges filling both banks in T0-then-T1 order.
    for (unsigned read = 0; read < 64; ++read) {
        datapath.onRegisterFileReadValid(50 + read);
        datapath.onOperandAValid(52 + read);
        datapath.sampleCycle();
    }
    EXPECT_EQ(datapath.operandATokens(), 64u);
    EXPECT_EQ(datapath.transposerInputRows(), 64u);
    EXPECT_EQ(datapath.transposerInputStalls(), 0u);
    EXPECT_EQ(datapath.payloadUnderflows(), 0u);
    EXPECT_EQ(datapath.firstRowEdge(), 52u);
    EXPECT_EQ(datapath.transposerMaxOccupancy(), 64u);
    EXPECT_GE(datapath.transposerBusyCycles(), 32u);

    // 32 SA-enable edges drain the first bank's transposed columns.
    for (unsigned column = 0; column < 32; ++column) {
        datapath.onSaEnable(120 + column);
    }
    EXPECT_EQ(datapath.transposerOutputColumns(), 32u);
    EXPECT_EQ(datapath.transposerOutputStalls(), 0u);
    EXPECT_EQ(datapath.firstColumnEdge(), 120u);
    // The last consumed column: lane a takes row (31-a), byte 31.
    for (unsigned lane = 0; lane < BeatBytes; ++lane) {
        EXPECT_EQ(datapath.lastColumn().bytes[lane],
                  patternBeat(31 - lane).bytes[31]);
    }
}

TEST(StrictPayloadDatapath, KeepsOperandBOutsideTheAtbdBanks)
{
    StrictPayloadDatapath datapath(
        deriveResourceConfigs(atbdSmallControl()));

    for (unsigned beat = 0; beat < 4; ++beat) {
        datapath.onMemoryDataVisible(20 + beat, patternBeat(beat), true);
        datapath.onOperandBValid(24 + beat);
    }
    EXPECT_EQ(datapath.streamedBeats(), 4u);
    EXPECT_EQ(datapath.operandBTokens(), 4u);
    EXPECT_EQ(datapath.transposerInputRows(), 0u);
    EXPECT_EQ(datapath.payloadUnderflows(), 0u);
}

TEST(StrictPayloadDatapath, CountsPulsePayloadDivergence)
{
    StrictPayloadDatapath datapath(
        deriveResourceConfigs(atbdSmallControl()));

    // An operand edge without a queued payload and an SA edge without
    // a drainable column are counted, not fatal.
    datapath.onOperandAValid(5);
    datapath.onOperandBValid(6);
    datapath.onSaEnable(7);
    EXPECT_EQ(datapath.payloadUnderflows(), 2u);
    EXPECT_EQ(datapath.transposerOutputStalls(), 1u);
    EXPECT_EQ(datapath.transposerInputRows(), 0u);
}

} // anonymous namespace
} // namespace gem5::sau
