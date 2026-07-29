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
    control.cutbit = 8;
    control.flowLoopTimes = 1;
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
    control.vertical.instructionCycle = 1;
    control.outputAddress = 0x29120c00;
    control.output.xStep = 0;
    control.output.xBurst = 1;
    control.output.yStep = 1;
    control.output.yBurst = 32;
    control.output.flowStep = 0;
    control.output.flowBurst = 1;
    control.output.instructionStep = 0;
    control.output.instructionBurst = 1;
    control.output.registerXBurst = 1;
    control.output.registerYStep = 1;
    control.output.registerYCycle = 32;
    control.output.registerCStep = 1;
    control.output.registerCCycle = 1;
    return control;
}

SauControlFields
abtdSmallControl()
{
    auto control = atbdSmallControl();
    control.transMode = 2;
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
        datapath.beginCycle();
        datapath.onRegisterFileReadValid(50 + read);
        datapath.onOperandAValid(52 + read);
        const auto &events = datapath.boundaryEvents();
        ASSERT_TRUE(events.operandA);
        ASSERT_TRUE(events.transposerInput);
        EXPECT_EQ(events.operandA->edge, 52u + read);
        EXPECT_EQ(events.operandA->data, patternBeat(read % 32));
        EXPECT_EQ(events.transposerInput->edge, 53u + read);
        EXPECT_EQ(events.transposerInput->bank, read / 32);
        datapath.sampleCycle(52 + read);
    }
    EXPECT_EQ(datapath.operandATokens(), 64u);
    EXPECT_EQ(datapath.transposerInputRows(), 64u);
    EXPECT_EQ(datapath.transposerInputStalls(), 0u);
    EXPECT_EQ(datapath.payloadUnderflows(), 0u);
    EXPECT_EQ(datapath.firstRowEdge(), 53u);
    EXPECT_EQ(datapath.transposerMaxOccupancy(), 64u);
    EXPECT_GE(datapath.transposerBusyCycles(), 32u);

    // 32 SA-enable edges drain the first bank's transposed columns.
    for (unsigned column = 0; column < 32; ++column) {
        datapath.beginCycle();
        datapath.onSaEnable(120 + column);
        datapath.sampleCycle(120 + column);
        const auto &events = datapath.boundaryEvents();
        ASSERT_TRUE(events.transposerOutput);
        EXPECT_EQ(events.transposerOutput->edge, 120u + column);
        EXPECT_EQ(events.transposerOutput->bank, 0u);
        if (column + 1 == 32) {
            ASSERT_TRUE(events.transposerPrefetch);
            EXPECT_EQ(events.transposerPrefetch->edge, 152u);
            EXPECT_EQ(events.transposerPrefetch->bank, 1u);
        } else {
            EXPECT_FALSE(events.transposerPrefetch);
        }
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

TEST(StrictPayloadDatapath, DrivesAtbdPairsThroughTheArrayResultStream)
{
    StrictPayloadDatapath datapath(
        deriveResourceConfigs(atbdSmallControl()));

    // Fill T0 with A rows.  Sampling each strict tick also advances the
    // array clock, though no vector pair is accepted during this phase.
    for (unsigned sourceRow = 0; sourceRow < 32; ++sourceRow) {
        MemoryBeat256 row;
        for (unsigned inner = 0; inner < 32; ++inner) {
            row.bytes[inner] = static_cast<uint8_t>(
                static_cast<int8_t>(inner - sourceRow));
        }
        datapath.onMemoryDataVisible(sourceRow, row, false);
        datapath.beginCycle();
        datapath.onRegisterFileReadValid(sourceRow);
        datapath.onOperandAValid(sourceRow);
        datapath.sampleCycle(sourceRow);
    }

    std::array<MemoryBeat256, 32> weightRows;
    for (unsigned inner = 0; inner < 32; ++inner) {
        for (unsigned column = 0; column < 32; ++column) {
            weightRows[inner].bytes[column] = static_cast<uint8_t>(
                static_cast<int8_t>(column + inner - 16));
        }
        datapath.onMemoryDataVisible(32 + inner, weightRows[inner], true);
    }

    // B is registered one edge before SA_ENGINE samples it. The first
    // B edge therefore primes the payload register; the following 32
    // SA edges pair B[k] with transposed-A column k.
    datapath.beginCycle();
    datapath.onOperandBValid(32);
    datapath.sampleCycle(32);
    for (unsigned inner = 0; inner < 32; ++inner) {
        const uint64_t edge = 33 + inner;
        datapath.beginCycle();
        if (inner + 1 < 32) {
            datapath.onOperandBValid(edge);
        }
        datapath.onSaEnable(edge);
        datapath.sampleCycle(edge);

        const auto &events = datapath.boundaryEvents();
        ASSERT_TRUE(events.arrayInput);
        EXPECT_EQ(events.arrayInput->edge, edge);
        EXPECT_EQ(events.arrayInput->finish, inner == 31);
        EXPECT_EQ(events.arrayInput->weights,
                  operandFromBeat(weightRows[inner]));
        EXPECT_EQ(events.arrayInput->activations,
                  operandFromBeat(datapath.lastColumn()));
    }
    EXPECT_EQ(datapath.arrayState().acceptedInputs(), 32u);

    // The frozen RTL starts row streaming 13 cycles after the final
    // accepted pair.  Keep that control request explicit at this layer.
    std::array<OperandVector32x8, 32> outputs;
    unsigned outputCount = 0;
    for (uint64_t edge = 65; edge <= 108; ++edge) {
        datapath.beginCycle();
        if (edge == 77) {
            datapath.requestArrayOutput();
        }
        datapath.sampleCycle(edge);
        const auto &event = datapath.boundaryEvents().arrayOutput;
        if (!event) {
            continue;
        }
        ASSERT_LT(outputCount, outputs.size());
        EXPECT_EQ(event->edge, 77u + outputCount);
        EXPECT_EQ(event->row, outputCount);
        outputs[outputCount++] = event->data;
    }
    ASSERT_EQ(outputCount, 32u);
    EXPECT_TRUE(datapath.arrayState().calFinish());

    // Independent scalar GEMM replay in physical SA row order:
    // The transposer reverses its output lanes and the array reverses
    // them again, so physical row r consumes resident beat r.
    for (unsigned row = 0; row < 32; ++row) {
        for (unsigned column = 0; column < 32; ++column) {
            int32_t sum = 0;
            for (unsigned inner = 0; inner < 32; ++inner) {
                const int8_t activation =
                    static_cast<int8_t>(inner - row);
                const int8_t weight =
                    static_cast<int8_t>(column + inner - 16);
                sum = saturateAddInt24(
                    sum, static_cast<int32_t>(activation) * weight);
            }
            EXPECT_EQ(outputs[row].lanes[column],
                      satTruncateInt24(sum, 8));
        }
    }

    // Driver result-valid edges drain the finite result serializer into
    // register_file_out. Flow mode 0 preserves row order.
    ASSERT_TRUE(datapath.resultSerializerState().outputReady());
    for (unsigned row = 0; row < 32; ++row) {
        datapath.beginCycle();
        datapath.onResultValid(120 + row);
        const auto &update = datapath.boundaryEvents().outputRegister;
        ASSERT_TRUE(update);
        EXPECT_EQ(update->edge, 120u + row);
        EXPECT_EQ(update->update.address, row);
        EXPECT_EQ(update->update.last, row == 31);
        for (unsigned column = 0; column < 32; ++column) {
            EXPECT_EQ(update->update.data.bytes[column],
                      static_cast<uint8_t>(outputs[row].lanes[column]));
        }
    }
    EXPECT_EQ(datapath.outputRegisterUpdates(), 32u);
    EXPECT_TRUE(datapath.outputRegisterState().resultAccumDone());
    EXPECT_FALSE(datapath.resultSerializerState().outputReady());

    // The first REGISTER_UNLOAD edge arms register_out_state. The next
    // edge launches register_addr; the registered address and d1/d2
    // pipeline then expose the first native payload four ticks later.
    datapath.beginCycle();
    datapath.onRegisterUnload(200, true);
    datapath.beginCycle();
    datapath.onRegisterUnload(201, true);
    unsigned firstUnloadEdge = 0;
    unsigned lastUnloadEdge = 0;
    for (uint64_t edge = 202; edge <= 240; ++edge) {
        datapath.beginCycle();
        datapath.onRegisterUnload(edge, true);
        const auto &unload = datapath.boundaryEvents().outputUnload;
        if (!unload) {
            continue;
        }
        if (firstUnloadEdge == 0) {
            firstUnloadEdge = edge;
        }
        if (unload->unload.last) {
            lastUnloadEdge = edge;
        }
    }
    EXPECT_EQ(firstUnloadEdge, 205u);
    EXPECT_EQ(lastUnloadEdge, 236u);
    EXPECT_EQ(datapath.outputUnloadBeats(), 32u);
    EXPECT_TRUE(datapath.outputRegisterState().unloadDone());

    for (unsigned row = 0; row < 32; ++row) {
        ASSERT_TRUE(datapath.hasWritePayload());
        const auto payload = datapath.takeWritePayload();
        EXPECT_EQ(payload.logicalAddress, row);
        EXPECT_EQ(payload.address,
                  0x29120c00 + static_cast<Addr>(row) * BeatBytes);
        EXPECT_EQ(payload.last, row == 31);
        for (unsigned column = 0; column < 32; ++column) {
            EXPECT_EQ(payload.data.bytes[column],
                      static_cast<uint8_t>(outputs[row].lanes[column]));
        }
    }
    EXPECT_FALSE(datapath.hasWritePayload());
    EXPECT_THROW(datapath.takeWritePayload(), std::logic_error);
}

TEST(StrictPayloadDatapath, KeepsOperandBOutsideTheAtbdBanks)
{
    StrictPayloadDatapath datapath(
        deriveResourceConfigs(atbdSmallControl()));

    for (unsigned beat = 0; beat < 4; ++beat) {
        datapath.onMemoryDataVisible(20 + beat, patternBeat(beat), true);
        datapath.beginCycle();
        datapath.onOperandBValid(24 + beat);
        const auto &events = datapath.boundaryEvents();
        ASSERT_TRUE(events.operandB);
        EXPECT_EQ(events.operandB->edge, 24u + beat);
        EXPECT_EQ(events.operandB->data, patternBeat(beat));
        EXPECT_FALSE(events.transposerInput);
    }
    EXPECT_EQ(datapath.streamedBeats(), 4u);
    EXPECT_EQ(datapath.operandBTokens(), 4u);
    EXPECT_EQ(datapath.transposerInputRows(), 0u);
    EXPECT_EQ(datapath.payloadUnderflows(), 0u);
}

TEST(StrictPayloadDatapath,
     UsesBValidAsLoadTriggerAndInputSwitchAsPayloadMuxForAbtd)
{
    StrictPayloadDatapath datapath(deriveResourceConfigs(abtdSmallControl()));

    std::array<MemoryBeat256, 32> residentRows;
    std::array<MemoryBeat256, 32> streamedRows;
    for (unsigned row = 0; row < 32; ++row) {
        residentRows[row] = patternBeat(row);
        streamedRows[row] = patternBeat(row + 64);
        datapath.onMemoryDataVisible(row, residentRows[row], false);
        datapath.onMemoryDataVisible(row, streamedRows[row], true);
    }

    // Frozen ABTD exposes one resident-tail B pulse before A readout.
    datapath.beginCycle();
    datapath.onOperandBValid(0);
    ASSERT_TRUE(datapath.boundaryEvents().operandB);
    EXPECT_EQ(datapath.boundaryEvents().operandB->data, residentRows[31]);
    datapath.advanceFeederMux(0, 0x3);
    datapath.sampleCycle(0);

    // First A readout pass has no streamed B-valid.
    for (unsigned row = 0; row < 32; ++row) {
        const uint64_t edge = row + 1;
        datapath.beginCycle();
        datapath.onRegisterFileReadValid(edge);
        datapath.onOperandAValid(edge);
        datapath.advanceFeederMux(edge, 0x3);
        if (row == 0) {
            ASSERT_TRUE(datapath.boundaryEvents().transposerInput);
            EXPECT_EQ(datapath.boundaryEvents().transposerInput->data,
                      residentRows[31]);
        }
        datapath.sampleCycle(edge);
    }

    // The second A replay coincides with all 32 streamed B pulses. Switch 01
    // makes B-valid the load trigger but selects the current A payload. The
    // first streamed trigger only schedules the registered load; SA-enable
    // begins on the following edge.
    for (unsigned row = 0; row < 32; ++row) {
        const uint64_t edge = row + 33;
        datapath.beginCycle();
        datapath.onRegisterFileReadValid(edge);
        datapath.onOperandAValid(edge);
        datapath.onOperandBValid(edge);
        ASSERT_TRUE(datapath.boundaryEvents().operandB);
        EXPECT_EQ(datapath.boundaryEvents().operandB->data,
                  streamedRows[row]);
        datapath.advanceFeederMux(edge, 0x1);
        if (row != 0) {
            datapath.onSaEnable(edge, 0x1);
            ASSERT_TRUE(datapath.boundaryEvents().arrayInput);
            EXPECT_EQ(datapath.boundaryEvents().arrayInput->activations,
                      OperandVector32x8{});
            EXPECT_FALSE(datapath.boundaryEvents().transposerOutput);
            EXPECT_FALSE(datapath.boundaryEvents().arrayInput->finish);
        }
        datapath.sampleCycle(edge);
    }

    // The final B trigger arrives after the single T0 tile is already full;
    // frozen ready gating drops that pending overflow row. The registered
    // ready/outCol state is now visible to the final SA-enable edge.
    datapath.beginCycle();
    datapath.advanceFeederMux(65, 0x1);
    EXPECT_FALSE(datapath.boundaryEvents().transposerInput);
    datapath.onSaEnable(65, 0x1);
    ASSERT_TRUE(datapath.boundaryEvents().arrayInput);
    ASSERT_TRUE(datapath.boundaryEvents().transposerOutput);
    EXPECT_EQ(
        datapath.boundaryEvents().arrayInput->activations,
        operandFromBeat(datapath.boundaryEvents().transposerOutput->data));
    EXPECT_TRUE(datapath.boundaryEvents().arrayInput->finish);

    EXPECT_EQ(datapath.transposerInputRows(), 32u);
    EXPECT_EQ(datapath.transposerInputStalls(), 1u);
    EXPECT_EQ(datapath.payloadUnderflows(), 0u);
    EXPECT_EQ(datapath.operandATokens(), 64u);
    EXPECT_EQ(datapath.operandBTokens(), 33u);
    EXPECT_EQ(datapath.transposerOutputColumns(), 1u);
    EXPECT_EQ(datapath.transposerOutputStalls(), 0u);
    EXPECT_TRUE(datapath.arbiterState().columnReady());
}

TEST(StrictPayloadDatapath, CountsPulsePayloadDivergence)
{
    StrictPayloadDatapath datapath(
        deriveResourceConfigs(atbdSmallControl()));

    // An operand edge without a queued payload and an SA edge without
    // a drainable column are counted, not fatal.
    datapath.beginCycle();
    datapath.onOperandAValid(5);
    datapath.onOperandBValid(6);
    datapath.onSaEnable(7);
    EXPECT_EQ(datapath.payloadUnderflows(), 2u);
    EXPECT_EQ(datapath.transposerOutputStalls(), 1u);
    EXPECT_EQ(datapath.transposerInputRows(), 0u);
}

} // anonymous namespace
} // namespace gem5::sau
