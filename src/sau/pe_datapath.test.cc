#include <gtest/gtest.h>

#include <stdexcept>

#include "sau/pe_datapath.hh"

namespace gem5::sau
{
namespace
{

SystolicPeInputs
mac(int8_t activation, int8_t weight)
{
    return SystolicPeInputs{
        activation, weight, true, true, false
    };
}

TEST(SystolicPe, MultipliesSignedInt8Operands)
{
    SystolicPe pe;

    pe.tick(mac(127, 127));
    EXPECT_EQ(pe.accumulator(), 16129);

    pe.reset();
    pe.tick(mac(-128, 127));
    EXPECT_EQ(pe.accumulator(), -16256);

    pe.reset();
    pe.tick(mac(-128, -128));
    EXPECT_EQ(pe.accumulator(), 16384);
}

TEST(SystolicPe, RequiresBothAlignedEnableSignals)
{
    SystolicPe pe;
    auto inputs = mac(7, -9);

    inputs.columnEnable = false;
    pe.tick(inputs);
    EXPECT_EQ(pe.accumulator(), 0);

    inputs.columnEnable = true;
    inputs.macAddEnable = false;
    pe.tick(inputs);
    EXPECT_EQ(pe.accumulator(), 0);

    inputs.macAddEnable = true;
    pe.tick(inputs);
    EXPECT_EQ(pe.accumulator(), -63);
}

TEST(SystolicPe, AccumulatesContinuouslyAndClearHasPriority)
{
    SystolicPe pe;

    pe.tick(mac(10, 12));
    pe.tick(mac(-3, 7));
    pe.tick(mac(-8, -4));
    EXPECT_EQ(pe.accumulator(), 131);

    auto clearAndMac = mac(100, 100);
    clearAndMac.clear = true;
    pe.tick(clearAndMac);
    EXPECT_EQ(pe.accumulator(), 0);

    pe.tick(mac(2, 5));
    EXPECT_EQ(pe.accumulator(), 10);
}

TEST(SystolicPe, SaturatesAtSigned24BitLimits)
{
    SystolicPe positive;
    SystolicPe negative;

    for (unsigned iteration = 0; iteration < 1024; ++iteration) {
        positive.tick(mac(127, 127));
        negative.tick(mac(-128, 127));
    }

    EXPECT_EQ(positive.accumulator(), Int24Max);
    EXPECT_EQ(negative.accumulator(), Int24Min);

    positive.tick(mac(-1, 1));
    negative.tick(mac(1, 1));
    EXPECT_EQ(positive.accumulator(), Int24Max - 1);
    EXPECT_EQ(negative.accumulator(), Int24Min + 1);
}

TEST(SystolicPe, QuantizesAcrossTheRawCutbitDomain)
{
    SystolicPe positive;
    positive.tick(mac(-128, -128));
    EXPECT_EQ(positive.quantized(0), 127);
    EXPECT_EQ(positive.quantized(1), 127);
    EXPECT_EQ(positive.quantized(8), 64);
    EXPECT_EQ(positive.quantized(15), 0);
    EXPECT_EQ(positive.quantized(23), 0);
    EXPECT_EQ(positive.quantized(31), 0);

    SystolicPe negative;
    negative.tick(mac(-128, 127));
    EXPECT_EQ(negative.quantized(0), -128);
    EXPECT_EQ(negative.quantized(1), -128);
    EXPECT_EQ(negative.quantized(8), -64);
    EXPECT_EQ(negative.quantized(15), -1);
    EXPECT_EQ(negative.quantized(23), -1);
    EXPECT_EQ(negative.quantized(31), -1);
    EXPECT_THROW(negative.quantized(32), std::invalid_argument);
}

} // anonymous namespace
} // namespace gem5::sau
