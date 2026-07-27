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

} // anonymous namespace
} // namespace gem5::sau
