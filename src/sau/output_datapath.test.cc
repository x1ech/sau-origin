#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "sau/output_datapath.hh"

namespace gem5::sau
{
namespace
{

SauOutputResourceConfig
singleResultConfig()
{
    SauOutputResourceConfig config;
    config.internalXBurst = 1;
    config.internalYBurst = 1;
    config.internalFlowBurst = 1;
    config.internalInstructionBurst = 1;
    return config;
}

OutputVector32x16
uniformRow(int16_t value)
{
    OutputVector32x16 row;
    row.lanes.fill(value);
    return row;
}

OperandVector32x8
patternedArrayRow(unsigned row)
{
    OperandVector32x8 result;
    for (unsigned lane = 0; lane < BeatLanes; ++lane) {
        result.lanes[lane] = static_cast<int8_t>(static_cast<int>(row) - lane);
    }
    return result;
}

OutputVector32x16
rawByteRow(uint8_t value)
{
    return uniformRow(static_cast<int8_t>(value));
}

TEST(ResultSerializer, DrainsNormalModesInArrayRowOrder)
{
    ResultSerializer serializer(singleResultConfig());

    EXPECT_THROW(serializer.take(), std::logic_error);
    for (unsigned row = 0; row < BeatLanes; ++row) {
        serializer.accept(patternedArrayRow(row));
    }
    EXPECT_TRUE(serializer.outputReady());
    EXPECT_THROW(serializer.accept(patternedArrayRow(0)), std::logic_error);

    for (unsigned row = 0; row < BeatLanes; ++row) {
        const auto output = serializer.take();
        for (unsigned lane = 0; lane < BeatLanes; ++lane) {
            EXPECT_EQ(output.data.lanes[lane],
                      static_cast<int16_t>(static_cast<int>(row) - lane));
        }
        EXPECT_EQ(output.last, row == BeatLanes - 1);
    }
    EXPECT_TRUE(serializer.canAccept());
}

TEST(ResultSerializer, DrainsTransposedModesInReversedColumnOrder)
{
    auto config = singleResultConfig();
    config.transposedOrder = true;
    ResultSerializer serializer(config);

    for (unsigned row = 0; row < BeatLanes; ++row) {
        serializer.accept(patternedArrayRow(row));
    }
    for (unsigned column = 0; column < BeatLanes; ++column) {
        const auto output = serializer.take();
        for (unsigned lane = 0; lane < BeatLanes; ++lane) {
            const int expected =
                static_cast<int>(BeatLanes - 1 - lane) - column;
            EXPECT_EQ(output.data.lanes[lane], expected);
        }
    }
}

TEST(ResultSerializer, ResetDropsPartialPayloadAndControlState)
{
    ResultSerializer serializer(singleResultConfig());
    serializer.accept(patternedArrayRow(7));
    ASSERT_EQ(serializer.rowsAccepted(), 1U);

    serializer.reset();
    EXPECT_EQ(serializer.rowsAccepted(), 0U);
    EXPECT_EQ(serializer.outputsTaken(), 0U);
    EXPECT_TRUE(serializer.canAccept());
    EXPECT_THROW(serializer.peek(), std::logic_error);
}

TEST(OutputRegisterFile, SelectsBothSramHalvesFromTheRawPointer)
{
    auto config = singleResultConfig();
    config.internalXStep = 128;
    config.internalXBurst = 2;
    OutputRegisterFile file(config);

    const auto low = file.accept(uniformRow(200));
    const auto high = file.accept(uniformRow(-129));
    EXPECT_EQ(low.address, 0);
    EXPECT_FALSE(low.last);
    EXPECT_EQ(high.address, 128);
    EXPECT_TRUE(high.last);
    for (unsigned lane = 0; lane < BeatLanes; ++lane) {
        EXPECT_EQ(file.read(0).bytes[lane], uint8_t{0x7f});
        EXPECT_EQ(file.read(128).bytes[lane], uint8_t{0x80});
    }
    EXPECT_TRUE(file.resultAccumDone());
}

TEST(OutputRegisterFile, RetainUsesForwardingAndSigned16Wrap)
{
    auto config = singleResultConfig();
    config.accumulateExisting = true;
    config.internalXBurst = 2;
    OutputRegisterFile file(config);

    file.accept(uniformRow(127));
    const auto forwarded = file.accept(uniformRow(32767));
    EXPECT_EQ(forwarded.address, 0);
    // signed16(32767 + 127) wraps negative before int8 saturation.
    for (unsigned lane = 0; lane < BeatLanes; ++lane) {
        EXPECT_EQ(forwarded.data.bytes[lane], uint8_t{0x80});
        EXPECT_EQ(file.read(0).bytes[lane], uint8_t{0x80});
    }
}

TEST(OutputRegisterFile, NormalModeOverwritesStaleSameAddressData)
{
    auto config = singleResultConfig();
    config.internalXBurst = 2;
    OutputRegisterFile file(config);

    file.accept(uniformRow(100));
    file.accept(uniformRow(-5));
    for (unsigned lane = 0; lane < BeatLanes; ++lane) {
        EXPECT_EQ(file.read(0).bytes[lane], uint8_t{0xfb});
    }
}

TEST(OutputRegisterFile, WalksRawNestedAddressesAndReturnsToZero)
{
    auto config = singleResultConfig();
    config.internalXStep = 1;
    config.internalXBurst = 2;
    config.internalYStep = 8;
    config.internalYBurst = 2;
    config.internalFlowStep = 32;
    config.internalFlowBurst = 2;
    config.internalInstructionStep = 64;
    config.internalInstructionBurst = 2;
    OutputRegisterFile file(config);

    const std::array<uint8_t, 16> expected = {
        0, 1, 8, 9, 32, 33, 40, 41,
        64, 65, 72, 73, 96, 97, 104, 105
    };
    for (unsigned index = 0; index < expected.size(); ++index) {
        const auto update = file.accept(uniformRow(index));
        EXPECT_EQ(update.address, expected[index]);
        EXPECT_EQ(update.last, index + 1 == expected.size());
    }
    EXPECT_EQ(file.currentAddress(), 0);
    EXPECT_EQ(file.acceptedResults(), expected.size());
}

TEST(OutputRegisterFile, CompletionClearPreservesStoredPayload)
{
    OutputRegisterFile file(singleResultConfig());
    file.accept(uniformRow(19));
    ASSERT_TRUE(file.resultAccumDone());

    file.clearCompletion();
    EXPECT_FALSE(file.resultAccumDone());
    EXPECT_EQ(file.read(0).bytes[0], uint8_t{19});

    file.reset();
    EXPECT_EQ(file.acceptedResults(), uint64_t{0});
    EXPECT_EQ(file.read(0).bytes[0], uint8_t{0});
}

TEST(OutputRegisterFile, RejectsZeroCounterDimensions)
{
    auto config = singleResultConfig();
    config.internalFlowBurst = 0;
    EXPECT_THROW((OutputRegisterFile{config}), std::invalid_argument);
}

TEST(OutputRegisterFile, UnloadsBothHalvesThroughRegisteredWritePipeline)
{
    auto config = singleResultConfig();
    config.internalXStep = 1;
    config.internalXBurst = 64;
    config.internalYStep = 64;
    config.internalYBurst = 4;
    config.registerXBurst = 8;
    config.registerYStep = 8;
    config.registerYCycle = 32;
    config.registerCStep = 1;
    config.registerCCycle = 1;
    OutputRegisterFile file(config);

    for (unsigned address = 0; address < OutputRegisterFile::Depth;
         ++address) {
        file.accept(rawByteRow(static_cast<uint8_t>(address)));
    }
    ASSERT_TRUE(file.resultAccumDone());

    constexpr Addr BaseAddress = 0x29120c00;
    file.startUnload(BaseAddress);
    std::vector<OutputRegisterUnload> outputs;
    unsigned firstOutputTick = 0;
    unsigned lastOutputTick = 0;
    for (unsigned tick = 1; tick <= 300; ++tick) {
        const auto output = file.tickUnload();
        if (!output) {
            continue;
        }
        if (firstOutputTick == 0) {
            firstOutputTick = tick;
        }
        if (output->last) {
            lastOutputTick = tick;
        }
        outputs.push_back(*output);
    }

    ASSERT_EQ(outputs.size(), OutputRegisterFile::Depth);
    EXPECT_EQ(firstOutputTick, 4U);
    EXPECT_EQ(lastOutputTick, 259U);
    for (unsigned index = 0; index < outputs.size(); ++index) {
        EXPECT_EQ(outputs[index].logicalAddress, static_cast<uint8_t>(index));
        EXPECT_EQ(outputs[index].address,
                  BaseAddress + static_cast<Addr>(index) * BeatBytes);
        EXPECT_EQ(outputs[index].data.bytes[0], static_cast<uint8_t>(index));
        EXPECT_EQ(outputs[index].last, index + 1 == OutputRegisterFile::Depth);
    }
    EXPECT_TRUE(file.unloadDone());
    EXPECT_FALSE(file.unloading());
}

TEST(OutputRegisterFile, EnforcesAccumulationAndUnloadPhaseOwnership)
{
    auto config = singleResultConfig();
    config.registerXBurst = 1;
    config.registerYStep = 1;
    config.registerYCycle = 1;
    config.registerCStep = 1;
    config.registerCCycle = 1;
    OutputRegisterFile file(config);

    EXPECT_THROW(file.startUnload(0x1000), std::logic_error);
    file.accept(uniformRow(3));
    file.startUnload(0x1000);
    EXPECT_THROW(file.accept(uniformRow(4)), std::logic_error);
    EXPECT_THROW(file.clearCompletion(), std::logic_error);

    std::optional<OutputRegisterUnload> output;
    for (unsigned tick = 0; tick < 8 && !output; ++tick) {
        output = file.tickUnload();
    }
    ASSERT_TRUE(output);
    ASSERT_TRUE(output->last);
    file.clearCompletion();
    EXPECT_FALSE(file.resultAccumDone());
    EXPECT_FALSE(file.unloadDone());
    EXPECT_EQ(file.read(0).bytes[0], uint8_t{3});
}

} // anonymous namespace
} // namespace gem5::sau
