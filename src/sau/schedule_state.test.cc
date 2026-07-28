#include <gtest/gtest.h>

#include <stdexcept>

#include "sau/schedule_state.hh"

namespace gem5::sau
{
namespace
{

RtlCommandDriverConfig
commandDriverConfig(uint32_t flowTimes = 8,
                    uint32_t instructionTimes = 8,
                    uint8_t saFlowMode = 0)
{
    const uint32_t residentXBurst = flowTimes;
    return {
        {32, flowTimes, instructionTimes, 1, 1, saFlowMode, false},
        {residentXBurst, 32, 1},
        {1, 32, flowTimes, instructionTimes},
        {1, 32, flowTimes, instructionTimes, 32, 3, 2, 2, 2},
        {32, flowTimes, 0, false},
        {32, 4, 4},
        {1, 32, 1, instructionTimes, instructionTimes, 32, 1},
        3,
    };
}

TEST(SauSchedule, MapsSemanticStatesToRtlGuards)
{
    EXPECT_STREQ(scheduleStateMapping(SauScheduleState::ResidentLoad).rtlStates,
                 "REGISTER_LOAD");
    EXPECT_STREQ(scheduleStateMapping(SauScheduleState::TransposeSetup).rtlStates,
                 "TRANSPOSE_LOAD");
    EXPECT_STREQ(scheduleStateMapping(SauScheduleState::FlowExecute).rtlStates,
                 "REUSE_LOAD");
    EXPECT_STREQ(scheduleStateMapping(SauScheduleState::FlowBoundary).rtlStates,
                 "TRANSPOSE_CLIP");
    EXPECT_STREQ(
        scheduleStateMapping(SauScheduleState::DrainAndWriteback).rtlStates,
        "FIRST_LOAD|D_OUT|REGISTER_UNLOAD");
    EXPECT_STREQ(scheduleStateMapping(SauScheduleState::Complete).rtlStates,
                 "IDLE");
}

TEST(RtlSchedulerSkeleton, ReplaysCsrAndSchedulerStartRegisters)
{
    RtlSchedulerSkeleton scheduler({32, 8, 8, 1, 1, 0, false});

    RtlSchedulerInputs inputs;
    inputs.startWrite = true;
    scheduler.tick(inputs);
    EXPECT_TRUE(scheduler.start());
    EXPECT_EQ(scheduler.coreState(), RtlCoreState::Idle);

    scheduler.tick();
    EXPECT_FALSE(scheduler.start());
    EXPECT_EQ(scheduler.coreState(), RtlCoreState::Idle);

    scheduler.tick();
    EXPECT_EQ(scheduler.coreState(), RtlCoreState::RegisterLoad);
    EXPECT_EQ(scheduler.instructionState(), RtlInstructionState::First);
}

TEST(RtlSchedulerSkeleton, ResidentCountersDriveRegisterLoadDone)
{
    RtlSchedulerSkeleton scheduler({32, 8, 8, 1, 1, 0, false});
    RtlResidentLoadSkeleton resident({8, 32, 1});

    uint32_t requestCycles = 0;
    uint32_t transposeCycle = 0;
    bool transitionSawResidentLast = false;
    for (uint32_t cycle = 0; cycle < 300; ++cycle) {
        // Snapshot producer outputs before either component commits this edge.
        const bool start = scheduler.start();
        const bool residentLast = resident.requestLast();
        RtlSchedulerInputs inputs;
        inputs.startWrite = cycle == 0;
        inputs.registerLoadDone = residentLast;

        scheduler.tick(inputs);
        resident.tick(start);
        requestCycles += resident.requestValid() ? 1 : 0;

        if (scheduler.coreState() == RtlCoreState::TransposeLoad) {
            transposeCycle = cycle;
            transitionSawResidentLast = residentLast;
            break;
        }
    }

    EXPECT_EQ(requestCycles, 256U);
    EXPECT_EQ(transposeCycle, 258U);
    EXPECT_TRUE(transitionSawResidentLast);
}

TEST(RtlSchedulerSkeleton, CountsTheRtlTransposeWindowPerTick)
{
    RtlSchedulerSkeleton scheduler({32, 1, 1, 1, 1, 0, false});
    RtlSchedulerInputs inputs;
    inputs.startWrite = true;
    scheduler.tick(inputs);
    scheduler.tick();
    scheduler.tick();
    ASSERT_EQ(scheduler.coreState(), RtlCoreState::RegisterLoad);

    inputs = {};
    inputs.registerLoadDone = true;
    scheduler.tick(inputs);
    ASSERT_EQ(scheduler.coreState(), RtlCoreState::TransposeLoad);
    for (uint32_t count = 1; count < 32; ++count) {
        scheduler.tick();
        EXPECT_EQ(scheduler.coreState(), RtlCoreState::TransposeLoad);
        EXPECT_EQ(scheduler.transposeCount(), count);
    }
    scheduler.tick();
    EXPECT_EQ(scheduler.coreState(), RtlCoreState::ReuseLoad);
    EXPECT_EQ(scheduler.transposeCount(), 0U);
    EXPECT_TRUE(scheduler.nextExecuteStart());
}

TEST(RtlSchedulerSkeleton, UsesFlowAndInstructionCountersForDOut)
{
    RtlSchedulerSkeleton scheduler({32, 1, 1, 1, 1, 0, false});
    RtlSchedulerInputs inputs;
    inputs.startWrite = true;
    scheduler.tick(inputs);
    scheduler.tick();
    scheduler.tick();
    inputs = {};
    inputs.registerLoadDone = true;
    scheduler.tick(inputs);
    for (uint32_t count = 0; count < 32; ++count) {
        scheduler.tick();
    }
    ASSERT_EQ(scheduler.coreState(), RtlCoreState::ReuseLoad);

    inputs = {};
    inputs.loadDone = true;
    scheduler.tick(inputs);
    EXPECT_EQ(scheduler.coreState(), RtlCoreState::FirstLoad);
    EXPECT_EQ(scheduler.flowCount(), 1U);
    scheduler.tick(inputs);
    EXPECT_EQ(scheduler.coreState(), RtlCoreState::DOut);
    EXPECT_EQ(scheduler.instructionState(), RtlInstructionState::Done);

    inputs = {};
    inputs.updateFinished = true;
    scheduler.tick(inputs);
    EXPECT_EQ(scheduler.coreState(), RtlCoreState::RegisterUnload);
    EXPECT_FALSE(scheduler.commandDone());

    inputs = {};
    inputs.writeFinished = true;
    scheduler.tick(inputs);
    EXPECT_EQ(scheduler.coreState(), RtlCoreState::Idle);
    EXPECT_TRUE(scheduler.commandDone());
}

TEST(RtlSchedulerSkeleton, UsesUpdateForSingleFlowInstructionBoundaries)
{
    RtlSchedulerSkeleton scheduler({1, 1, 2, 1, 1, 0, false});
    RtlSchedulerInputs inputs;
    inputs.startWrite = true;
    scheduler.tick(inputs);
    scheduler.tick();
    scheduler.tick();
    inputs = {};
    inputs.registerLoadDone = true;
    scheduler.tick(inputs);
    scheduler.tick();
    ASSERT_EQ(scheduler.coreState(), RtlCoreState::ReuseLoad);

    inputs = {};
    inputs.loadDone = true;
    scheduler.tick(inputs);
    scheduler.tick(inputs);
    ASSERT_EQ(scheduler.coreState(), RtlCoreState::DOut);
    inputs = {};
    inputs.updateFinished = true;
    scheduler.tick(inputs);
    EXPECT_EQ(scheduler.coreState(), RtlCoreState::ReuseLoad);
    EXPECT_EQ(scheduler.instructionState(), RtlInstructionState::Loop);

    inputs = {};
    inputs.loadDone = true;
    scheduler.tick(inputs);
    EXPECT_EQ(scheduler.coreState(), RtlCoreState::DOut);
    EXPECT_EQ(scheduler.instructionState(), RtlInstructionState::Done);
    scheduler.tick();
    EXPECT_EQ(scheduler.coreState(), RtlCoreState::DOut);
    inputs = {};
    inputs.updateFinished = true;
    scheduler.tick(inputs);
    EXPECT_EQ(scheduler.coreState(), RtlCoreState::RegisterUnload);
}

TEST(RtlStreamLoadSkeleton, EmitsOneLoadDoneForA32BeatVerticalBurst)
{
    RtlStreamLoadSkeleton stream({1, 32, 1, 1});
    RtlStreamLoadInputs inputs;
    inputs.start = true;
    stream.tick(inputs);

    inputs = {};
    inputs.coreState = RtlCoreState::ReuseLoad;
    inputs.inputSwitch = 1;
    inputs.nextExecuteStart = true;
    stream.tick(inputs);
    inputs.nextExecuteStart = false;

    uint32_t readCycles = 0;
    uint32_t readLastCycles = 0;
    uint32_t loadDoneCycle = 0;
    for (uint32_t cycle = 2; cycle < 80; ++cycle) {
        const auto beforeEdge = stream.outputs(inputs);
        stream.tick(inputs);
        const auto afterEdge = stream.outputs(inputs);
        readCycles += afterEdge.readEnable ? 1 : 0;
        readLastCycles += afterEdge.readLast ? 1 : 0;
        if (beforeEdge.loadDone) {
            loadDoneCycle = cycle;
            break;
        }
    }

    EXPECT_EQ(readCycles, 32U);
    EXPECT_EQ(readLastCycles, 1U);
    EXPECT_EQ(loadDoneCycle, 34U);
}

TEST(RtlStreamLoadSkeleton, RetriggersEachFlowFromDelayedReadLast)
{
    RtlStreamLoadSkeleton stream({1, 32, 2, 1});
    RtlStreamLoadInputs inputs;
    inputs.start = true;
    stream.tick(inputs);

    inputs = {};
    inputs.coreState = RtlCoreState::ReuseLoad;
    inputs.inputSwitch = 1;
    inputs.nextExecuteStart = true;
    stream.tick(inputs);
    inputs.nextExecuteStart = false;

    uint32_t readCycles = 0;
    uint32_t readLastCycles = 0;
    uint32_t doneCycles[2] = {};
    uint32_t doneCount = 0;
    for (uint32_t cycle = 2; cycle < 120 && doneCount < 2; ++cycle) {
        const auto beforeEdge = stream.outputs(inputs);
        stream.tick(inputs);
        const auto afterEdge = stream.outputs(inputs);
        readCycles += afterEdge.readEnable ? 1 : 0;
        readLastCycles += afterEdge.readLast ? 1 : 0;
        if (beforeEdge.loadDone) {
            doneCycles[doneCount++] = cycle;
        }
    }

    ASSERT_EQ(doneCount, 2U);
    EXPECT_EQ(doneCycles[0], 34U);
    EXPECT_EQ(doneCycles[1], 67U);
    EXPECT_EQ(readCycles, 64U);
    EXPECT_EQ(readLastCycles, 2U);
}

TEST(RtlStreamLoadSkeleton, RejectsShapesNotYetValidatedAgainstRtl)
{
    EXPECT_THROW((RtlStreamLoadSkeleton({2, 32, 1, 1})),
                 std::invalid_argument);
    EXPECT_THROW((RtlStreamLoadSkeleton({1, 16, 1, 1})),
                 std::invalid_argument);
}

TEST(RtlStreamLoadSkeleton, DrivesBaselineSchedulerToFirstDOut)
{
    RtlSchedulerSkeleton scheduler({32, 8, 8, 1, 1, 0, false});
    RtlResidentLoadSkeleton resident({8, 32, 1});
    RtlStreamLoadSkeleton stream({1, 32, 8, 8});

    uint32_t registerLoadCycle = 0;
    uint32_t transposeLoadCycle = 0;
    uint32_t reuseLoadCycle = 0;
    uint32_t transposeClipCycle = 0;
    uint32_t dOutCycle = 0;
    uint32_t dOutExitCycle = 0;
    bool sawDOut = false;
    for (uint32_t cycle = 0; cycle < 600; ++cycle) {
        // All three components sample the same pre-edge register snapshot.
        const bool start = scheduler.start();
        RtlStreamLoadInputs streamInputs;
        streamInputs.start = start;
        streamInputs.coreState = scheduler.coreState();
        streamInputs.inputSwitch = scheduler.inputSwitch();
        streamInputs.lastFlowTime = scheduler.lastFlowTime();
        streamInputs.nextExecuteStart = scheduler.nextExecuteStart();
        streamInputs.registerRequestValid = resident.requestValid();
        streamInputs.registerRequestLast = resident.requestLast();

        RtlSchedulerInputs schedulerInputs;
        schedulerInputs.startWrite = cycle == 0;
        schedulerInputs.loadDone = stream.outputs(streamInputs).loadDone;
        schedulerInputs.registerLoadDone = resident.requestLast();

        const auto oldCore = scheduler.coreState();
        scheduler.tick(schedulerInputs);
        resident.tick(start);
        stream.tick(streamInputs);

        const auto newCore = scheduler.coreState();
        if (newCore != oldCore) {
            switch (newCore) {
              case RtlCoreState::RegisterLoad:
                registerLoadCycle = cycle;
                break;
              case RtlCoreState::TransposeLoad:
                transposeLoadCycle = cycle;
                break;
              case RtlCoreState::ReuseLoad:
                if (reuseLoadCycle == 0) {
                    reuseLoadCycle = cycle;
                }
                break;
              case RtlCoreState::TransposeClip:
                transposeClipCycle = cycle;
                break;
              case RtlCoreState::DOut:
                dOutCycle = cycle;
                sawDOut = true;
                break;
              case RtlCoreState::Idle:
              case RtlCoreState::FirstLoad:
              case RtlCoreState::RegisterUnload:
                break;
            }
        }
        if (sawDOut && oldCore == RtlCoreState::DOut &&
            newCore == RtlCoreState::ReuseLoad) {
            dOutExitCycle = cycle;
            break;
        }
    }

    // The 2026-07-22 current-baseline FSDB records these command-relative
    // state edges and the immediate non-final D_OUT exit at edge 555.
    EXPECT_EQ(registerLoadCycle, 2U);
    EXPECT_EQ(transposeLoadCycle, 258U);
    EXPECT_EQ(reuseLoadCycle, 290U);
    EXPECT_EQ(transposeClipCycle, 521U);
    EXPECT_EQ(dOutCycle, 554U);
    EXPECT_EQ(dOutExitCycle, 555U);
}

TEST(RtlSaEnableSkeleton, PreservesPreEdgeSamplingAtExecuteBoundary)
{
    RtlSaEnableSkeleton feeder;
    RtlExecuteUpdateSkeleton execute({32, 8, 0, false});
    RtlSaEnableInputs feederInputs;
    RtlExecuteUpdateInputs executeInputs;

    // Prime EN_i_d while input_switch_i still selects no execute input.
    feederInputs.dataAValid = true;
    executeInputs.enable = feeder.saEnable(feederInputs);
    execute.tick(executeInputs);
    feeder.tick(feederInputs);
    EXPECT_TRUE(feeder.inputEnableDelayed());
    EXPECT_EQ(execute.calculationCount(), 0U);

    // The switch becomes visible after this edge. SA_ENGINE sampled the old
    // combinational sa_en_i, so the counter still has not advanced.
    executeInputs.enable = feeder.saEnable(feederInputs);
    execute.tick(executeInputs);
    feeder.tick(feederInputs);
    feederInputs.inputSwitch = 0x1;
    EXPECT_TRUE(feeder.saEnable(feederInputs));
    EXPECT_EQ(execute.calculationCount(), 0U);

    // The following edge is the first accepted SA enable.
    executeInputs.enable = feeder.saEnable(feederInputs);
    execute.tick(executeInputs);
    feeder.tick(feederInputs);
    EXPECT_EQ(execute.calculationCount(), 1U);

    // A one-edge A/B-valid bubble remains visible through EN_i_d for the
    // current edge and stalls the calculation on the following edge.
    feederInputs.dataAValid = false;
    executeInputs.enable = feeder.saEnable(feederInputs);
    execute.tick(executeInputs);
    feeder.tick(feederInputs);
    EXPECT_EQ(execute.calculationCount(), 2U);
    EXPECT_FALSE(feeder.saEnable(feederInputs));

    executeInputs.enable = feeder.saEnable(feederInputs);
    execute.tick(executeInputs);
    feeder.tick(feederInputs);
    EXPECT_EQ(execute.calculationCount(), 2U);
}

TEST(RtlResultSerializerSkeleton, PipelinesInternalFinishToResultLast)
{
    RtlResultSerializerSkeleton serializer({32, 4, 4});
    uint32_t firstMacroCycle = 0;
    uint32_t storageReadyCycle = 0;
    uint32_t rowValidCycle = 0;
    uint32_t transposerReadyCycle = 0;
    uint32_t transposerValidCycle = 0;
    uint32_t resultValidCycle = 0;
    uint32_t transposerLastCycle = 0;
    uint32_t resultLastCycle = 0;
    uint32_t resultValidCycles = 0;

    for (uint32_t cycle = 0; cycle < 100; ++cycle) {
        RtlResultSerializerInputs inputs;
        inputs.internalFinish = cycle == 0;
        serializer.tick(inputs);

        if (serializer.firstMacroValid() && firstMacroCycle == 0) {
            firstMacroCycle = cycle;
        }
        if (serializer.engineStorageReady() && storageReadyCycle == 0) {
            storageReadyCycle = cycle;
        }
        if (serializer.rowScoreValid() && rowValidCycle == 0) {
            rowValidCycle = cycle;
        }
        if (serializer.transposerReadyOut() && transposerReadyCycle == 0) {
            transposerReadyCycle = cycle;
        }
        if (serializer.transposerValid() && transposerValidCycle == 0) {
            transposerValidCycle = cycle;
        }
        if (serializer.resultValid() && resultValidCycle == 0) {
            resultValidCycle = cycle;
        }
        if (serializer.transposerLast() && transposerLastCycle == 0) {
            transposerLastCycle = cycle;
        }
        if (serializer.resultLast() && resultLastCycle == 0) {
            resultLastCycle = cycle;
        }
        resultValidCycles += serializer.resultValid() ? 1 : 0;
    }

    // With cycle 0 representing the edge that samples internal_finish, the
    // offsets correspond to RTL command edges 566, 574, 575, 578, 610, 611,
    // 612, 642, and 643 respectively.
    EXPECT_EQ(firstMacroCycle, 8U);
    EXPECT_EQ(storageReadyCycle, 9U);
    EXPECT_EQ(rowValidCycle, 12U);
    EXPECT_EQ(transposerReadyCycle, 44U);
    EXPECT_EQ(transposerValidCycle, 45U);
    EXPECT_EQ(resultValidCycle, 46U);
    EXPECT_EQ(transposerLastCycle, 76U);
    EXPECT_EQ(resultLastCycle, 77U);
    EXPECT_EQ(resultValidCycles, 32U);
}

TEST(RtlResultSerializerSkeleton, RejectsUnsupportedGeometry)
{
    EXPECT_THROW((RtlResultSerializerSkeleton({16, 4, 4})),
                 std::invalid_argument);
    EXPECT_THROW((RtlResultSerializerSkeleton({32, 8, 4})),
                 std::invalid_argument);
}

TEST(RtlOutputWritebackSkeleton, DrainsBaselineResultTileAndPipelinesDone)
{
    // The passing 64x256x256 RTL command exposes these values directly at
    // u_register_file_out: accumulation 1x32x1x8, output address 8x32x1.
    RtlOutputWritebackSkeleton output({1, 32, 1, 8, 8, 32, 1});
    RtlOutputWritebackInputs inputs;
    inputs.coreState = RtlCoreState::DOut;

    for (uint32_t token = 0; token < 255; ++token) {
        inputs.resultValid = true;
        output.tick(inputs);
        EXPECT_FALSE(output.resultAccumDone());
    }
    output.tick(inputs);
    EXPECT_TRUE(output.resultAccumDone());

    inputs.resultValid = false;
    inputs.coreState = RtlCoreState::RegisterUnload;
    uint32_t firstWriteCycle = 0;
    uint32_t dataLastCycle = 0;
    uint32_t writeFinishedCycle = 0;
    uint32_t writeCycles = 0;
    for (uint32_t cycle = 0; cycle < 300; ++cycle) {
        output.tick(inputs);
        if (output.writeValid() && firstWriteCycle == 0) {
            firstWriteCycle = cycle;
        }
        if (output.writeDataLast()) {
            dataLastCycle = cycle;
        }
        if (output.writeFinished()) {
            writeFinishedCycle = cycle;
        }
        writeCycles += output.writeValid() ? 1 : 0;
    }

    EXPECT_EQ(firstWriteCycle, 4U);
    EXPECT_EQ(dataLastCycle, 259U);
    EXPECT_EQ(writeFinishedCycle, 263U);
    EXPECT_EQ(writeCycles, 256U);

    inputs.coreState = RtlCoreState::Idle;
    output.tick(inputs);
    EXPECT_FALSE(output.resultAccumDone());
}

TEST(RtlOutputWritebackSkeleton, RejectsInvalidAccumulationDimensions)
{
    EXPECT_THROW((RtlOutputWritebackSkeleton({0, 32, 1, 8, 8, 32, 1})),
                 std::invalid_argument);
    EXPECT_THROW((RtlOutputWritebackSkeleton({1, 32, 1, 65, 8, 32, 1})),
                 std::invalid_argument);
}

TEST(RtlOutputWritebackSkeleton, DrivesSchedulerCompletionAfterNativeDrain)
{
    RtlSchedulerSkeleton scheduler({1, 1, 1, 1, 1, 0, false});
    RtlOutputWritebackSkeleton output({1, 32, 1, 8, 8, 32, 1});

    RtlSchedulerInputs schedulerInputs;
    schedulerInputs.startWrite = true;
    scheduler.tick(schedulerInputs);
    scheduler.tick();
    scheduler.tick();
    schedulerInputs = {};
    schedulerInputs.registerLoadDone = true;
    scheduler.tick(schedulerInputs);
    scheduler.tick();
    schedulerInputs = {};
    schedulerInputs.loadDone = true;
    scheduler.tick(schedulerInputs);
    scheduler.tick(schedulerInputs);
    ASSERT_EQ(scheduler.coreState(), RtlCoreState::DOut);

    RtlOutputWritebackInputs outputInputs;
    outputInputs.coreState = RtlCoreState::DOut;
    outputInputs.resultValid = true;
    for (uint32_t token = 0; token < 256; ++token) {
        scheduler.tick();
        output.tick(outputInputs);
    }
    ASSERT_TRUE(output.resultAccumDone());

    schedulerInputs = {};
    schedulerInputs.updateFinished = true;
    scheduler.tick(schedulerInputs);
    outputInputs.resultValid = false;
    output.tick(outputInputs);
    ASSERT_EQ(scheduler.coreState(), RtlCoreState::RegisterUnload);

    uint32_t commandDoneCycle = 0;
    uint32_t writeCycles = 0;
    for (uint32_t cycle = 0; cycle < 280; ++cycle) {
        schedulerInputs = {};
        schedulerInputs.writeFinished = output.writeFinished();
        outputInputs.coreState = scheduler.coreState();
        scheduler.tick(schedulerInputs);
        output.tick(outputInputs);

        writeCycles += output.writeValid() ? 1 : 0;
        if (scheduler.commandDone()) {
            commandDoneCycle = cycle;
            break;
        }
    }

    // REGISTER_UNLOAD is already visible before cycle 0. RTL raises the
    // native write-finished pulse 264 edges later and command done one edge
    // after that, matching edges 2507 -> 2771 -> 2772 in the passing FSDB.
    EXPECT_EQ(commandDoneCycle, 264U);
    EXPECT_EQ(writeCycles, 256U);
    EXPECT_EQ(scheduler.coreState(), RtlCoreState::Idle);
}

TEST(RtlSramWriteTransportSkeleton, PipelinesAllNativeWritesIntoSram)
{
    RtlSramWriteTransportSkeleton transport;
    RtlSramWriteTransportInputs inputs;
    inputs.crossbarStart = true;
    transport.tick(inputs);
    ASSERT_TRUE(transport.crossbarActive());

    uint32_t firstAcceptedCycle = 0;
    uint32_t finalAcceptedCycle = 0;
    uint32_t acceptedWrites = 0;
    for (uint32_t cycle = 0; cycle < 260; ++cycle) {
        inputs = {};
        inputs.nativeWriteValid = cycle < 256;
        inputs.nativeWriteLast = cycle == 255;
        transport.tick(inputs);

        if (transport.memoryWriteAccepted()) {
            if (acceptedWrites == 0) {
                firstAcceptedCycle = cycle;
            }
            ++acceptedWrites;
        }
        if (transport.memoryWriteLast()) {
            finalAcceptedCycle = cycle;
        }
    }

    // Given a native valid sampled at cycle 0, mem_ctrl drives the master at
    // cycle 0, crossbar drives the slave at cycle 1, and SRAM samples it at
    // cycle 2. The tail drains through the same two registered boundaries.
    EXPECT_EQ(firstAcceptedCycle, 2U);
    EXPECT_EQ(finalAcceptedCycle, 257U);
    EXPECT_EQ(acceptedWrites, 256U);

    inputs = {};
    inputs.crossbarDone = true;
    transport.tick(inputs);
    EXPECT_FALSE(transport.crossbarActive());
}

TEST(RtlSramWriteTransportSkeleton, DoesNotForwardWhileCrossbarIsIdle)
{
    RtlSramWriteTransportSkeleton transport;
    RtlSramWriteTransportInputs inputs;
    inputs.nativeWriteValid = true;
    inputs.nativeWriteLast = true;
    transport.tick(inputs);
    EXPECT_TRUE(transport.memCtrlWriteValid());
    EXPECT_FALSE(transport.crossbarSlaveValid());
    EXPECT_FALSE(transport.memoryWriteAccepted());

    transport.tick();
    transport.tick();
    EXPECT_FALSE(transport.memoryWriteAccepted());
}

TEST(RtlResidentFillSkeleton, DrainsResidentTailPastSchedulerTransition)
{
    RtlSchedulerSkeleton scheduler({32, 8, 8, 1, 1, 0, false});
    RtlResidentLoadSkeleton resident({8, 32, 1});
    RtlResidentFillSkeleton fill({3});

    uint32_t firstMemoryCycle = 0;
    uint32_t memoryLastCycle = 0;
    uint32_t firstFeederWriteCycle = 0;
    uint32_t firstResidentWriteCycle = 0;
    uint32_t lastResidentWriteCycle = 0;
    uint32_t memoryCycles = 0;
    uint32_t feederWriteCycles = 0;
    uint32_t residentWriteCycles = 0;
    uint32_t transposeCycle = 0;

    for (uint32_t cycle = 0; cycle < 300; ++cycle) {
        const bool start = scheduler.start();
        RtlSchedulerInputs schedulerInputs;
        schedulerInputs.startWrite = cycle == 0;
        schedulerInputs.registerLoadDone = resident.requestLast();

        RtlResidentFillInputs fillInputs;
        fillInputs.readRequestValid = resident.requestValid();
        fillInputs.readRequestLast = resident.requestLast();
        fillInputs.coreState = scheduler.coreState();

        scheduler.tick(schedulerInputs);
        resident.tick(start);
        fill.tick(fillInputs);

        if (fill.memoryDataValid()) {
            if (memoryCycles == 0) {
                firstMemoryCycle = cycle;
            }
            ++memoryCycles;
        }
        if (fill.memoryDataLast()) {
            memoryLastCycle = cycle;
        }
        if (fill.registerFileInputValid()) {
            if (feederWriteCycles == 0) {
                firstFeederWriteCycle = cycle;
            }
            ++feederWriteCycles;
        }
        if (fill.residentWriteValid()) {
            if (residentWriteCycles == 0) {
                firstResidentWriteCycle = cycle;
            }
            lastResidentWriteCycle = cycle;
            ++residentWriteCycles;
        }
        if (scheduler.coreState() == RtlCoreState::TransposeLoad &&
            transposeCycle == 0) {
            transposeCycle = cycle;
        }
    }

    EXPECT_EQ(firstMemoryCycle, 7U);
    EXPECT_EQ(memoryLastCycle, 262U);
    EXPECT_EQ(firstFeederWriteCycle, 8U);
    EXPECT_EQ(firstResidentWriteCycle, 9U);
    EXPECT_EQ(lastResidentWriteCycle, 264U);
    EXPECT_EQ(memoryCycles, 256U);
    EXPECT_EQ(feederWriteCycles, 256U);
    EXPECT_EQ(residentWriteCycles, 256U);
    EXPECT_EQ(transposeCycle, 258U);
}

TEST(RtlResidentFillSkeleton, RejectsInvalidSramDelay)
{
    EXPECT_THROW((RtlResidentFillSkeleton({0})), std::invalid_argument);
}

TEST(RtlInputFeederSkeleton, ReproducesCurrentRtlInputBoundaryEdges)
{
    RtlSchedulerSkeleton scheduler({32, 8, 8, 1, 1, 0, false});
    RtlResidentLoadSkeleton resident({8, 32, 1});
    RtlStreamLoadSkeleton stream({1, 32, 8, 8});
    RtlInputFeederSkeleton feeder({1, 32, 8, 8, 32, 3, 2, 2, 2});
    uint32_t firstReadEnableCycle = 0;
    uint32_t secondReadEnableCycle = 0;
    uint32_t readEnableCount = 0;
    uint32_t firstReadValidCycle = 0;
    uint32_t firstReadLastCycle = 0;
    uint32_t firstAValidCycle = 0;
    uint32_t firstBValidCycle = 0;
    uint32_t firstInputSwitchCycle = 0;

    for (uint32_t cycle = 0; cycle < 340; ++cycle) {
        const bool start = scheduler.start();
        RtlStreamLoadInputs streamInputs;
        streamInputs.start = start;
        streamInputs.coreState = scheduler.coreState();
        streamInputs.inputSwitch = scheduler.inputSwitch();
        streamInputs.lastFlowTime = scheduler.lastFlowTime();
        streamInputs.nextExecuteStart = scheduler.nextExecuteStart();
        streamInputs.registerRequestValid = resident.requestValid();
        streamInputs.registerRequestLast = resident.requestLast();

        RtlSchedulerInputs schedulerInputs;
        schedulerInputs.startWrite = cycle == 0;
        schedulerInputs.loadDone = stream.outputs(streamInputs).loadDone;
        schedulerInputs.registerLoadDone = resident.requestLast();

        RtlInputFeederInputs inputs;
        inputs.coreState = scheduler.coreState();
        inputs.inputSwitch = scheduler.inputSwitch();
        // The memory producer makes B visible after edge 297. The feeder
        // samples that registered value from the pre-edge snapshot at 298.
        inputs.memoryDataValid = cycle >= 298 && cycle < 330;

        scheduler.tick(schedulerInputs);
        resident.tick(start);
        stream.tick(streamInputs);
        feeder.tick(inputs);

        if (feeder.registerFileReadEnable()) {
            if (readEnableCount == 0) {
                firstReadEnableCycle = cycle;
            } else if (readEnableCount == 1) {
                secondReadEnableCycle = cycle;
            }
            ++readEnableCount;
        }
        if (feeder.registerFileReadValid() && firstReadValidCycle == 0) {
            firstReadValidCycle = cycle;
        }
        if (feeder.registerFileReadLast() && firstReadLastCycle == 0) {
            firstReadLastCycle = cycle;
        }
        if (feeder.dataAValid() && firstAValidCycle == 0) {
            firstAValidCycle = cycle;
        }
        if (feeder.dataBValid() && firstBValidCycle == 0) {
            firstBValidCycle = cycle;
        }
        if (feeder.outputInputSwitch() != 0 &&
            firstInputSwitchCycle == 0) {
            firstInputSwitchCycle = cycle;
        }
    }

    EXPECT_EQ(firstReadEnableCycle, 266U);
    EXPECT_EQ(firstReadValidCycle, 267U);
    EXPECT_EQ(firstAValidCycle, 269U);
    EXPECT_EQ(firstReadLastCycle, 298U);
    EXPECT_EQ(secondReadEnableCycle, 298U);
    EXPECT_EQ(firstBValidCycle, 301U);
    EXPECT_EQ(firstInputSwitchCycle, 302U);
}

TEST(RtlInputFeederSkeleton, FeedsSaOneEdgeAfterSwitchBecomesVisible)
{
    RtlInputFeederSkeleton feeder({1, 32, 8, 8, 32, 3, 2, 2, 2});
    RtlSaEnableSkeleton saEnable;
    RtlExecuteUpdateSkeleton execute({32, 8, 0, false});
    uint32_t saEnableCycle = 0;
    uint32_t firstSaSampleCycle = 0;

    for (uint32_t cycle = 0; cycle < 320; ++cycle) {
        RtlInputFeederInputs feederInputs;
        if (cycle >= 259 && cycle < 291) {
            feederInputs.coreState = RtlCoreState::TransposeLoad;
        } else if (cycle >= 291) {
            feederInputs.coreState = RtlCoreState::ReuseLoad;
        } else {
            feederInputs.coreState = RtlCoreState::RegisterLoad;
        }
        feederInputs.inputSwitch = cycle >= 292 ? 0x1 : 0;
        feederInputs.memoryDataValid = cycle >= 298;

        RtlSaEnableInputs saInputs;
        saInputs.dataAValid = feeder.dataAValid();
        saInputs.dataBValid = feeder.dataBValid();
        saInputs.inputSwitch = feeder.outputInputSwitch();
        RtlExecuteUpdateInputs executeInputs;
        executeInputs.enable = saEnable.saEnable(saInputs);

        execute.tick(executeInputs);
        saEnable.tick(saInputs);
        feeder.tick(feederInputs);

        RtlSaEnableInputs visibleInputs;
        visibleInputs.dataAValid = feeder.dataAValid();
        visibleInputs.dataBValid = feeder.dataBValid();
        visibleInputs.inputSwitch = feeder.outputInputSwitch();
        if (saEnable.saEnable(visibleInputs) && saEnableCycle == 0) {
            saEnableCycle = cycle;
        }
        if (execute.calculationCount() != 0 && firstSaSampleCycle == 0) {
            firstSaSampleCycle = cycle;
        }
    }

    EXPECT_EQ(saEnableCycle, 302U);
    EXPECT_EQ(firstSaSampleCycle, 303U);
}

TEST(RtlInputFeederSkeleton, RejectsInvalidConfiguration)
{
    EXPECT_THROW((RtlInputFeederSkeleton({0, 32, 8, 8, 32, 3, 2, 2, 2})),
                 std::invalid_argument);
    EXPECT_THROW((RtlInputFeederSkeleton({1, 32, 8, 8, 32, 3, 2, 2, 0})),
                 std::invalid_argument);
}

TEST(RtlCommandDriverSkeleton, RunsBaselineCsrShapeToCommandDone)
{
    RtlCommandDriverSkeleton driver(commandDriverConfig());
    uint32_t registerLoadCycle = 0;
    uint32_t transposeLoadCycle = 0;
    uint32_t reuseLoadCycle = 0;
    uint32_t firstResidentReadRequestCycle = 0;
    uint32_t finalResidentReadRequestCycle = 0;
    uint32_t firstRfReadCycle = 0;
    uint32_t firstRfValidCycle = 0;
    uint32_t firstAValidCycle = 0;
    uint32_t firstStreamReadCycle = 0;
    uint32_t firstStreamMemoryRequestCycle = 0;
    uint32_t finalStreamMemoryRequestCycle = 0;
    uint32_t firstMemoryDataCycle = 0;
    uint32_t firstBValidCycle = 0;
    uint32_t inputSwitchCycle = 0;
    uint32_t saEnableCycle = 0;
    uint32_t firstSaSampleCycle = 0;
    uint32_t firstDOutCycle = 0;
    uint32_t firstInternalFinishCycle = 0;
    uint32_t firstResultCycle = 0;
    uint32_t finalResultLastCycle = 0;
    uint32_t registerUnloadCycle = 0;
    uint32_t firstNativeWriteCycle = 0;
    uint32_t firstMemoryWriteRequestCycle = 0;
    uint32_t firstPhysicalWriteCycle = 0;
    uint32_t finalNativeWriteCycle = 0;
    uint32_t finalMemoryWriteRequestCycle = 0;
    uint32_t finalPhysicalWriteCycle = 0;
    uint32_t commandDoneCycle = 0;
    uint64_t previousAcceptedSaTokens = 0;
    RtlCoreState previousCore = RtlCoreState::Idle;

    for (uint32_t cycle = 0; cycle < 3000; ++cycle) {
        driver.tick(cycle == 0);

        if (driver.coreState() != previousCore) {
            switch (driver.coreState()) {
              case RtlCoreState::RegisterLoad:
                if (registerLoadCycle == 0) {
                    registerLoadCycle = cycle;
                }
                break;
              case RtlCoreState::TransposeLoad:
                if (transposeLoadCycle == 0) {
                    transposeLoadCycle = cycle;
                }
                break;
              case RtlCoreState::ReuseLoad:
                if (reuseLoadCycle == 0) {
                    reuseLoadCycle = cycle;
                }
                break;
              case RtlCoreState::DOut:
                if (firstDOutCycle == 0) {
                    firstDOutCycle = cycle;
                }
                break;
              case RtlCoreState::RegisterUnload:
                if (registerUnloadCycle == 0) {
                    registerUnloadCycle = cycle;
                }
                break;
              case RtlCoreState::Idle:
              case RtlCoreState::TransposeClip:
              case RtlCoreState::FirstLoad:
                break;
            }
        }
        previousCore = driver.coreState();

        if (driver.memoryReadRequestValid() &&
            !driver.memoryReadRequestIsStream()) {
            if (firstResidentReadRequestCycle == 0) {
                firstResidentReadRequestCycle = cycle;
            }
            finalResidentReadRequestCycle = cycle;
        }
        if (driver.registerFileReadEnable() && firstRfReadCycle == 0) {
            firstRfReadCycle = cycle;
        }
        if (driver.registerFileReadValid() && firstRfValidCycle == 0) {
            firstRfValidCycle = cycle;
        }
        if (driver.dataAValid() && firstAValidCycle == 0) {
            firstAValidCycle = cycle;
        }
        if (driver.streamReadEnable() && firstStreamReadCycle == 0) {
            firstStreamReadCycle = cycle;
        }
        if (driver.memoryReadRequestValid() &&
            driver.memoryReadRequestIsStream()) {
            if (firstStreamMemoryRequestCycle == 0) {
                firstStreamMemoryRequestCycle = cycle;
            }
            finalStreamMemoryRequestCycle = cycle;
        }
        if (driver.memoryDataValid() && firstMemoryDataCycle == 0 &&
            cycle > 264) {
            firstMemoryDataCycle = cycle;
        }
        if (driver.dataBValid() && firstBValidCycle == 0) {
            firstBValidCycle = cycle;
        }
        if (driver.outputInputSwitch() != 0 && inputSwitchCycle == 0) {
            inputSwitchCycle = cycle;
        }
        if (driver.saEnable() && saEnableCycle == 0) {
            saEnableCycle = cycle;
        }
        if (driver.acceptedSaTokens() != previousAcceptedSaTokens &&
            firstSaSampleCycle == 0) {
            firstSaSampleCycle = cycle;
        }
        previousAcceptedSaTokens = driver.acceptedSaTokens();
        if (driver.internalFinish() && firstInternalFinishCycle == 0) {
            firstInternalFinishCycle = cycle;
        }
        if (driver.resultValid() && firstResultCycle == 0) {
            firstResultCycle = cycle;
        }
        if (driver.resultLast()) {
            finalResultLastCycle = cycle;
        }
        if (driver.nativeWriteValid() && firstNativeWriteCycle == 0) {
            firstNativeWriteCycle = cycle;
        }
        if (driver.memoryWriteRequestValid() &&
            firstMemoryWriteRequestCycle == 0) {
            firstMemoryWriteRequestCycle = cycle;
        }
        if (driver.physicalWriteAccepted() &&
            firstPhysicalWriteCycle == 0) {
            firstPhysicalWriteCycle = cycle;
        }
        if (driver.nativeWriteLast()) {
            finalNativeWriteCycle = cycle;
        }
        if (driver.memoryWriteRequestLast()) {
            finalMemoryWriteRequestCycle = cycle;
        }
        if (driver.physicalWriteLast()) {
            finalPhysicalWriteCycle = cycle;
        }
        if (driver.commandDone()) {
            commandDoneCycle = cycle;
            break;
        }
    }

    EXPECT_EQ(registerLoadCycle, 2U);
    EXPECT_EQ(firstResidentReadRequestCycle, 3U);
    EXPECT_EQ(finalResidentReadRequestCycle, 258U);
    EXPECT_EQ(transposeLoadCycle, 258U);
    EXPECT_EQ(firstRfReadCycle, 266U);
    EXPECT_EQ(firstRfValidCycle, 267U);
    EXPECT_EQ(firstAValidCycle, 269U);
    EXPECT_EQ(reuseLoadCycle, 290U);
    EXPECT_EQ(firstStreamReadCycle, 292U);
    EXPECT_EQ(firstStreamMemoryRequestCycle, 293U);
    EXPECT_EQ(finalStreamMemoryRequestCycle, 2417U);
    EXPECT_EQ(firstMemoryDataCycle, 297U);
    EXPECT_EQ(firstBValidCycle, 301U);
    EXPECT_EQ(inputSwitchCycle, 302U);
    EXPECT_EQ(saEnableCycle, 302U);
    EXPECT_EQ(firstSaSampleCycle, 303U);
    EXPECT_EQ(firstDOutCycle, 554U);
    EXPECT_EQ(firstInternalFinishCycle, 565U);
    EXPECT_EQ(firstResultCycle, 612U);
    EXPECT_EQ(finalResultLastCycle, 2505U);
    EXPECT_EQ(registerUnloadCycle, 2507U);
    EXPECT_EQ(firstNativeWriteCycle, 2512U);
    EXPECT_EQ(firstMemoryWriteRequestCycle, 2513U);
    EXPECT_EQ(firstPhysicalWriteCycle, 2515U);
    EXPECT_EQ(finalNativeWriteCycle, 2767U);
    EXPECT_EQ(finalMemoryWriteRequestCycle, 2768U);
    EXPECT_EQ(finalPhysicalWriteCycle, 2770U);
    EXPECT_EQ(commandDoneCycle, 2772U);

    EXPECT_EQ(driver.residentReadTokens(), 256U);
    EXPECT_EQ(driver.streamReadTokens(), 2048U);
    EXPECT_EQ(driver.registerFileReadTokens(), 2048U);
    EXPECT_EQ(driver.operandATokens(), 2048U);
    EXPECT_EQ(driver.operandBTokens(), 2048U);
    EXPECT_EQ(driver.acceptedSaTokens(), 2048U);
    EXPECT_EQ(driver.resultTokens(), 256U);
    EXPECT_EQ(driver.nativeWriteTokens(), 256U);
    EXPECT_EQ(driver.physicalWriteTokens(), 256U);

    EXPECT_EQ(driver.residentReadWindow().firstEdge, 3U);
    EXPECT_EQ(driver.residentReadWindow().lastEdge, 258U);
    EXPECT_EQ(driver.residentReadWindow().span(), 256U);
    EXPECT_EQ(driver.streamReadWindow().firstEdge, 293U);
    EXPECT_EQ(driver.streamReadWindow().lastEdge, 2417U);
    EXPECT_EQ(driver.streamReadWindow().span(), 2125U);
    EXPECT_EQ(driver.operandAWindow().firstEdge, 269U);
    EXPECT_EQ(driver.operandAWindow().lastEdge, 2392U);
    EXPECT_EQ(driver.operandAWindow().span(), 2124U);
    EXPECT_EQ(driver.operandBWindow().firstEdge, 301U);
    EXPECT_EQ(driver.operandBWindow().lastEdge, 2425U);
    EXPECT_EQ(driver.operandBWindow().span(), 2125U);
    EXPECT_EQ(driver.resultWindow().firstEdge, 612U);
    EXPECT_EQ(driver.resultWindow().lastEdge, 2505U);
    EXPECT_EQ(driver.resultWindow().span(), 1894U);
    EXPECT_EQ(driver.memoryWriteWindow().firstEdge, 2513U);
    EXPECT_EQ(driver.memoryWriteWindow().lastEdge, 2768U);
    EXPECT_EQ(driver.memoryWriteWindow().span(), 256U);
    EXPECT_TRUE(driver.commandDoneObserved());
    EXPECT_EQ(driver.commandDoneEdge(), 2772U);
}

TEST(RtlCommandDriverSkeleton, RejectsInconsistentComponentConfiguration)
{
    auto config = commandDriverConfig();
    config.execute.flowLoops = 7;
    EXPECT_THROW((RtlCommandDriverSkeleton(config)), std::invalid_argument);
}

TEST(RtlCommandDriverSkeleton, ConservesCurrentFlowInstructionShapes)
{
    struct Shape
    {
        uint32_t flowTimes;
        uint32_t instructionTimes;
    };
    constexpr Shape Shapes[] = {
        {1, 1},
        {1, 8},
        {4, 8},
        {8, 1},
        {8, 4},
        {8, 8},
    };

    for (const auto &shape : Shapes) {
        RtlCommandDriverSkeleton driver(
            commandDriverConfig(shape.flowTimes, shape.instructionTimes));
        for (uint32_t cycle = 0;
             cycle < 20000 && !driver.commandDone(); ++cycle) {
            driver.tick(cycle == 0);
        }

        const uint64_t residentTokens =
            static_cast<uint64_t>(shape.flowTimes) * 32;
        const uint64_t inputTokens = residentTokens *
            shape.instructionTimes;
        const uint64_t operandATokens = inputTokens +
            (residentTokens == 32 ? 32 : 0);
        const uint64_t outputTokens =
            static_cast<uint64_t>(shape.instructionTimes) * 32;
        EXPECT_TRUE(driver.commandDone())
            << "flow=" << shape.flowTimes
            << " instructions=" << shape.instructionTimes;
        EXPECT_EQ(driver.residentReadTokens(), residentTokens);
        EXPECT_EQ(driver.streamReadTokens(), inputTokens);
        EXPECT_EQ(driver.operandATokens(), operandATokens);
        EXPECT_EQ(driver.operandBTokens(), inputTokens);
        EXPECT_EQ(driver.acceptedSaTokens(), inputTokens);
        EXPECT_EQ(driver.resultTokens(), outputTokens);
        EXPECT_EQ(driver.nativeWriteTokens(), outputTokens);
        EXPECT_EQ(driver.physicalWriteTokens(), outputTokens);
        EXPECT_TRUE(driver.residentReadWindow().observed);
        EXPECT_TRUE(driver.streamReadWindow().observed);
        EXPECT_TRUE(driver.operandAWindow().observed);
        EXPECT_TRUE(driver.operandBWindow().observed);
        EXPECT_TRUE(driver.resultWindow().observed);
        EXPECT_TRUE(driver.memoryWriteWindow().observed);
        EXPECT_TRUE(driver.commandDoneObserved());
    }
}

TEST(RtlCommandDriverSkeleton, FlowTransposeSharesNonRetainControlTiming)
{
    RtlCommandDriverSkeleton normal(commandDriverConfig(5, 2, 0));
    RtlCommandDriverSkeleton transpose(commandDriverConfig(5, 2, 1));

    for (uint32_t cycle = 0; cycle < 5000; ++cycle) {
        normal.tick(cycle == 0);
        transpose.tick(cycle == 0);

        EXPECT_EQ(transpose.coreState(), normal.coreState());
        EXPECT_EQ(transpose.streamReadEnable(), normal.streamReadEnable());
        EXPECT_EQ(transpose.dataAValid(), normal.dataAValid());
        EXPECT_EQ(transpose.dataBValid(), normal.dataBValid());
        EXPECT_EQ(transpose.saEnable(), normal.saEnable());
        EXPECT_EQ(transpose.resultValid(), normal.resultValid());
        EXPECT_EQ(transpose.nativeWriteValid(), normal.nativeWriteValid());
        EXPECT_EQ(transpose.commandDone(), normal.commandDone());
        if (normal.commandDone()) {
            break;
        }
    }

    EXPECT_TRUE(normal.commandDoneObserved());
    EXPECT_TRUE(transpose.commandDoneObserved());
    EXPECT_EQ(transpose.commandDoneEdge(), normal.commandDoneEdge());
    EXPECT_EQ(transpose.resultTokens(), normal.resultTokens());
    EXPECT_EQ(transpose.physicalWriteTokens(),
              normal.physicalWriteTokens());
}

TEST(RtlCommandDriverSkeleton, FlowRetainCompletesWithoutResultOrWriteback)
{
    RtlCommandDriverSkeleton retain(commandDriverConfig(8, 1, 2));

    for (uint32_t cycle = 0;
         cycle < 5000 && !retain.commandDone(); ++cycle) {
        retain.tick(cycle == 0);
    }

    EXPECT_TRUE(retain.commandDoneObserved());
    EXPECT_EQ(retain.commandDoneEdge(), 569u);
    EXPECT_EQ(retain.residentReadTokens(), 256u);
    EXPECT_EQ(retain.streamReadTokens(), 256u);
    EXPECT_EQ(retain.acceptedSaTokens(), 256u);
    EXPECT_EQ(retain.resultTokens(), 0u);
    EXPECT_EQ(retain.nativeWriteTokens(), 0u);
    EXPECT_EQ(retain.physicalWriteTokens(), 0u);
    EXPECT_FALSE(retain.resultWindow().observed);
    EXPECT_FALSE(retain.memoryWriteWindow().observed);
}

TEST(RtlExecuteUpdateSkeleton, CountsEnabledSaEdgesAndPipelinesExecuteDone)
{
    RtlExecuteUpdateSkeleton execute({32, 8, 0, false});
    RtlExecuteUpdateInputs inputs;
    inputs.enable = true;
    for (uint32_t count = 0; count < 128; ++count) {
        execute.tick(inputs);
    }
    EXPECT_EQ(execute.calculationCount(), 128U);

    inputs.enable = false;
    for (uint32_t bubble = 0; bubble < 7; ++bubble) {
        execute.tick(inputs);
    }
    EXPECT_EQ(execute.calculationCount(), 128U);

    inputs.enable = true;
    for (uint32_t count = 128; count < 256; ++count) {
        execute.tick(inputs);
    }
    EXPECT_EQ(execute.calculationCount(), 0U);
    EXPECT_TRUE(execute.internalFinish());
    EXPECT_FALSE(execute.executeDone());
    EXPECT_FALSE(execute.updateFinished());

    inputs.enable = false;
    execute.tick(inputs);
    EXPECT_FALSE(execute.executeDone());
    execute.tick(inputs);
    EXPECT_TRUE(execute.executeDone());
    EXPECT_FALSE(execute.updateFinished());
    execute.tick(inputs);
    EXPECT_FALSE(execute.executeDone());
    EXPECT_TRUE(execute.updateFinished());
    execute.tick(inputs);
    EXPECT_FALSE(execute.updateFinished());
}

TEST(RtlExecuteUpdateSkeleton, FinalInstructionWaitsForResultLast)
{
    RtlExecuteUpdateSkeleton execute({32, 1, 0, false});
    RtlResultSerializerSkeleton serializer({32, 4, 4});
    RtlExecuteUpdateInputs inputs;
    inputs.enable = true;
    inputs.currentInstructionOutput = true;
    for (uint32_t count = 0; count < 32; ++count) {
        RtlResultSerializerInputs serializerInputs;
        serializerInputs.internalFinish = execute.internalFinish();
        inputs.resultLast = serializer.resultLast();
        execute.tick(inputs);
        serializer.tick(serializerInputs);
    }
    inputs.enable = false;
    uint32_t resultLastCycle = 0;
    uint32_t updateFinishedCycle = 0;
    for (uint32_t cycle = 1; cycle < 100; ++cycle) {
        RtlResultSerializerInputs serializerInputs;
        serializerInputs.internalFinish = execute.internalFinish();
        inputs.resultLast = serializer.resultLast();
        execute.tick(inputs);
        serializer.tick(serializerInputs);

        if (serializer.resultLast() && resultLastCycle == 0) {
            resultLastCycle = cycle;
        }
        if (execute.updateFinished() && updateFinishedCycle == 0) {
            updateFinishedCycle = cycle;
        }
    }

    EXPECT_EQ(resultLastCycle, 78U);
    EXPECT_EQ(updateFinishedCycle, 79U);
}

TEST(RtlExecuteUpdateSkeleton, RejectsUnsupportedKernelSize)
{
    EXPECT_THROW((RtlExecuteUpdateSkeleton({32, 8, 3, false})),
                 std::invalid_argument);
    EXPECT_NO_THROW((RtlExecuteUpdateSkeleton({32, 8, 0, true})));
}

TEST(SauSchedule, RequiresResidentAAndTransposeCompletion)
{
    SauSchedule schedule;
    schedule.beginCommand();

    EXPECT_TRUE(schedule.canIssueRead(StreamKind::OperandA));
    EXPECT_FALSE(schedule.canIssueRead(StreamKind::OperandB));
    EXPECT_FALSE(schedule.canAdmitArrayA());
    EXPECT_FALSE(schedule.canAdmitArrayB());
    EXPECT_FALSE(schedule.onArrayAAdmitted());

    EXPECT_TRUE(schedule.onAReadAccepted(true));
    EXPECT_EQ(schedule.state(), SauScheduleState::TransposeSetup);
    EXPECT_TRUE(schedule.canIssueRead(StreamKind::OperandB));
    EXPECT_FALSE(schedule.canAdmitArrayB());

    // A resident token makes array admission legal, but it does not replace
    // scheduler.sv's SA_SIZE transpose counter guard.
    EXPECT_TRUE(schedule.onResidentReady());
    EXPECT_TRUE(schedule.canAdmitArrayA());
    EXPECT_TRUE(schedule.canAdmitArrayB());
    EXPECT_TRUE(schedule.onArrayAAdmitted());
    EXPECT_EQ(schedule.state(), SauScheduleState::TransposeSetup);
    EXPECT_TRUE(schedule.onTransposeComplete());
    EXPECT_EQ(schedule.state(), SauScheduleState::FlowExecute);
}

TEST(SauSchedule, HoldsAtFlowBoundaryUntilAnotherArrayAdmission)
{
    SauSchedule schedule;
    schedule.beginCommand();
    ASSERT_TRUE(schedule.onAReadAccepted(true));
    ASSERT_TRUE(schedule.onResidentReady());
    ASSERT_TRUE(schedule.onTransposeComplete());
    ASSERT_TRUE(schedule.onArrayBAdmitted(true, false));
    EXPECT_EQ(schedule.state(), SauScheduleState::FlowBoundary);

    // A full pipeline or empty B FIFO cannot call onArrayBAdmitted(), so the
    // semantic state remains at the boundary until an actual admission.
    EXPECT_EQ(schedule.state(), SauScheduleState::FlowBoundary);
    EXPECT_TRUE(schedule.onArrayBAdmitted(false, false));
    EXPECT_EQ(schedule.state(), SauScheduleState::FlowExecute);
}

TEST(SauSchedule, DrainsResultsAndWritesOnlyAfterFinalBWork)
{
    SauSchedule schedule;
    schedule.beginCommand();
    ASSERT_TRUE(schedule.onAReadAccepted(true));
    ASSERT_TRUE(schedule.onResidentReady());
    ASSERT_TRUE(schedule.onTransposeComplete());

    EXPECT_FALSE(schedule.canIssueWriteback());
    EXPECT_TRUE(schedule.onArrayBAdmitted(false, true));
    EXPECT_EQ(schedule.state(), SauScheduleState::DrainAndWriteback);
    EXPECT_FALSE(schedule.canAdmitArrayB());
    EXPECT_TRUE(schedule.canAdmitArrayA());
    EXPECT_TRUE(schedule.canReleaseResult());
    EXPECT_TRUE(schedule.canIssueWriteback());
    EXPECT_TRUE(schedule.onArrayAAdmitted());
    EXPECT_EQ(schedule.state(), SauScheduleState::DrainAndWriteback);

    EXPECT_NO_THROW(schedule.completeCommand());
    EXPECT_EQ(schedule.state(), SauScheduleState::Complete);
}

TEST(SauSchedule, RejectsCompletionBeforeWritebackDrain)
{
    SauSchedule schedule;
    schedule.beginCommand();

    EXPECT_THROW(schedule.completeCommand(), std::logic_error);
}

} // anonymous namespace
} // namespace gem5::sau
