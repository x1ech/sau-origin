#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "sau/input_datapath.hh"

namespace gem5::sau
{
namespace
{

MemoryBeat256
patternBeat(uint8_t seed)
{
    MemoryBeat256 beat;
    for (unsigned byte = 0; byte < BeatBytes; ++byte) {
        beat.bytes[byte] = static_cast<uint8_t>(seed + byte);
    }
    return beat;
}

TEST(StreamPaddingShifter, ZeroPaddingPassesBeatsThrough)
{
    StreamPaddingShifter shifter(0);

    const auto first = patternBeat(0x10);
    const auto second = patternBeat(0x60);
    EXPECT_EQ(shifter.shift(first, true), first);
    EXPECT_EQ(shifter.shift(second, false), second);
}

TEST(StreamPaddingShifter, InsertsZerosAtStartAndCarriesThePreviousTail)
{
    StreamPaddingShifter shifter(3);

    const auto first = patternBeat(0x10);
    const auto shiftedFirst = shifter.shift(first, true);
    for (unsigned byte = 0; byte < 3; ++byte) {
        EXPECT_EQ(shiftedFirst.bytes[byte], 0);
    }
    for (unsigned byte = 3; byte < BeatBytes; ++byte) {
        EXPECT_EQ(shiftedFirst.bytes[byte], first.bytes[byte - 3]);
    }

    const auto second = patternBeat(0x60);
    const auto shiftedSecond = shifter.shift(second, false);
    for (unsigned byte = 0; byte < 3; ++byte) {
        EXPECT_EQ(shiftedSecond.bytes[byte],
                  first.bytes[BeatBytes - 3 + byte]);
    }
    for (unsigned byte = 3; byte < BeatBytes; ++byte) {
        EXPECT_EQ(shiftedSecond.bytes[byte], second.bytes[byte - 3]);
    }
}

TEST(InputRegisterFile, StoresBeatsAndTracksAvailLabels)
{
    InputRegisterFile file;

    EXPECT_FALSE(file.available(7));
    file.write(7, patternBeat(0x21));
    EXPECT_TRUE(file.available(7));
    EXPECT_EQ(file.read(7), patternBeat(0x21));

    // register_clear_flag clears only the labels; SRAM contents are not
    // a functional initializer and the read port has no label gate.
    file.clearLabels();
    EXPECT_FALSE(file.available(7));
    EXPECT_EQ(file.read(7), patternBeat(0x21));
}

TEST(FeederBPipeline, DelaysBeatsByFourRegisteredStages)
{
    FeederBPipeline pipeline;

    // Post-edge reads: after step k the one-stage write tap holds beat
    // k and the four-register data_B_o chain holds beat k-3 (i.e. the
    // input becomes visible on the fourth cycle, as in the RTL).
    const MemoryBeat256 zero{};
    for (uint8_t beat = 0; beat < 10; ++beat) {
        pipeline.step(patternBeat(beat), true);
        EXPECT_EQ(pipeline.registerFileWriteData(), patternBeat(beat));
        if (beat >= 3) {
            EXPECT_EQ(pipeline.operandB(), patternBeat(beat - 3));
        } else {
            EXPECT_EQ(pipeline.operandB(), zero);
        }
    }
}

TEST(FeederBPipeline, RegistersAZeroBeatWhenTheArbiterDisablesB)
{
    FeederBPipeline pipeline;

    for (uint8_t beat = 0; beat < 6; ++beat) {
        pipeline.step(patternBeat(beat), beat != 5);
    }
    // Edge 5 was disabled: the output register holds zero even though
    // the internal chain kept moving.
    EXPECT_EQ(pipeline.operandB(), MemoryBeat256{});
    pipeline.step(patternBeat(6), true);
    EXPECT_EQ(pipeline.operandB(), patternBeat(3));
}

TEST(FeederAPipeline, DelaysReadoutsByTwoRegisteredStages)
{
    FeederAPipeline pipeline;

    const MemoryBeat256 zero{};
    for (uint8_t beat = 0; beat < 8; ++beat) {
        pipeline.step(patternBeat(beat), true);
        if (beat >= 1) {
            EXPECT_EQ(pipeline.operandA(), patternBeat(beat - 1));
        } else {
            EXPECT_EQ(pipeline.operandA(), zero);
        }
    }
}

TEST(FeederAPipeline, IdleCyclesInsertZeroBubbles)
{
    FeederAPipeline pipeline;

    pipeline.step(patternBeat(1), true);
    pipeline.step(patternBeat(2), false);
    pipeline.step(patternBeat(3), true);
    // The disabled edge loaded zero into the shift stage; it emerges
    // one edge later while the enabled beats pass through in order.
    EXPECT_EQ(pipeline.operandA(), MemoryBeat256{});
    pipeline.step(patternBeat(4), true);
    EXPECT_EQ(pipeline.operandA(), patternBeat(3));
}

TEST(InputDatapath, ResidentAPathPreservesTheStoredBeatOrder)
{
    // Full A-side composition: beats stored through the write path are
    // read back in read-pointer order and reach operand A unchanged and
    // in sequence — the payload contract the ABTD golden shows between
    // the register-file readout and data_A.
    InputRegisterFile file;
    InputWritePath writePath(0);
    for (uint8_t beat = 0; beat < 32; ++beat) {
        writePath.write(file, beat, patternBeat(beat), false, beat == 0);
    }

    SauInputCsrConfig counters;
    counters.xStep = 1;
    counters.xBurst = 1;
    counters.yStep = 1;
    counters.yBurst = 32;
    counters.flowStep = 1;
    counters.flowBurst = 1;
    counters.instructionStep = 1;
    counters.instructionBurst = 1;

    InputReadPointerProgram program(counters);
    FeederAPipeline pipeline;
    std::vector<MemoryBeat256> received;
    while (!program.done()) {
        pipeline.step(file.read(program.pointer()), true);
        received.push_back(pipeline.operandA());
        program.advance();
    }
    pipeline.step(MemoryBeat256{}, false);
    received.push_back(pipeline.operandA());

    ASSERT_EQ(received.size(), 33u);
    EXPECT_EQ(received.front(), MemoryBeat256{});
    for (uint8_t beat = 0; beat < 32; ++beat) {
        EXPECT_EQ(received[beat + 1u], patternBeat(beat));
    }
}

TEST(InputWritePath, StoresRawBeatsWhenPaddingIsZero)
{
    InputRegisterFile file;
    InputWritePath path(0);

    for (uint8_t beat = 0; beat < 4; ++beat) {
        path.write(file, beat, patternBeat(beat), false, beat == 0);
    }
    for (uint8_t beat = 0; beat < 4; ++beat) {
        EXPECT_TRUE(file.available(beat));
        EXPECT_EQ(file.read(beat), patternBeat(beat));
    }
}

TEST(InputWritePath, ZeroesPadFlaggedBeatsThroughTheShifter)
{
    InputRegisterFile file;
    InputWritePath path(2);

    // A pad-flagged first beat writes zero into the shifter; the next
    // valid beat's low bytes carry the zero tail, and its own tail
    // carries into the beat after.
    path.write(file, 0, patternBeat(0x40), true, true);
    path.write(file, 1, patternBeat(0x50), false, false);
    path.write(file, 2, patternBeat(0x70), false, false);

    EXPECT_EQ(file.read(0), MemoryBeat256{});
    const auto second = file.read(1);
    EXPECT_EQ(second.bytes[0], 0);
    EXPECT_EQ(second.bytes[1], 0);
    for (unsigned byte = 2; byte < BeatBytes; ++byte) {
        EXPECT_EQ(second.bytes[byte], patternBeat(0x50).bytes[byte - 2]);
    }
    const auto third = file.read(2);
    EXPECT_EQ(third.bytes[0], patternBeat(0x50).bytes[30]);
    EXPECT_EQ(third.bytes[1], patternBeat(0x50).bytes[31]);
    for (unsigned byte = 2; byte < BeatBytes; ++byte) {
        EXPECT_EQ(third.bytes[byte], patternBeat(0x70).bytes[byte - 2]);
    }
}

TEST(InputReadPointerProgram, WalksTheSmallFixtureSequence)
{
    // Small fixture input group: one-beat x, 32-row y walk, single
    // flow/instruction: pointers 0..31, last-of-burst on the final row.
    SauInputCsrConfig counters;
    counters.xStep = 1;
    counters.xBurst = 1;
    counters.yStep = 1;
    counters.yBurst = 32;
    counters.flowStep = 1;
    counters.flowBurst = 1;
    counters.instructionStep = 1;
    counters.instructionBurst = 1;

    InputReadPointerProgram program(counters);
    for (uint32_t beat = 0; beat < 32; ++beat) {
        ASSERT_FALSE(program.done());
        EXPECT_EQ(program.pointer(), beat);
        EXPECT_EQ(program.lastOfBurst(), beat == 31);
        EXPECT_EQ(program.last(), beat == 31);
        program.advance();
    }
    EXPECT_TRUE(program.done());
}

TEST(InputReadPointerProgram, StepsInstructionBlocksLikeTheBaseline)
{
    // Baseline input group: 32-row bursts repeated for 8 instructions
    // with instruction step 32 walk the whole 256-entry register file.
    SauInputCsrConfig counters;
    counters.xStep = 1;
    counters.xBurst = 1;
    counters.yStep = 1;
    counters.yBurst = 32;
    counters.flowStep = 1;
    counters.flowBurst = 1;
    counters.instructionStep = 32;
    counters.instructionBurst = 8;

    InputReadPointerProgram program(counters);
    for (uint32_t beat = 0; beat < 256; ++beat) {
        const uint32_t instruction = beat / 32;
        const uint32_t row = beat % 32;
        ASSERT_FALSE(program.done());
        EXPECT_EQ(program.pointer(), instruction * 32 + row);
        EXPECT_EQ(program.instruction(), instruction);
        EXPECT_EQ(program.y(), row);
        EXPECT_EQ(program.lastOfBurst(), row == 31);
        EXPECT_EQ(program.last(), beat == 255);
        program.advance();
    }
    EXPECT_TRUE(program.done());
}

TEST(InputReadPointerProgram, WrapsTheEightBitPointer)
{
    SauInputCsrConfig counters;
    counters.xStep = 1;
    counters.xBurst = 1;
    counters.yStep = 200;
    counters.yBurst = 3;
    counters.flowStep = 1;
    counters.flowBurst = 1;
    counters.instructionStep = 1;
    counters.instructionBurst = 1;

    InputReadPointerProgram program(counters);
    EXPECT_EQ(program.pointer(), 0);
    program.advance();
    EXPECT_EQ(program.pointer(), 200);
    program.advance();
    // 400 wraps mod 256 in the 8-bit address register.
    EXPECT_EQ(program.pointer(), 144);
    EXPECT_TRUE(program.last());
    program.advance();
    EXPECT_TRUE(program.done());
}

TEST(InputReadPointerProgram, ZeroBurstMeansSixtyFourIterations)
{
    // The 6-bit "burst - 1" compare wraps a raw zero burst to 63, so
    // this FSM runs 64 iterations — deliberately different from
    // mem_addr.sv's immediate-done zero guard.
    SauInputCsrConfig counters;
    counters.xStep = 2;
    counters.xBurst = 0;
    counters.yStep = 1;
    counters.yBurst = 1;
    counters.flowStep = 1;
    counters.flowBurst = 1;
    counters.instructionStep = 1;
    counters.instructionBurst = 1;

    InputReadPointerProgram program(counters);
    for (uint32_t beat = 0; beat < 64; ++beat) {
        ASSERT_FALSE(program.done());
        EXPECT_EQ(program.pointer(), static_cast<uint8_t>(beat * 2));
        EXPECT_EQ(program.last(), beat == 63);
        program.advance();
    }
    EXPECT_TRUE(program.done());
}

} // anonymous namespace
} // namespace gem5::sau
