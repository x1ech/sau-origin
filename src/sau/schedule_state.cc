#include "sau/schedule_state.hh"

#include <iterator>
#include <stdexcept>

namespace gem5::sau
{
namespace
{

constexpr SauScheduleStateMapping StateMappings[] = {
    {SauScheduleState::Idle, "IDLE", "command accepted"},
    {SauScheduleState::ResidentLoad, "REGISTER_LOAD",
     "accepted Operand-A preload requests"},
    {SauScheduleState::TransposeSetup, "TRANSPOSE_LOAD",
     "register_load_done"},
    {SauScheduleState::FlowExecute, "REUSE_LOAD",
     "scheduler.SA_SIZE transpose counter complete"},
    {SauScheduleState::FlowBoundary, "TRANSPOSE_CLIP",
     "per-flow B work boundary"},
    {SauScheduleState::DrainAndWriteback, "FIRST_LOAD|D_OUT|REGISTER_UNLOAD",
     "final B work accepted; results and writes drain"},
    {SauScheduleState::Complete, "IDLE",
     "all local packets, tokens, and writes drained"},
};

} // anonymous namespace

const SauScheduleStateMapping &
scheduleStateMapping(SauScheduleState state)
{
    const auto index = static_cast<unsigned>(state);
    if (index >= std::size(StateMappings)) {
        throw std::invalid_argument("unknown SAU schedule state");
    }
    return StateMappings[index];
}

RtlResidentLoadSkeleton::RtlResidentLoadSkeleton(
    const RtlResidentLoadConfig &config_) : config(config_)
{
    if (config.xBurst == 0 || config.xBurst > 64 ||
        config.yCycles == 0 || config.yCycles > 64 ||
        config.channelCycles == 0 || config.channelCycles > 256) {
        throw std::invalid_argument("invalid RTL resident-load dimensions");
    }
}

void
RtlResidentLoadSkeleton::tick(bool start)
{
    // register_addr.sv registers its combinational RUNNING outputs, so these
    // values are computed before the state/counters commit below.
    const bool running = state == State::Running;
    const bool endX = xCounter == config.xBurst - 1;
    const bool endY = yCounter == config.yCycles - 1;
    const bool endChannel = channelCounter == config.channelCycles - 1;
    const bool last = running && endX && endY && endChannel;

    State nextState = state;
    uint32_t nextX = xCounter;
    uint32_t nextY = yCounter;
    uint32_t nextChannel = channelCounter;
    switch (state) {
      case State::Idle:
        if (start) {
            nextState = State::Running;
            nextX = 0;
            nextY = 0;
            nextChannel = 0;
        }
        break;
      case State::Running:
        if (!endX) {
            ++nextX;
        } else if (!endY) {
            nextX = 0;
            ++nextY;
        } else if (!endChannel) {
            nextX = 0;
            nextY = 0;
            ++nextChannel;
        } else {
            nextState = State::Done;
            nextX = 0;
            nextY = 0;
            nextChannel = 0;
        }
        break;
      case State::Done:
        nextState = State::Idle;
        break;
    }

    requestValidReg = running;
    requestLastReg = last;
    state = nextState;
    xCounter = nextX;
    yCounter = nextY;
    channelCounter = nextChannel;
}

RtlStreamLoadSkeleton::RtlStreamLoadSkeleton(
    const RtlStreamLoadConfig &config_) : config(config_)
{
    // All eight current matmul waveform fixtures use this hardware tile
    // shape. Keep the first executable contract honest until another RTL
    // shape is validated instead of silently extrapolating it.
    if (config.xBurst != 1 || config.yCycles != 32 ||
        config.flowCycles == 0 || config.flowCycles > 255 ||
        config.instructionCycles == 0 || config.instructionCycles > 63) {
        throw std::invalid_argument("unsupported RTL streamed-load shape");
    }
}

bool
RtlStreamLoadSkeleton::testCounterClear() const
{
    return (testCounterValid && testCounter == 8) || lastFlowTimeDelay;
}

bool
RtlStreamLoadSkeleton::verticalCounterStart(
    const RtlStreamLoadInputs &inputs) const
{
    const bool leftTransposeLoad =
        coreStateDelay == RtlCoreState::TransposeLoad &&
        inputs.coreState != RtlCoreState::TransposeLoad;
    const bool nextReadStart =
        inputs.coreState != RtlCoreState::TransposeLoad &&
        (readLastDelay || leftTransposeLoad);
    const bool loadState =
        inputs.coreState == RtlCoreState::ReuseLoad ||
        inputs.coreState == RtlCoreState::TransposeClip ||
        inputs.coreState == RtlCoreState::FirstLoad;
    return (nextReadStart && loadState) || loadStartDelay;
}

RtlStreamLoadOutputs
RtlStreamLoadSkeleton::outputs(const RtlStreamLoadInputs &inputs) const
{
    // mem_addr.sv keeps load_done combinational. Consumers must snapshot this
    // value before calling tick(), just as they sample it before an RTL edge.
    const bool dataLast = (inputs.inputSwitch & 0x1) != 0 ?
        verticalLast : inputs.registerRequestLast;
    const bool loadDone = testCounterValid ?
        (testCounterClear() &&
         (dataLast || !(inputs.registerRequestValid || verticalValid))) :
        dataLast;
    return {readEnableReg, readLastReg, loadDone};
}

void
RtlStreamLoadSkeleton::tick(const RtlStreamLoadInputs &inputs)
{
    // Snapshot every combinational expression before committing registers.
    const bool counterStart = verticalCounterStart(inputs);
    const bool counterClear = testCounterClear();
    const bool loadDone = outputs(inputs).loadDone;

    State nextState = state;
    Step nextStep = step;
    uint32_t nextX = xCounter;
    uint32_t nextY = yCounter;
    uint32_t nextFlow = flowCounter;
    uint32_t nextInstruction = instructionCounter;
    bool nextVerticalValid = verticalValid;
    bool nextVerticalLast = verticalLast;

    switch (state) {
      case State::Idle:
        if (inputs.start) {
            nextState = State::WaitTrigger;
            nextStep = Step::Idle;
            nextX = 0;
            nextY = 0;
            nextFlow = 0;
            nextInstruction = 0;
        }
        break;
      case State::WaitTrigger:
        nextVerticalValid = false;
        nextVerticalLast = false;
        if (counterStart) {
            nextState = State::Running;
            nextVerticalValid = true;
            nextX = 0;
            nextY = 0;
            if (step == Step::Flow) {
                ++nextFlow;
            } else if (step == Step::Instruction) {
                nextFlow = 0;
                ++nextInstruction;
            }
        }
        break;
      case State::Running:
        if (xCounter < config.xBurst - 1) {
            ++nextX;
        } else if (yCounter < config.yCycles - 1) {
            nextX = 0;
            ++nextY;
            if (yCounter == config.yCycles - 2) {
                nextVerticalLast = true;
                if (flowCounter == config.flowCycles - 1 &&
                    instructionCounter == config.instructionCycles - 1) {
                    nextState = State::Done;
                    nextStep = Step::Idle;
                } else if (flowCounter == config.flowCycles - 1) {
                    nextState = State::WaitTrigger;
                    nextStep = Step::Instruction;
                } else {
                    nextState = State::WaitTrigger;
                    nextStep = Step::Flow;
                }
            }
        } else {
            // This branch is retained from mem_addr.sv. The validated y=32
            // path finishes through the yCounter == yCycles - 2 guard above.
            nextState = State::Done;
            nextStep = Step::Idle;
            nextX = 0;
            nextY = 0;
            nextFlow = 0;
            nextInstruction = 0;
        }
        break;
      case State::Done:
        nextVerticalValid = false;
        nextVerticalLast = false;
        nextState = State::Idle;
        break;
    }

    // Registered output stage: ADDR_DELAY=2 leaves input_switch undelayed in
    // the current RTL, while valid/last capture the old vertical flags.
    if (inputs.coreState != RtlCoreState::Idle &&
        (inputs.inputSwitch & 0x1) != 0) {
        readEnableReg = verticalValid;
        readLastReg = verticalLast;
    } else {
        readEnableReg = false;
        readLastReg = false;
    }

    if (counterClear) {
        testCounter = 0;
        testCounterValid = false;
    } else {
        if (testCounterValid) {
            ++testCounter;
        }
        if (counterStart) {
            testCounterValid = true;
        }
    }

    state = nextState;
    step = nextStep;
    xCounter = nextX;
    yCounter = nextY;
    flowCounter = nextFlow;
    instructionCounter = nextInstruction;
    verticalValid = nextVerticalValid;
    verticalLast = nextVerticalLast;
    coreStateDelay = inputs.coreState;
    loadStartDelay = inputs.nextExecuteStart;
    readLastDelay = loadDone;
    lastFlowTimeDelay = inputs.lastFlowTime;
}

RtlExecuteUpdateSkeleton::RtlExecuteUpdateSkeleton(
    const RtlExecuteUpdateConfig &config_) : config(config_)
{
    // Step 5.5 currently supports the fixed normal-int8 GEMM control mode.
    // Other kernels and retain modes have different update serialization.
    if (config.saSize == 0 || config.flowLoops == 0 ||
        config.flowLoops > 63 ||
        config.convolutionKernel != 0 || config.keepMode) {
        throw std::invalid_argument("unsupported RTL execute/update mode");
    }
    const uint64_t cycles =
        static_cast<uint64_t>(config.saSize) * config.flowLoops;
    if (cycles > 0x7ff) {
        throw std::invalid_argument("RTL calculation counter overflow");
    }
    calculationCycles = static_cast<uint32_t>(cycles);
}

void
RtlExecuteUpdateSkeleton::tick(const RtlExecuteUpdateInputs &inputs)
{
    // sa_feeder.update_finished is a register fed by the old update state and
    // old execute/result pulse.
    bool updateFinished = false;
    UpdateState nextUpdateState = updateState;
    switch (updateState) {
      case UpdateState::FirstOut:
        updateFinished = inputs.currentInstructionOutput ?
            inputs.resultLast : arrayFinishReg;
        if (!inputs.currentInstructionOutput && arrayFinishReg) {
            nextUpdateState = UpdateState::PingPong;
        }
        break;
      case UpdateState::PingPong:
        updateFinished = inputs.currentInstructionOutput ?
            false : inputs.resultLast;
        if (inputs.currentInstructionOutput) {
            nextUpdateState = UpdateState::LastPingPong;
        }
        break;
      case UpdateState::LastPingPong:
        updateFinished = inputs.resultLast;
        if (inputs.resultLast) {
            nextUpdateState = UpdateState::FirstOut;
        }
        break;
    }

    bool nextInternalFinish = false;
    if (inputs.enable) {
        if (calculationCounter == calculationCycles - 1) {
            calculationCounter = 0;
            nextInternalFinish = true;
        } else {
            ++calculationCounter;
        }
    }

    // SA_ENGINE.internal_finish_pulse -> delay_finish_flag[0] -> first
    // SA_ROW/SA_PE_array acc_finish_flag_d1. Every assignment samples the
    // pre-edge value, matching the RTL nonblocking chain.
    arrayFinishReg = finishDelayReg;
    finishDelayReg = internalFinishReg;
    internalFinishReg = nextInternalFinish;
    updateFinishedReg = updateFinished;
    updateState = nextUpdateState;
}

RtlSchedulerSkeleton::RtlSchedulerSkeleton(
    const RtlSchedulerConfig &config_) : config(config_)
{
    if (config.saSize == 0 || config.flowTimes == 0 ||
        config.instructionTimes == 0 || config.flowTimes > 127 ||
        config.instructionTimes > 127) {
        throw std::invalid_argument("invalid RTL scheduler dimensions");
    }
}

void
RtlSchedulerSkeleton::tick(const RtlSchedulerInputs &inputs)
{
    // All expressions in this section use the old registered state, just as
    // scheduler.sv's combinational guards do before the positive edge.
    const bool startBeforeEdge = startReg;
    const bool lastInstructionTimeBeforeEdge = lastInstructionTimeReg;
    const bool transFlag = config.transMode != 0;
    const bool reuseFlag = config.reuseMode != 0;
    const bool keepMode = (config.saFlowMode & 0x2) != 0;
    const bool executeFlag = ((inputSwitchReg & 0x1) != 0) !=
                             ((inputSwitchReg & 0x2) != 0);
    const bool convMode = reuseFlag && executeFlag;

    const uint32_t activeFlowLimit =
        instruction == RtlInstructionState::First ?
            config.flowTimes : config.flowTimes - 1;
    const bool transposeClear =
        transposeCounter == config.saSize - 1;
    const bool flowClear =
        flowCounter == activeFlowLimit && inputs.loadDone;
    const bool instructionClear =
        instructionCounter == config.instructionTimes - 1 && flowClear;

    const bool dOutConditionA = lastInstructionReg ?
        inputs.updateFinished :
        (!config.shiftMode && !keepMode &&
         (inputs.updateFinished || updateFinishedQ));
    const bool dOutConditionB =
        inputs.updateFinished || inputs.writeFinished;
    const bool dOutCondition = dOutConditionA || dOutConditionB;
    const bool keepModeDone = keepMode && core == RtlCoreState::DOut &&
        instruction == RtlInstructionState::Done && dOutCondition;

    RtlInstructionState nextInstruction = instruction;
    switch (instruction) {
      case RtlInstructionState::Idle:
        if (instructionValid) {
            nextInstruction = RtlInstructionState::First;
        }
        break;
      case RtlInstructionState::First:
        if (instructionClear) {
            nextInstruction = RtlInstructionState::Done;
        } else if (dOutCondition && core == RtlCoreState::DOut) {
            nextInstruction = RtlInstructionState::Loop;
        }
        break;
      case RtlInstructionState::Loop:
        if (instructionClear) {
            nextInstruction = RtlInstructionState::Done;
        }
        break;
      case RtlInstructionState::Done:
        if (dOutCondition && core == RtlCoreState::DOut) {
            nextInstruction = RtlInstructionState::Idle;
        }
        break;
    }

    const bool transposeExecuteStart =
        (config.transMode & 0x1) != 0 && instructionCounter == 0 ?
            (transposeClear && core == RtlCoreState::TransposeLoad) :
            (dOutCondition && core == RtlCoreState::DOut);
    const bool nextExecuteStart =
        (instruction == RtlInstructionState::First ||
         instruction == RtlInstructionState::Loop) &&
        transposeExecuteStart;
    const bool lastInstruction = core == RtlCoreState::DOut &&
        instructionCounter == config.instructionTimes - 1 &&
        (instruction == RtlInstructionState::Loop ||
         (instruction == RtlInstructionState::Done &&
          config.instructionTimes == 1) ||
         (instruction == RtlInstructionState::First &&
          config.instructionTimes > 1 && dOutCondition));

    RtlCoreState nextCore = core;
    bool flowEnd = false;
    switch (core) {
      case RtlCoreState::Idle:
        if (instructionValid) {
            nextCore = RtlCoreState::RegisterLoad;
        } else if (instruction == RtlInstructionState::Loop) {
            nextCore = RtlCoreState::ReuseLoad;
        }
        break;
      case RtlCoreState::RegisterLoad:
        if (transFlag && inputs.registerLoadDone) {
            nextCore = RtlCoreState::TransposeLoad;
        } else if (inputs.registerLoadDone) {
            nextCore = RtlCoreState::ReuseLoad;
        }
        break;
      case RtlCoreState::TransposeLoad:
        if (transposeClear || inputs.loadDone) {
            nextCore = RtlCoreState::ReuseLoad;
        }
        break;
      case RtlCoreState::TransposeClip:
        if (flowClear) {
            nextCore = RtlCoreState::DOut;
        } else if (convMode && inputs.loadDone) {
            nextCore = RtlCoreState::ReuseLoad;
        }
        break;
      case RtlCoreState::FirstLoad:
        if (flowClear) {
            nextCore = RtlCoreState::DOut;
        } else if (convMode && inputs.loadDone) {
            nextCore = RtlCoreState::ReuseLoad;
        }
        break;
      case RtlCoreState::ReuseLoad:
        if (flowClear) {
            nextCore = RtlCoreState::DOut;
        } else if (flowCounter ==
                       (activeFlowLimit == 0 ? 127 : activeFlowLimit - 1) &&
                   inputs.loadDone && transFlag) {
            nextCore = (config.instructionTimes == 1 ||
                        instructionCounter == config.instructionTimes - 1) ?
                RtlCoreState::FirstLoad : RtlCoreState::TransposeClip;
        }
        break;
      case RtlCoreState::DOut:
        if (instruction == RtlInstructionState::Done && dOutCondition) {
            if (keepMode) {
                nextCore = RtlCoreState::Idle;
                flowEnd = true;
            } else {
                nextCore = RtlCoreState::RegisterUnload;
            }
        } else if (instruction == RtlInstructionState::First &&
                   !instructionClear && dOutCondition) {
            nextCore = RtlCoreState::ReuseLoad;
        } else if (instruction == RtlInstructionState::Loop &&
                   dOutCondition) {
            nextCore = RtlCoreState::ReuseLoad;
        }
        break;
      case RtlCoreState::RegisterUnload:
        if (inputs.writeFinished) {
            nextCore = RtlCoreState::Idle;
            flowEnd = true;
        }
        break;
    }

    uint8_t nextInputSwitch = inputSwitchReg;
    switch (core) {
      case RtlCoreState::Idle:
        nextInputSwitch =
            (config.transMode == 0x2 ||
             (config.transMode == 0 && config.reuseMode == 0x2)) ? 0x3 : 0;
        break;
      case RtlCoreState::FirstLoad:
      case RtlCoreState::RegisterLoad:
        if (dataLastD && !convMode) {
            nextInputSwitch =
                (inputSwitchReg & 0x2) |
                ((inputSwitchReg & 0x1) == 0 ? 0x1 : 0);
        }
        break;
      case RtlCoreState::ReuseLoad:
        nextInputSwitch = 0x1;
        break;
      case RtlCoreState::DOut:
        if (flowEnd) {
            nextInputSwitch = 0;
        }
        break;
      case RtlCoreState::TransposeLoad:
      case RtlCoreState::TransposeClip:
        break;
      case RtlCoreState::RegisterUnload:
        nextInputSwitch = 0;
        break;
    }

    // Positive-edge commit. Clear has priority over load for every FFLARNC.
    if (startReg || flowEnd) {
        startReg = false;
    } else if (inputs.startWrite) {
        startReg = true;
    }
    instructionValid = startBeforeEdge;
    dataLastD = inputs.loadDone;
    if (transposeClear) {
        transposeCounter = 0;
    } else if (core == RtlCoreState::TransposeLoad) {
        ++transposeCounter;
    }
    if (flowEnd) {
        updateFinishedQ = false;
    } else if (inputs.updateFinished) {
        updateFinishedQ = true;
    }
    if (instructionClear) {
        instructionCounter = 0;
    } else if (flowClear) {
        instructionCounter = (instructionCounter + 1) & 0x7f;
    }
    if (inputs.lastFlowTimeClear) {
        lastInstructionTimeReg = false;
    } else if (instructionClear) {
        lastInstructionTimeReg = true;
    }
    if (flowClear) {
        flowCounter = 0;
    } else if (inputs.loadDone && !lastInstructionTimeBeforeEdge) {
        flowCounter = (flowCounter + 1) & 0x7f;
    }
    if (nextExecuteStartReg || flowEnd) {
        lastFlowTimeReg = false;
    } else if (lastFlowTimeValidReg) {
        lastFlowTimeReg = true;
    }
    if (inputs.writeFinished || keepModeDone) {
        lastInstructionReg = false;
    } else if (lastInstruction) {
        lastInstructionReg = true;
    }

    instruction = nextInstruction;
    core = nextCore;
    inputSwitchReg = nextInputSwitch;
    nextExecuteStartReg = nextExecuteStart;
    lastFlowTimeValidReg = flowClear;
    lastInstructionTimeClearReg = instructionClear;
    crossbarDoneReg = flowEnd;
}

SauScheduleState
SauSchedule::state() const
{
    return current;
}

void
SauSchedule::beginCommand()
{
    current = SauScheduleState::ResidentLoad;
    residentReady = false;
}

void
SauSchedule::completeCommand()
{
    if (current != SauScheduleState::DrainAndWriteback) {
        throw std::logic_error("SAU command completed before drain/writeback");
    }
    current = SauScheduleState::Complete;
}

bool
SauSchedule::canIssueRead(StreamKind stream) const
{
    if (stream == StreamKind::OperandA) {
        // A later instruction may preload while an earlier one executes.
        return current != SauScheduleState::Idle &&
            current != SauScheduleState::DrainAndWriteback &&
            current != SauScheduleState::Complete;
    }
    if (stream == StreamKind::OperandB) {
        return current == SauScheduleState::TransposeSetup ||
            current == SauScheduleState::FlowExecute ||
            current == SauScheduleState::FlowBoundary;
    }
    return false;
}

bool
SauSchedule::canLoadResident() const
{
    return current == SauScheduleState::ResidentLoad ||
        current == SauScheduleState::TransposeSetup ||
        current == SauScheduleState::FlowExecute ||
        current == SauScheduleState::FlowBoundary;
}

bool
SauSchedule::canAdmitArrayA() const
{
    return residentReady &&
        (current == SauScheduleState::TransposeSetup ||
         current == SauScheduleState::FlowExecute ||
         current == SauScheduleState::FlowBoundary ||
         current == SauScheduleState::DrainAndWriteback);
}

bool
SauSchedule::canAdmitArrayB() const
{
    return residentReady &&
        (current == SauScheduleState::TransposeSetup ||
         current == SauScheduleState::FlowExecute ||
         current == SauScheduleState::FlowBoundary);
}

bool
SauSchedule::canReleaseResult() const
{
    return current == SauScheduleState::FlowExecute ||
        current == SauScheduleState::FlowBoundary ||
        current == SauScheduleState::DrainAndWriteback;
}

bool
SauSchedule::canIssueWriteback() const
{
    return current == SauScheduleState::DrainAndWriteback;
}

bool
SauSchedule::onAReadAccepted(bool lastForInstruction)
{
    if (!canIssueRead(StreamKind::OperandA)) {
        return false;
    }
    if (current == SauScheduleState::ResidentLoad && lastForInstruction) {
        current = SauScheduleState::TransposeSetup;
    }
    return true;
}

bool
SauSchedule::onResidentReady()
{
    if (!canLoadResident()) {
        return false;
    }
    residentReady = true;
    return true;
}

bool
SauSchedule::onTransposeComplete()
{
    if (current != SauScheduleState::TransposeSetup) {
        return false;
    }
    current = SauScheduleState::FlowExecute;
    return true;
}

bool
SauSchedule::onArrayAAdmitted()
{
    if (!canAdmitArrayA()) {
        return false;
    }
    return true;
}

bool
SauSchedule::onArrayBAdmitted(bool flowBoundary, bool finalWork)
{
    if (!canAdmitArrayB()) {
        return false;
    }
    if (finalWork) {
        current = SauScheduleState::DrainAndWriteback;
    } else if (flowBoundary) {
        current = SauScheduleState::FlowBoundary;
    } else {
        current = SauScheduleState::FlowExecute;
    }
    return true;
}

} // namespace gem5::sau
