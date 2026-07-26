#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "sau/address_program.hh"
#include "sau/resource_config.hh"

namespace gem5::sau
{
namespace
{

TEST(RtlStreamAddressProgram, MatchesTheValidatedCompositeStrideFormula)
{
    // x=1 is the strict-validated streamed shape: every beat advances the
    // y/flow/instruction counters, so the sequence must equal the
    // composite-stride formula proven by the eight RTL fixtures:
    //   base + y*yStep*32 + f*flowStep*yStep*32 + i*32   (conv_kernal=0)
    SauStreamAddressResourceConfig config;
    config.baseAddress = 0x29120400;
    config.convKernal = 0;
    config.counters.xStep = 5;  // unused when xBurst==1
    config.counters.xBurst = 1;
    config.counters.yStep = 3;
    config.counters.yCycle = 4;
    config.counters.flowStep = 7;
    config.counters.flowCycle = 3;
    config.counters.instructionStep = 9;  // ignored for conv_kernal=0
    config.counters.instructionCycle = 2;

    RtlStreamAddressProgram program(config);
    for (uint32_t beat = 0; beat < 4 * 3 * 2; ++beat) {
        const uint32_t y = beat % 4;
        const uint32_t f = (beat / 4) % 3;
        const uint32_t i = beat / 12;
        ASSERT_FALSE(program.done());
        EXPECT_EQ(program.x(), 0u);
        EXPECT_EQ(program.y(), y);
        EXPECT_EQ(program.flow(), f);
        EXPECT_EQ(program.instruction(), i);
        EXPECT_EQ(program.address(),
                  0x29120400 + y * (3 * 32) + f * (7 * 3 * 32) + i * 32);
        EXPECT_EQ(program.lastOfFlow(), y == 3);
        EXPECT_EQ(program.last(), beat == 23);
        program.advance();
    }
    EXPECT_TRUE(program.done());
}

TEST(RtlStreamAddressProgram, ZeroCountEndsImmediately)
{
    // mem_addr.sv raises last_load_done from IDLE when any count is
    // zero: the raw zero boundary produces no address, it is not "one".
    SauStreamAddressResourceConfig config;
    config.baseAddress = 0x1000;
    config.counters.xBurst = 1;
    config.counters.yCycle = 32;
    config.counters.flowCycle = 0;
    config.counters.instructionCycle = 1;

    RtlStreamAddressProgram program(config);
    EXPECT_TRUE(program.done());
}

TEST(RtlStreamAddressProgram, SingleRowEndsAfterTheXWalk)
{
    // y_cycle==1 takes the RUNNING else-branch straight to DONE after
    // the x walk; flow/instruction never advance (frozen source).
    SauStreamAddressResourceConfig config;
    config.baseAddress = 0x2000;
    config.counters.xStep = 2;
    config.counters.xBurst = 4;
    config.counters.yStep = 1;
    config.counters.yCycle = 1;
    config.counters.flowStep = 1;
    config.counters.flowCycle = 3;
    config.counters.instructionStep = 1;
    config.counters.instructionCycle = 2;

    RtlStreamAddressProgram program(config);
    for (uint32_t beat = 0; beat < 4; ++beat) {
        ASSERT_FALSE(program.done());
        EXPECT_EQ(program.address(), 0x2000 + beat * (2 * 32));
        EXPECT_FALSE(program.lastOfFlow());
        EXPECT_EQ(program.last(), beat == 3);
        program.advance();
    }
    EXPECT_TRUE(program.done());
}

TEST(RtlStreamAddressProgram, EmitsOneFinalRowBeatWhenXBurstExceedsOne)
{
    // The source leaves RUNNING when it enters a flow's final row, so an
    // x_burst>1 flow emits only that row's first beat before the
    // flow/instruction retrigger.
    SauStreamAddressResourceConfig config;
    config.baseAddress = 0x4000;
    config.counters.xStep = 1;
    config.counters.xBurst = 2;
    config.counters.yStep = 2;
    config.counters.yCycle = 3;
    config.counters.flowStep = 4;
    config.counters.flowCycle = 2;
    config.counters.instructionStep = 1;
    config.counters.instructionCycle = 1;

    const std::vector<uint64_t> expected = {
        // flow 0: two full rows, then one final-row beat.
        0x4000, 0x4020, 0x4040, 0x4060, 0x4080,
        // flow 1 restarts from flow_start + 4*2*32.
        0x4100, 0x4120, 0x4140, 0x4160, 0x4180,
    };
    RtlStreamAddressProgram program(config);
    for (size_t beat = 0; beat < expected.size(); ++beat) {
        ASSERT_FALSE(program.done());
        EXPECT_EQ(program.address(), expected[beat]) << "beat " << beat;
        EXPECT_EQ(program.lastOfFlow(), beat == 4 || beat == 9);
        EXPECT_EQ(program.last(), beat == 9);
        program.advance();
    }
    EXPECT_TRUE(program.done());
}

TEST(RtlResidentAddressProgram, MatchesTheValidatedLinearBaseline)
{
    // The baseline resident load (x_burst=8, y_step=8, y_cycle=32) is
    // linear because the y step equals the row extent: 256 consecutive
    // 32-byte beats.
    SauResidentAddressResourceConfig config;
    config.baseAddress = 0x29120000;
    config.xBurst = 8;
    config.yStep = 8;
    config.yCycle = 32;
    config.cStep = 0;
    config.cCycle = 1;

    RtlResidentAddressProgram program(config);
    for (uint32_t beat = 0; beat < 256; ++beat) {
        ASSERT_FALSE(program.done());
        EXPECT_EQ(program.address(), 0x29120000 + beat * 32);
        EXPECT_EQ(program.writePointer(), beat);
        EXPECT_FALSE(program.paddingBeat());
        EXPECT_EQ(program.last(), beat == 255);
        program.advance();
    }
    EXPECT_TRUE(program.done());
}

TEST(RtlResidentAddressProgram, PaddingFreezesAddressesButNotThePointer)
{
    // pad!=0 with valid windows x=[1,2], y=[0,1]: padded x positions
    // freeze the running address, a fully padded row freezes row_start
    // and returns to the row start, and the write pointer advances on
    // every beat regardless.
    SauResidentAddressResourceConfig config;
    config.baseAddress = 0x8000;
    config.xBurst = 4;
    config.yStep = 4;
    config.yCycle = 3;
    config.cStep = 3;
    config.cCycle = 2;
    config.padding = 1;
    config.validXStart = 1;
    config.validXEnd = 2;
    config.validYStart = 0;
    config.validYEnd = 1;

    struct Expected { uint64_t address; bool padded; };
    const std::vector<Expected> expected = {
        // channel 0, row 0 (valid y): x0 padded, x1/x2 valid, x3 padded.
        {0x8000, true}, {0x8000, false}, {0x8020, false}, {0x8040, true},
        // row 1 (valid y) starts at row_start + 4*32.
        {0x8080, true}, {0x8080, false}, {0x80a0, false}, {0x80c0, true},
        // row 2 is y-padded: the address freezes at the row start.
        {0x8100, true}, {0x8100, true}, {0x8100, true}, {0x8100, true},
        // channel 1 restarts at channel_start + 3*4*32.
        {0x8180, true}, {0x8180, false}, {0x81a0, false}, {0x81c0, true},
        {0x8200, true}, {0x8200, false}, {0x8220, false}, {0x8240, true},
        {0x8280, true}, {0x8280, true}, {0x8280, true}, {0x8280, true},
    };
    RtlResidentAddressProgram program(config);
    for (size_t beat = 0; beat < expected.size(); ++beat) {
        ASSERT_FALSE(program.done());
        EXPECT_EQ(program.address(), expected[beat].address)
            << "beat " << beat;
        EXPECT_EQ(program.paddingBeat(), expected[beat].padded)
            << "beat " << beat;
        EXPECT_EQ(program.writePointer(), beat);
        EXPECT_EQ(program.last(), beat == expected.size() - 1);
        program.advance();
    }
    EXPECT_TRUE(program.done());
}

TEST(RtlResidentAddressProgram, ZeroCountEndsImmediately)
{
    SauResidentAddressResourceConfig config;
    config.baseAddress = 0x1000;
    config.xBurst = 8;
    config.yCycle = 32;
    config.cCycle = 0;

    RtlResidentAddressProgram program(config);
    EXPECT_TRUE(program.done());
}

TEST(RtlAddressPrograms, ConsumeTheDispatchedResourceConfigs)
{
    // Raw control -> typed resource configs -> address programs is the
    // Step 2 chain: both programs start at their CSR base addresses and
    // produce the raw-counter beat totals.
    SauControlFields control;
    control.verticalAddress = 0x29120400;
    control.horizontalAddress = 0x29120000;
    control.vertical.xBurst = 1;
    control.vertical.yStep = 1;
    control.vertical.yCycle = 32;
    control.vertical.flowStep = 1;
    control.vertical.flowCycle = 1;
    control.vertical.instructionCycle = 1;
    control.registerInput.xBurst = 1;
    control.registerInput.yStep = 1;
    control.registerInput.yCycle = 32;
    control.registerInput.cCycle = 1;

    const auto configs = deriveResourceConfigs(control);
    RtlStreamAddressProgram stream(configs.streamAddress);
    RtlResidentAddressProgram resident(configs.residentAddress);

    EXPECT_EQ(stream.address(), 0x29120400);
    EXPECT_EQ(resident.address(), 0x29120000);

    uint32_t streamBeats = 0;
    while (!stream.done()) {
        ++streamBeats;
        stream.advance();
    }
    uint32_t residentBeats = 0;
    while (!resident.done()) {
        ++residentBeats;
        resident.advance();
    }
    EXPECT_EQ(streamBeats, 32u);
    EXPECT_EQ(residentBeats, 32u);
}

} // anonymous namespace
} // namespace gem5::sau
