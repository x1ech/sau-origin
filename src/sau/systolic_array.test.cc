#include <gtest/gtest.h>

#include "sau/systolic_array.hh"

namespace gem5::sau
{
namespace
{

SystolicArrayInput
uniformInput(int8_t activation, int8_t weight)
{
    SystolicArrayInput input;
    input.activations.lanes.fill(activation);
    input.weights.lanes.fill(weight);
    return input;
}

void
drain(SystolicArray &array)
{
    while (!array.pipelineEmpty()) {
        array.tick();
    }
}

TEST(SystolicArray, CommitsMacroBlocksAlongTheRtlWavefront)
{
    SystolicArray array;
    SystolicArrayInput input;
    for (unsigned lane = 0; lane < BeatLanes; ++lane) {
        input.activations.lanes[lane] = static_cast<int8_t>(lane);
        input.weights.lanes[lane] = 1;
    }

    array.tick(input);
    for (unsigned cycle = 1; cycle < SystolicArray::CalcDelay; ++cycle) {
        array.tick();
    }
    EXPECT_EQ(array.accumulator(0, 0), 0);

    array.tick();
    // The first 4x4 macro block commits at the base CALC_DELAY.
    EXPECT_EQ(array.accumulator(0, 0), 31);
    EXPECT_EQ(array.accumulator(3, 3), 28);
    EXPECT_EQ(array.accumulator(0, 4), 0);
    EXPECT_EQ(array.accumulator(4, 0), 0);

    drain(array);
    EXPECT_EQ(array.accumulator(0, 31), 31);
    EXPECT_EQ(array.accumulator(31, 0), 0);
    EXPECT_EQ(array.committedMacs(),
              uint64_t{SystolicArray::Rows} * SystolicArray::Columns);
}

TEST(SystolicArray, AcceptsOneOuterProductEveryCycle)
{
    SystolicArray array;

    array.tick(uniformInput(2, 3));
    array.tick(uniformInput(-4, 5));
    array.tick(uniformInput(-3, -2));
    EXPECT_EQ(array.acceptedInputs(), uint64_t{3});

    drain(array);
    // 2*3 + (-4)*5 + (-3)*(-2) = -8 in every PE.
    for (unsigned row = 0; row < SystolicArray::Rows; ++row) {
        for (unsigned column = 0;
             column < SystolicArray::Columns; ++column) {
            EXPECT_EQ(array.accumulator(row, column), -8);
        }
    }
    EXPECT_EQ(array.committedMacs(),
              uint64_t{3} * SystolicArray::Rows *
              SystolicArray::Columns);
}

TEST(SystolicArray, AppliesSigned24BitSaturationPerPe)
{
    SystolicArray positive;
    SystolicArray negative;

    for (unsigned input = 0; input < 1024; ++input) {
        positive.tick(uniformInput(127, 127));
        negative.tick(uniformInput(-128, 127));
    }
    drain(positive);
    drain(negative);

    EXPECT_EQ(positive.accumulator(0, 0), Int24Max);
    EXPECT_EQ(positive.accumulator(31, 31), Int24Max);
    EXPECT_EQ(negative.accumulator(0, 0), Int24Min);
    EXPECT_EQ(negative.accumulator(31, 31), Int24Min);
}

TEST(SystolicArray, QuantizesStableRowsAndChecksIndices)
{
    SystolicArray array;
    array.tick(uniformInput(-128, -128));
    drain(array);

    const auto cutbit8 = array.quantizedRow(0, 8);
    for (const int8_t lane : cutbit8.lanes) {
        EXPECT_EQ(lane, 64);
    }
    EXPECT_THROW(array.accumulator(32, 0), std::out_of_range);
    EXPECT_THROW(array.accumulator(0, 32), std::out_of_range);
    EXPECT_THROW(array.quantizedRow(32, 8), std::out_of_range);
}

TEST(SystolicArray, ResetClearsAccumulatorsAndInflightWork)
{
    SystolicArray array;
    array.tick(uniformInput(9, 7));
    EXPECT_FALSE(array.pipelineEmpty());

    array.reset();
    EXPECT_TRUE(array.pipelineEmpty());
    EXPECT_EQ(array.cycle(), uint64_t{0});
    EXPECT_EQ(array.acceptedInputs(), uint64_t{0});
    EXPECT_EQ(array.committedMacs(), uint64_t{0});

    for (unsigned cycle = 0;
         cycle <= SystolicArray::MaxWavefrontDelay; ++cycle) {
        array.tick();
    }
    EXPECT_EQ(array.accumulator(0, 0), 0);
    EXPECT_EQ(array.accumulator(31, 31), 0);
}

TEST(SystolicArray, FinishAndSnapshotPulsesFollowTheMacroStaircase)
{
    SystolicArray array;
    std::vector<std::pair<unsigned, uint8_t>> pePulses;
    std::vector<std::pair<unsigned, uint8_t>> snapshotPulses;

    for (unsigned edge = 0;
         edge <= 32 + SystolicArray::MaxWavefrontDelay; ++edge) {
        std::optional<SystolicArrayInput> input;
        if (edge < 32) {
            input = uniformInput(1, 1);
            input->finish = edge == 31;
        }
        array.tick(input);
        if (array.peFinishPulses()) {
            pePulses.emplace_back(edge, array.peFinishPulses());
        }
        if (array.snapshotReadyPulses()) {
            snapshotPulses.emplace_back(
                edge, array.snapshotReadyPulses());
        }
    }

    ASSERT_EQ(pePulses.size(), SystolicArray::MacroRows);
    ASSERT_EQ(snapshotPulses.size(), SystolicArray::MacroRows);
    for (unsigned macroRow = 0;
         macroRow < SystolicArray::MacroRows; ++macroRow) {
        EXPECT_EQ(pePulses[macroRow].first, 34 + macroRow);
        EXPECT_EQ(pePulses[macroRow].second,
                  uint8_t{1} << macroRow);
        EXPECT_EQ(snapshotPulses[macroRow].first, 41 + macroRow);
        EXPECT_EQ(snapshotPulses[macroRow].second,
                  uint8_t{1} << macroRow);
    }
}

TEST(SystolicArray, StreamsThirtyTwoSnapshotRowsThroughTheTokenChain)
{
    SystolicArray array;
    std::vector<std::pair<unsigned, unsigned>> rows;

    for (unsigned edge = 0; edge <= 76; ++edge) {
        std::optional<SystolicArrayInput> input;
        if (edge < 32) {
            input = uniformInput(1, 1);
            input->finish = edge == 31;
        }
        if (edge == 44) {
            array.requestOutput(0);
        }
        array.tick(input);
        if (array.streamOutput()) {
            ASSERT_TRUE(array.streamRow());
            rows.emplace_back(edge, *array.streamRow());
            for (const int8_t lane : array.streamOutput()->lanes) {
                EXPECT_EQ(lane, 32);
            }
        }
        EXPECT_EQ(array.calFinish(), edge == 75);
        if (edge == 42) {
            EXPECT_TRUE(array.storageReady());
        }
        if (edge == 76) {
            EXPECT_FALSE(array.storageReady());
        }
    }

    ASSERT_EQ(rows.size(), SystolicArray::Rows);
    for (unsigned row = 0; row < SystolicArray::Rows; ++row) {
        EXPECT_EQ(rows[row].first, 44 + row);
        EXPECT_EQ(rows[row].second, row);
    }
}

} // anonymous namespace
} // namespace gem5::sau
