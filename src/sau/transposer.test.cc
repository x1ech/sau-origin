#include <gtest/gtest.h>

#include <cstdint>

#include "sau/transposer.hh"

namespace gem5::sau
{
namespace
{

MemoryBeat256
syntheticRow(unsigned row)
{
    MemoryBeat256 beat;
    for (unsigned byte = 0; byte < BeatBytes; ++byte) {
        beat.bytes[byte] = static_cast<uint8_t>(row * 7 + byte * 13);
    }
    return beat;
}

void
fillBank(TransposerTinyBank &bank)
{
    for (unsigned row = 0; row < TransposerTinyBank::Rows; ++row) {
        ASSERT_TRUE(bank.inputReady());
        bank.writeRow(syntheticRow(row));
    }
}

TEST(TransposerTinyBank, TransposesWithTheRowReversedLaneMapping)
{
    TransposerTinyBank bank(true, false);
    EXPECT_FALSE(bank.outputReady());
    fillBank(bank);
    EXPECT_FALSE(bank.inputReady());
    ASSERT_TRUE(bank.outputReady());

    for (unsigned columnIndex = 0; columnIndex < TransposerTinyBank::Rows;
         ++columnIndex) {
        const auto output = bank.readOutput();
        EXPECT_EQ(output.last, columnIndex == TransposerTinyBank::Rows - 1);
        for (unsigned lane = 0; lane < BeatLanes; ++lane) {
            EXPECT_EQ(output.data.bytes[lane],
                      syntheticRow(TransposerTinyBank::Rows - 1 - lane)
                          .bytes[columnIndex]);
        }
    }
    // Non-reuse drain closes the output side and reopens the input.
    EXPECT_FALSE(bank.outputReady());
    EXPECT_TRUE(bank.inputReady());
}

TEST(TransposerTinyBank, MatchesTheAbtdGoldenFirstColumn)
{
    // Golden vector from the ABTD boundary package: the 32 accepted
    // trans0_inRow beats (byte 0 of each, accepted at command-relative
    // cycles 43 and 78..108) and the first prefetched trans0_outCol at
    // cycle 109.  The full column must reproduce the golden lane order.
    static const uint8_t rowByte0[TransposerTinyBank::Rows] = {
        0xf1, 0x0b, 0x02, 0x02, 0x33, 0x04, 0xe0, 0xfe,
        0xfb, 0x17, 0xc8, 0x18, 0xb4, 0xd5, 0x29, 0x3b,
        0x12, 0xf5, 0xd1, 0x0b, 0xf4, 0xf5, 0x06, 0x09,
        0x07, 0xfd, 0x10, 0xf2, 0x18, 0x05, 0x11, 0x1e,
    };

    TransposerTinyBank bank(true, false);
    for (unsigned row = 0; row < TransposerTinyBank::Rows; ++row) {
        MemoryBeat256 beat = syntheticRow(row);
        beat.bytes[0] = rowByte0[row];
        bank.writeRow(beat);
    }

    const auto output = bank.readOutput();
    for (unsigned lane = 0; lane < BeatLanes; ++lane) {
        EXPECT_EQ(output.data.bytes[lane],
                  rowByte0[TransposerTinyBank::Rows - 1 - lane])
            << "lane " << lane;
    }
}

TEST(TransposerTinyBank, PassthroughModeKeepsRowOrder)
{
    TransposerTinyBank bank(false, false);
    fillBank(bank);
    for (unsigned row = 0; row < TransposerTinyBank::Rows; ++row) {
        EXPECT_EQ(bank.readOutput().data, syntheticRow(row));
    }
}

TEST(TransposerTinyBank, ReuseKeepsTheOutputSideReadyForReplay)
{
    TransposerTinyBank bank(true, true);
    fillBank(bank);

    for (unsigned pass = 0; pass < 2; ++pass) {
        for (unsigned columnIndex = 0;
             columnIndex < TransposerTinyBank::Rows; ++columnIndex) {
            ASSERT_TRUE(bank.outputReady());
            const auto output = bank.readOutput();
            EXPECT_EQ(output.data.bytes[0],
                      syntheticRow(TransposerTinyBank::Rows - 1)
                          .bytes[columnIndex]);
        }
        // reuse_en holds ready_o after the drain, unlike non-reuse.
        EXPECT_TRUE(bank.outputReady());
    }
}

TEST(TransposerTinyBank, OverflowWriteLatchesErrorUntilTheDrain)
{
    TransposerTinyBank bank(true, false);
    fillBank(bank);
    EXPECT_FALSE(bank.error());

    // The source still stores and advances on a not-ready write; only
    // the error flag records the violation, and it clears when the
    // output side drains.
    bank.writeRow(syntheticRow(40));
    EXPECT_TRUE(bank.error());
    for (unsigned columnIndex = 0; columnIndex < TransposerTinyBank::Rows;
         ++columnIndex) {
        bank.readOutput();
    }
    EXPECT_FALSE(bank.error());
}

TEST(TransposerTinyBank, ClearResetsStorageFlagsAndCounters)
{
    TransposerTinyBank bank(true, false);
    fillBank(bank);
    bank.clear();

    EXPECT_TRUE(bank.inputReady());
    EXPECT_FALSE(bank.outputReady());
    EXPECT_EQ(bank.rowsAccepted(), 0u);

    // clear_i zeroes pe_outL itself, so a refill after clear must not
    // observe stale rows.
    bank.writeRow(syntheticRow(5));
    EXPECT_EQ(bank.rowsAccepted(), 1u);
}

SauTransposeReuseResourceConfig
transposeConfig(uint8_t transMode, uint8_t saFlowMode = 0)
{
    SauControlFields control;
    control.transMode = transMode;
    control.reuseMode = 1;
    control.saFlowMode = saFlowMode;
    return deriveResourceConfigs(control).transposeReuse;
}

TEST(TransposerArbiter, AbtdLoadsBAndFeedsTheAbovePort)
{
    TransposerArbiter arbiter(transposeConfig(2));
    EXPECT_FALSE(arbiter.loadsOperandA());
    EXPECT_TRUE(arbiter.loadsOperandB());

    // ABTD executes with input_switch=10: A direct on the left port,
    // the bank columns (B^T) on the above port.
    const auto routing = saOperandRouting(SauTransMode::ABTD, 0x2);
    EXPECT_FALSE(routing.leftFromTransposer);
    EXPECT_TRUE(routing.aboveFromTransposer);

    for (unsigned row = 0; row < TransposerTinyBank::Rows; ++row) {
        ASSERT_TRUE(arbiter.canAcceptRow());
        EXPECT_EQ(arbiter.acceptRow(syntheticRow(row)), 0u);
    }
    ASSERT_TRUE(arbiter.columnReady());
    arbiter.startOutputPhase();
    for (unsigned columnIndex = 0; columnIndex < TransposerTinyBank::Rows;
         ++columnIndex) {
        const auto output = arbiter.readColumn();
        for (unsigned lane = 0; lane < BeatLanes; ++lane) {
            EXPECT_EQ(output.data.bytes[lane],
                      syntheticRow(TransposerTinyBank::Rows - 1 - lane)
                          .bytes[columnIndex]);
        }
    }
    EXPECT_FALSE(arbiter.columnReady());
}

TEST(TransposerArbiter, AtbdFeedsTheLeftPort)
{
    TransposerArbiter arbiter(transposeConfig(1));
    EXPECT_TRUE(arbiter.loadsOperandA());
    EXPECT_FALSE(arbiter.loadsOperandB());

    const auto routing = saOperandRouting(SauTransMode::ATBD, 0x1);
    EXPECT_TRUE(routing.leftFromTransposer);
    EXPECT_FALSE(routing.aboveFromTransposer);
}

TEST(TransposerArbiter, AbdPureOutputModeAcceptsNoOperandRows)
{
    TransposerArbiter arbiter(transposeConfig(0));
    EXPECT_FALSE(arbiter.canAcceptRow());
    const auto routing = saOperandRouting(SauTransMode::ABD, 0x1);
    EXPECT_FALSE(routing.leftFromTransposer);
    EXPECT_FALSE(routing.aboveFromTransposer);
}

TEST(TransposerArbiter, PingPongsBetweenBanksAcrossFlows)
{
    TransposerArbiter arbiter(transposeConfig(2));

    // Flow 0 fills T0; the drain phase latches it as the output bank.
    for (unsigned row = 0; row < TransposerTinyBank::Rows; ++row) {
        EXPECT_EQ(arbiter.acceptRow(syntheticRow(row)), 0u);
    }
    arbiter.startOutputPhase();
    EXPECT_EQ(arbiter.outputBank(), 0u);

    // Flow 1 rows steer to T1 while T0 drains.  Within one modeled
    // cycle the input steering samples the pre-edge bank ready, so the
    // accept is presented before the same-cycle column read.
    for (unsigned step = 0; step < TransposerTinyBank::Rows; ++step) {
        ASSERT_TRUE(arbiter.canAcceptRow());
        EXPECT_EQ(arbiter.acceptRow(syntheticRow(step + 64)), 1u);
        arbiter.readColumn();
    }
    // T0 drained and reopened; the exclusively ready T1 takes over.
    ASSERT_TRUE(arbiter.columnReady());
    EXPECT_EQ(arbiter.outputBank(), 1u);
    for (unsigned columnIndex = 0; columnIndex < TransposerTinyBank::Rows;
         ++columnIndex) {
        const auto output = arbiter.readColumn();
        for (unsigned lane = 0; lane < BeatLanes; ++lane) {
            EXPECT_EQ(output.data.bytes[lane],
                      syntheticRow(TransposerTinyBank::Rows - 1 - lane + 64)
                          .bytes[columnIndex]);
        }
    }
}

TEST(TransposerArbiter, RetainModeSkipsTheCommandStartClear)
{
    TransposerArbiter retaining(transposeConfig(2, 2));
    for (unsigned row = 0; row < TransposerTinyBank::Rows; ++row) {
        retaining.acceptRow(syntheticRow(row));
    }
    retaining.clearOnStart();
    EXPECT_TRUE(retaining.columnReady());

    TransposerArbiter clearing(transposeConfig(2, 0));
    for (unsigned row = 0; row < TransposerTinyBank::Rows; ++row) {
        clearing.acceptRow(syntheticRow(row));
    }
    clearing.clearOnStart();
    EXPECT_FALSE(clearing.columnReady());
    EXPECT_TRUE(clearing.canAcceptRow());
}

} // anonymous namespace
} // namespace gem5::sau
