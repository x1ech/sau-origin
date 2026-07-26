#include <gtest/gtest.h>

#include <stdexcept>

#include "sau/data_beat.hh"

namespace gem5::sau
{
namespace
{

TEST(DataBeat, OperandLanesFollowAscendingByteAddresses)
{
    MemoryBeat256 beat;
    for (unsigned byte = 0; byte < BeatBytes; ++byte) {
        beat.bytes[byte] = static_cast<uint8_t>(0xe0 + byte);
    }

    const OperandVector32x8 operand = operandFromBeat(beat);
    // 0xe0 reinterprets as signed -32; lane k must be beat byte k.
    EXPECT_EQ(operand.lanes[0], -32);
    EXPECT_EQ(operand.lanes[31], -1);
    EXPECT_EQ(beatFromOperand(operand), beat);
}

TEST(DataBeat, SignExtendInt24CoversBothSignBoundaries)
{
    EXPECT_EQ(signExtendInt24(0x000000u), 0);
    EXPECT_EQ(signExtendInt24(0x7fffffu), Int24Max);
    EXPECT_EQ(signExtendInt24(0x800000u), Int24Min);
    EXPECT_EQ(signExtendInt24(0xffffffu), -1);
    // Bits above 24 must be ignored before sign extension.
    EXPECT_EQ(signExtendInt24(0xff800001u), Int24Min + 1);
}

TEST(DataBeat, SaturateAddInt24ClampsInsteadOfWrapping)
{
    EXPECT_EQ(saturateAddInt24(100, -350), -250);
    EXPECT_EQ(saturateAddInt24(Int24Max, 1), Int24Max);
    EXPECT_EQ(saturateAddInt24(Int24Max, Int24Max), Int24Max);
    EXPECT_EQ(saturateAddInt24(Int24Min, -1), Int24Min);
    EXPECT_EQ(saturateAddInt24(Int24Min, Int24Min), Int24Min);
    EXPECT_EQ(saturateAddInt24(Int24Min, Int24Max), -1);
}

TEST(DataBeat, SatTruncateShiftsArithmeticallyThenSaturates)
{
    EXPECT_EQ(satTruncateInt24(100, 0), 100);
    EXPECT_EQ(satTruncateInt24(-100, 0), -100);
    EXPECT_EQ(satTruncateInt24(128, 0), 127);
    EXPECT_EQ(satTruncateInt24(-129, 0), -128);
    // Arithmetic right shift keeps the sign and rounds toward -inf.
    EXPECT_EQ(satTruncateInt24(0x1234, 8), 0x12);
    EXPECT_EQ(satTruncateInt24(-256, 4), -16);
    EXPECT_EQ(satTruncateInt24(-1, 5), -1);
    EXPECT_EQ(satTruncateInt24(-257, 8), -2);
    EXPECT_EQ(satTruncateInt24(Int24Max, 8), 127);
    EXPECT_EQ(satTruncateInt24(Int24Min, 8), -128);
    // The full raw 5-bit cutbit domain is legal; 31 drains every
    // magnitude bit of the 24-bit accumulator.
    EXPECT_EQ(satTruncateInt24(Int24Max, 31), 0);
    EXPECT_EQ(satTruncateInt24(Int24Min, 31), -1);
    EXPECT_THROW(satTruncateInt24(0, 32), std::invalid_argument);
}

TEST(DataBeat, WrapAddInt16WrapsAtBothExtremes)
{
    EXPECT_EQ(wrapAddInt16(1200, -200), 1000);
    EXPECT_EQ(wrapAddInt16(32767, 1), -32768);
    EXPECT_EQ(wrapAddInt16(-32768, -1), 32767);
    EXPECT_EQ(wrapAddInt16(-32768, -32768), 0);
}

TEST(DataBeat, SatSigned8ClampsOutputLanes)
{
    EXPECT_EQ(satSigned8(5), 5);
    EXPECT_EQ(satSigned8(127), 127);
    EXPECT_EQ(satSigned8(128), 127);
    EXPECT_EQ(satSigned8(-128), -128);
    EXPECT_EQ(satSigned8(-129), -128);
    EXPECT_EQ(satSigned8(32767), 127);
    EXPECT_EQ(satSigned8(-32768), -128);
}

TEST(DataBeat, WriteBeatSaturatesEachLaneIntoAscendingBytes)
{
    OutputVector32x16 output;
    output.lanes[0] = 300;
    output.lanes[1] = -300;
    output.lanes[2] = -1;
    output.lanes[31] = 64;

    const WriteBeat256 beat = writeBeatFromOutput(output);
    EXPECT_EQ(beat.bytes[0], 0x7f);
    EXPECT_EQ(beat.bytes[1], 0x80);
    EXPECT_EQ(beat.bytes[2], 0xff);
    EXPECT_EQ(beat.bytes[3], 0x00);
    EXPECT_EQ(beat.bytes[31], 0x40);
}

} // anonymous namespace
} // namespace gem5::sau
