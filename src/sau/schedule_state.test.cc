#include <gtest/gtest.h>

#include <stdexcept>

#include "sau/schedule_state.hh"

namespace gem5::sau
{
namespace
{

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
    EXPECT_TRUE(scheduler.updateFinishedLatched());
    EXPECT_FALSE(scheduler.commandDone());

    inputs = {};
    inputs.writeFinished = true;
    scheduler.tick(inputs);
    EXPECT_EQ(scheduler.coreState(), RtlCoreState::Idle);
    EXPECT_TRUE(scheduler.commandDone());
    EXPECT_FALSE(scheduler.updateFinishedLatched());
}

TEST(RtlSchedulerSkeleton, LatchesUpdateAcrossInstructionBoundaries)
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
    EXPECT_TRUE(scheduler.updateFinishedLatched());

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
                reuseLoadCycle = cycle;
                break;
              case RtlCoreState::TransposeClip:
                transposeClipCycle = cycle;
                break;
              case RtlCoreState::DOut:
                dOutCycle = cycle;
                break;
              case RtlCoreState::Idle:
              case RtlCoreState::FirstLoad:
              case RtlCoreState::RegisterUnload:
                break;
            }
        }
        if (dOutCycle != 0) {
            break;
        }
    }

    // Baseline diagnostic command 1 uses absolute start 28914 and records
    // these state edges at 28916, 29172, 29204, 29435, and 29468.
    EXPECT_EQ(registerLoadCycle, 2U);
    EXPECT_EQ(transposeLoadCycle, 258U);
    EXPECT_EQ(reuseLoadCycle, 290U);
    EXPECT_EQ(transposeClipCycle, 521U);
    EXPECT_EQ(dOutCycle, 554U);
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
    RtlExecuteUpdateInputs inputs;
    inputs.enable = true;
    inputs.currentInstructionOutput = true;
    for (uint32_t count = 0; count < 32; ++count) {
        execute.tick(inputs);
    }
    inputs.enable = false;
    execute.tick(inputs);
    execute.tick(inputs);
    ASSERT_TRUE(execute.executeDone());
    execute.tick(inputs);
    EXPECT_FALSE(execute.updateFinished());

    inputs.resultLast = true;
    execute.tick(inputs);
    EXPECT_TRUE(execute.updateFinished());
    inputs.resultLast = false;
    execute.tick(inputs);
    EXPECT_FALSE(execute.updateFinished());
}

TEST(RtlExecuteUpdateSkeleton, RejectsUnsupportedControlModes)
{
    EXPECT_THROW((RtlExecuteUpdateSkeleton({32, 8, 3, false})),
                 std::invalid_argument);
    EXPECT_THROW((RtlExecuteUpdateSkeleton({32, 8, 0, true})),
                 std::invalid_argument);
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
