#include "sau/schedule_state.hh"

#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>

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

void
RtlStageWindow::observe(uint64_t edge)
{
    if (!observed) {
        observed = true;
        firstEdge = edge;
    }
    lastEdge = edge;
}

uint64_t
RtlStageWindow::span() const
{
    if (!observed) {
        throw std::logic_error("RTL stage window was not observed");
    }
    return lastEdge - firstEdge + 1;
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
    // The current stage supports normal int8 GEMM in clear and keep modes.
    // Nonzero kernels still select a different update serialization.
    if (config.saSize == 0 || config.flowLoops == 0 ||
        config.flowLoops > 63 ||
        config.convolutionKernel != 0) {
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
        if (!config.keepMode && !inputs.currentInstructionOutput &&
            arrayFinishReg) {
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

bool
RtlSaEnableSkeleton::saEnable(const RtlSaEnableInputs &inputs) const
{
    const bool executeInputSelected =
        inputs.inputSwitch == 0x1 || inputs.inputSwitch == 0x2;
    return executeInputSelected && inputEnableDelayReg;
}

void
RtlSaEnableSkeleton::tick(const RtlSaEnableInputs &inputs)
{
    // sa_feeder.EN_i is combinational A-valid OR B-valid. EN_i_d samples it
    // at this edge; SA_ENGINE has already sampled the old sa_en_i value.
    inputEnableDelayReg = inputs.dataAValid || inputs.dataBValid;
}

RtlResultSerializerSkeleton::RtlResultSerializerSkeleton(
    const RtlResultSerializerConfig &config_) : config(config_)
{
    if (config.saSize != 32 || config.peRowNum != 4 ||
        config.peColNum != 4) {
        throw std::invalid_argument(
            "unsupported RTL result serializer geometry");
    }

    const uint32_t macroColumns = config.saSize / config.peColNum;
    finishToMacroPipeline.assign(macroColumns, false);
}

void
RtlResultSerializerSkeleton::tick(
    const RtlResultSerializerInputs &inputs)
{
    // Every expression is evaluated from the pre-edge register snapshot.
    const bool rowScoreValidT = macroStreamingReg;
    const bool engineStorageReadyBeforeEdge = engineStorageReadyReg;
    const bool outputStartBeforeEdge = outputStartReg;
    const bool calFinishT =
        rowScoreValidT && globalRowCount == config.saSize - 1;
    const bool storageReadyStart =
        !engineStorageReadyDelayReg && engineStorageReadyReg;
    const bool serialStart =
        (storageReadyStart || outputPendingReg) &&
        transposerInputReadyReg && !serialActiveReg;
    const bool resultLastT = resultGateReg && transposerLastReg;
    const bool transposerReadEnable =
        transposerReadyOutReg && serialActiveReg && !resultLastT;
    const bool transposerInputLast =
        transposerInputCount == config.saSize - 1;
    const bool transposerOutputLast =
        transposerOutputCount == config.saSize - 1;
    const bool resultValidT = resultGateReg && transposerValidReg;

    const bool nextFirstMacroValid = finishToMacroPipeline.back();
    for (uint32_t index = finishToMacroPipeline.size() - 1;
         index > 0; --index) {
        finishToMacroPipeline[index] = finishToMacroPipeline[index - 1];
    }
    finishToMacroPipeline[0] = inputs.internalFinish;

    // SA_ENGINE.storage_ready is set by macro_valid_out[0] and cleared by the
    // final streamed row. Both guards sample their old values at this edge.
    if (calFinishT) {
        engineStorageReadyReg = false;
    } else if (firstMacroValidReg) {
        engineStorageReadyReg = true;
    }
    engineStorageReadyDelayReg = engineStorageReadyBeforeEdge;

    if (serialStart) {
        outputPendingReg = false;
    } else if (storageReadyStart) {
        outputPendingReg = true;
    }
    if (resultLastT) {
        serialActiveReg = false;
        resultGateReg = false;
    } else {
        if (serialStart) {
            serialActiveReg = true;
            resultGateReg = true;
        } else if (calFinishReg) {
            resultGateReg = true;
        }
    }

    if (calFinishT) {
        outputStartReg = false;
    } else if (serialStart) {
        outputStartReg = true;
    }

    const bool macroResultAvailable =
        macroResultPendingReg || firstMacroValidReg;
    if (!macroStreamingReg && macroResultAvailable &&
        outputStartBeforeEdge) {
        macroStreamingReg = true;
        macroResultPendingReg = false;
        macroStreamCount = 0;
    } else if (macroStreamingReg) {
        if (macroStreamCount == config.saSize - 1) {
            macroStreamingReg = false;
            macroStreamCount = 0;
        } else {
            ++macroStreamCount;
        }
    } else if (firstMacroValidReg) {
        macroResultPendingReg = true;
    }

    if (rowScoreValidT) {
        if (globalRowCount == config.saSize - 1) {
            globalRowCount = 0;
        } else {
            ++globalRowCount;
        }
    }

    const bool transposerInputEnable = rowScoreValidReg;
    if (transposerReadEnable && transposerOutputLast) {
        transposerInputReadyReg = true;
        transposerReadyOutReg = false;
    } else if (transposerInputEnable && transposerInputLast) {
        transposerInputReadyReg = false;
        transposerReadyOutReg = true;
    }

    if (transposerInputEnable) {
        transposerInputCount = transposerInputLast ?
            0 : transposerInputCount + 1;
    }
    if (transposerReadEnable) {
        transposerOutputCount = transposerOutputLast ?
            0 : transposerOutputCount + 1;
    }

    transposerValidReg = transposerReadEnable;
    transposerLastReg = transposerReadEnable && transposerOutputLast;
    resultValidReg = resultValidT;
    resultLastReg = resultLastT;
    rowScoreValidReg = rowScoreValidT;
    calFinishReg = calFinishT;
    firstMacroValidReg = nextFirstMacroValid;
}

RtlOutputWritebackSkeleton::RtlOutputWritebackSkeleton(
    const RtlOutputWritebackConfig &config_)
    : config(config_),
      outputAddress({config_.outputXBurst, config_.outputYCycles,
                     config_.outputCCycles})
{
    if (config.internalXBurst == 0 || config.internalXBurst > 64 ||
        config.internalYBurst == 0 || config.internalYBurst > 64 ||
        config.internalFlowBurst == 0 || config.internalFlowBurst > 64 ||
        config.internalInstructionBurst == 0 ||
        config.internalInstructionBurst > 64) {
        throw std::invalid_argument(
            "invalid RTL result-accumulation dimensions");
    }
}

void
RtlOutputWritebackSkeleton::tick(
    const RtlOutputWritebackInputs &inputs)
{
    // All guards and pipeline inputs use the pre-edge register snapshot.
    const bool endX = resultXCounter == config.internalXBurst - 1;
    const bool endY = resultYCounter == config.internalYBurst - 1;
    const bool endFlow =
        resultFlowCounter == config.internalFlowBurst - 1;
    const bool endInstruction =
        resultInstructionCounter == config.internalInstructionBurst - 1;
    const bool resultEnd = endX && endY && endFlow && endInstruction;
    const bool resultDoneSet = inputs.resultValid && resultEnd;
    const bool registerOutRequest =
        inputs.coreState == RtlCoreState::RegisterUnload &&
        resultAccumDoneReg;
    const bool registerOutStart =
        registerOutStateReg && !registerOutStateDelayReg;
    const bool addressValid = outputAddress.requestValid();
    const bool addressLast = outputAddress.requestLast();
    const bool writeLastBeforeEdge = writeLastDelay2Reg;

    if (inputs.resultValid) {
        if (!endX) {
            ++resultXCounter;
        } else if (!endY) {
            resultXCounter = 0;
            ++resultYCounter;
        } else if (!endFlow) {
            resultXCounter = 0;
            resultYCounter = 0;
            ++resultFlowCounter;
        } else if (!endInstruction) {
            resultXCounter = 0;
            resultYCounter = 0;
            resultFlowCounter = 0;
            ++resultInstructionCounter;
        } else {
            resultXCounter = 0;
            resultYCounter = 0;
            resultFlowCounter = 0;
            resultInstructionCounter = 0;
        }
    }

    // FFLARNC clears the sticky completion flag when the scheduler returns
    // to IDLE; otherwise the final accepted result token sets it.
    if (inputs.coreState == RtlCoreState::Idle) {
        resultAccumDoneReg = false;
    } else if (resultDoneSet) {
        resultAccumDoneReg = true;
    }

    outputAddress.tick(registerOutStart);
    registerOutStateDelayReg = registerOutStateReg;
    registerOutStateReg = registerOutRequest;

    writeValidDelay2Reg = writeValidDelay1Reg;
    writeValidDelay1Reg = addressValid;
    writeLastDelay2Reg = writeLastDelay1Reg;
    writeLastDelay1Reg = addressLast;
    writeDoneDelay4Reg = writeDoneDelay3Reg;
    writeDoneDelay3Reg = writeDoneDelay2Reg;
    writeDoneDelay2Reg = writeDoneDelay1Reg;
    writeDoneDelay1Reg = writeLastBeforeEdge;
}

void
RtlSramWriteTransportSkeleton::tick(
    const RtlSramWriteTransportInputs &inputs)
{
    // The shared-memory macro samples the old registered crossbar output at
    // this edge. Expose that physical write event after the commit.
    memoryWriteAcceptedReg = crossbarSlaveValidReg;
    memoryWriteLastReg =
        crossbarSlaveValidReg && crossbarSlaveLastReg;

    // crossbar_mi.crossbar_logic registers the selected master request only
    // while the accelerator owns the bus in ACTIVE.
    if (state == State::Active) {
        crossbarSlaveValidReg = memCtrlWriteValidReg;
        crossbarSlaveLastReg =
            memCtrlWriteValidReg && memCtrlWriteLastReg;
    } else {
        crossbarSlaveValidReg = false;
        crossbarSlaveLastReg = false;
    }

    // mem_ctrl's write branch is one register between register_file_out and
    // sau_sram_enable/wstrb. The native interface has no ready/backpressure.
    memCtrlWriteValidReg = inputs.nativeWriteValid;
    memCtrlWriteLastReg =
        inputs.nativeWriteValid && inputs.nativeWriteLast;

    State nextState = state;
    switch (state) {
      case State::Idle:
        if (inputs.crossbarStart) {
            nextState = State::Active;
        } else if (inputs.dbusRequest) {
            nextState = State::RvActive;
        }
        break;
      case State::Active:
        if (inputs.crossbarDone) {
            nextState = State::Idle;
        }
        break;
      case State::RvActive:
        if (inputs.crossbarStart) {
            nextState = State::Active;
        } else if (!inputs.dbusRequest) {
            nextState = State::Idle;
        }
        break;
    }
    state = nextState;
}

RtlResidentFillSkeleton::RtlResidentFillSkeleton(
    const RtlResidentFillConfig &config_) : config(config_)
{
    if (config.sramDelay == 0) {
        throw std::invalid_argument("invalid RTL resident-fill SRAM delay");
    }

    // mem_ctrl.STATE_DELAY = SRAM_DELAY + 1. register_file_in.PAD_DELAY
    // additionally includes MEMCTRL_DELAY=2, ADDR_DELAY=1, and one final
    // alignment edge.
    memoryValidPipeline.assign(config.sramDelay + 1, false);
    memoryLastPipeline.assign(config.sramDelay + 1, false);
    coreStatePipeline.assign(config.sramDelay + 4, RtlCoreState::Idle);
}

void
RtlResidentFillSkeleton::tick(const RtlResidentFillInputs &inputs)
{
    const uint32_t coreStateIndex = coreStatePipeline.size() - 2;
    const bool fillEnable =
        registerFileInputValidReg &&
        coreStatePipeline[coreStateIndex] == RtlCoreState::RegisterLoad;

    // Each consumer samples the producer's old registered output. The final
    // assignment is stream_padding_shifter.valid_o, which is also the input
    // RF SRAM write enable in the validated no-padding matmul path.
    residentWriteValidReg = fillEnable;
    registerFileInputValidReg = memoryDataValidReg;
    memoryDataValidReg = memoryValidPipeline.back();
    memoryDataLastReg = memoryLastPipeline.back();

    for (uint32_t index = memoryValidPipeline.size() - 1;
         index > 0; --index) {
        memoryValidPipeline[index] = memoryValidPipeline[index - 1];
        memoryLastPipeline[index] = memoryLastPipeline[index - 1];
    }
    memoryValidPipeline[0] = inputs.readRequestValid;
    memoryLastPipeline[0] =
        inputs.readRequestValid && inputs.readRequestLast;

    for (uint32_t index = coreStatePipeline.size() - 1;
         index > 0; --index) {
        coreStatePipeline[index] = coreStatePipeline[index - 1];
    }
    coreStatePipeline[0] = inputs.coreState;
}

RtlInputFeederSkeleton::RtlInputFeederSkeleton(
    const RtlInputFeederConfig &config_) : config(config_)
{
    if (config.xBurst == 0 || config.xBurst > 64 ||
        config.yBurst == 0 || config.yBurst > 64 ||
        config.flowBurst == 0 || config.flowBurst > 64 ||
        config.instructionBurst == 0 ||
        config.instructionBurst > 64 || config.saSize == 0 ||
        config.saSize > 64 || config.sramDelay == 0 ||
        config.addressDelay == 0 || config.memoryControlDelay == 0 ||
        config.registerDelay == 0) {
        throw std::invalid_argument("invalid RTL input-feeder configuration");
    }

    const uint32_t stateDelay = config.sramDelay + config.addressDelay +
        config.memoryControlDelay;
    coreStatePipeline.assign(stateDelay, RtlCoreState::Idle);
    inputSwitchPipeline.assign(stateDelay, 0);
    lastFlowTimePipeline.assign(stateDelay, false);
    outputInputSwitchPipeline.assign(config.registerDelay, 0);
    memoryValidPipeline.assign(config.registerDelay, false);
}

void
RtlInputFeederSkeleton::tick(const RtlInputFeederInputs &inputs)
{
    // All producers and consumers below use the same pre-edge snapshot. This
    // is important at the 266/267 RF boundary and at the 302/303 SA-enable
    // boundary: a value made visible by this commit is sampled next edge.
    const RtlCoreState delayedCoreState = coreStatePipeline.back();
    const bool enteredTransposeLoad =
        delayedCoreState == RtlCoreState::TransposeLoad &&
        delayedCoreStateReg != RtlCoreState::TransposeLoad;
    const bool memoryDataStart =
        inputs.memoryDataValid && !memoryDataValidReg;
    const bool readTrigger =
        reuseLoadStateReg && (enteredTransposeLoad || memoryDataStart);

    const bool readActive =
        (readState == ReadState::Idle && readEnableReg) ||
        readState == ReadState::Burst;
    const bool endX = xCounter == config.xBurst - 1;
    const bool endY = yCounter == config.yBurst - 1;
    const bool endFlow = flowCounter == config.flowBurst - 1;
    const bool endInstruction =
        instructionCounter == config.instructionBurst - 1;
    const bool readLast = readActive && endX && endY;

    ReadState nextReadState = readState;
    if (readActive) {
        if (!endX) {
            ++xCounter;
            nextReadState = ReadState::Burst;
        } else if (!endY) {
            xCounter = 0;
            ++yCounter;
            nextReadState = ReadState::Burst;
        } else {
            xCounter = 0;
            yCounter = 0;
            nextReadState = ReadState::Idle;
            if (!endFlow) {
                ++flowCounter;
            } else if (!endInstruction) {
                flowCounter = 0;
                ++instructionCounter;
            } else {
                flowCounter = 0;
                instructionCounter = 0;
            }
        }
    }

    // conv_kernal=0 is the validated bypass path. shift_register registers
    // RF valid once, then shift_data_cnt_valid holds a 32-token SA window;
    // feeder adds the final data_A_valid_o register.
    const bool shiftCountValid =
        shiftDataCounter != 0 || shiftDataValidReg;
    const bool shiftCountClear =
        shiftDataCounter == config.saSize - 1;
    if (shiftCountClear) {
        shiftDataCounter = 0;
    } else if (shiftCountValid) {
        ++shiftDataCounter;
    }

    dataAValidReg = shiftCountValid;
    shiftDataValidReg = readValidReg;
    readValidReg = readActive;
    readLastReg = readLast;
    readState = nextReadState;
    readEnableReg = readTrigger;

    // The fixed ATB/reuse-A path streams B through EN_i_d, REGISTER_DELAY,
    // the A/B arbiter, and the final data_B_valid_o register. The arbiter
    // remains active while the scheduler crosses D_OUT; core state does not
    // directly gate this output.
    // feeder.sv's ATB arbiter registers
    //   input_switch_case = ~input_switch_d_o[0][0]
    // and only enables B when that case bit is clear. This blocks the
    // resident-load tail that is still visible after the scheduler has
    // entered TRANSPOSE_LOAD, while admitting the streamed operand once the
    // delayed input switch selects 01.
    dataBValidReg =
        !inputSwitchCaseReg && memoryValidPipeline.back();
    for (uint32_t index = memoryValidPipeline.size() - 1;
         index > 0; --index) {
        memoryValidPipeline[index] = memoryValidPipeline[index - 1];
    }
    memoryValidPipeline[0] = memoryDataValidReg;
    memoryDataValidReg = inputs.memoryDataValid;

    outputInputSwitchReg = outputInputSwitchPipeline.back();
    for (uint32_t index = outputInputSwitchPipeline.size() - 1;
         index > 0; --index) {
        outputInputSwitchPipeline[index] =
            outputInputSwitchPipeline[index - 1];
    }
    outputInputSwitchPipeline[0] = inputSwitchDelayReg;
    inputSwitchCaseReg = (inputSwitchDelayReg & 0x1) == 0;
    inputSwitchDelayReg = inputSwitchPipeline.back();
    lastFlowTimeClearReg =
        !lastFlowTimePipeline.back() &&
        lastFlowTimePipeline[lastFlowTimePipeline.size() - 2];

    delayedCoreStateReg = delayedCoreState;
    reuseLoadStateReg =
        coreStatePipeline[coreStatePipeline.size() - 2] ==
            RtlCoreState::ReuseLoad ||
        coreStatePipeline[coreStatePipeline.size() - 2] ==
            RtlCoreState::TransposeLoad ||
        coreStatePipeline[coreStatePipeline.size() - 2] ==
            RtlCoreState::TransposeClip;
    for (uint32_t index = coreStatePipeline.size() - 1;
         index > 0; --index) {
        coreStatePipeline[index] = coreStatePipeline[index - 1];
        inputSwitchPipeline[index] = inputSwitchPipeline[index - 1];
        lastFlowTimePipeline[index] = lastFlowTimePipeline[index - 1];
    }
    coreStatePipeline[0] = inputs.coreState;
    inputSwitchPipeline[0] = inputs.inputSwitch;
    lastFlowTimePipeline[0] = inputs.lastFlowTime;
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
        (!config.shiftMode && config.flowTimes != 1 && !keepMode);
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

RtlCommandDriverSkeleton::RtlCommandDriverSkeleton(
    const RtlCommandDriverConfig &config_)
    : config(config_),
      scheduler(config.scheduler),
      residentLoad(config.residentLoad),
      streamLoad(config.streamLoad),
      readPath({config.sramDelay}),
      inputFeeder(config.inputFeeder),
      execute(config.execute),
      resultSerializer(config.resultSerializer),
      outputWriteback(config.outputWriteback)
{
    if (config.scheduler.saSize != config.inputFeeder.saSize ||
        config.scheduler.saSize != config.execute.saSize ||
        config.scheduler.saSize != config.resultSerializer.saSize ||
        config.scheduler.flowTimes != config.streamLoad.flowCycles ||
        config.scheduler.flowTimes != config.inputFeeder.flowBurst ||
        config.scheduler.flowTimes != config.execute.flowLoops ||
        config.scheduler.instructionTimes !=
            config.streamLoad.instructionCycles ||
        config.scheduler.instructionTimes !=
            config.inputFeeder.instructionBurst ||
        config.scheduler.instructionTimes !=
            config.outputWriteback.internalInstructionBurst ||
        config.sramDelay != config.inputFeeder.sramDelay) {
        throw std::invalid_argument(
            "inconsistent RTL command-driver configuration");
    }
}

void
RtlCommandDriverSkeleton::tick(bool startWrite)
{
    // Snapshot every producer output before any component commits this edge.
    const bool start = scheduler.start();
    const bool residentRequestValid = residentLoad.requestValid();
    const bool residentRequestLast = residentLoad.requestLast();

    RtlStreamLoadInputs streamInputs;
    streamInputs.start = start;
    streamInputs.coreState = scheduler.coreState();
    streamInputs.inputSwitch = scheduler.inputSwitch();
    streamInputs.lastFlowTime = scheduler.lastFlowTime();
    streamInputs.nextExecuteStart = scheduler.nextExecuteStart();
    streamInputs.registerRequestValid = residentRequestValid;
    streamInputs.registerRequestLast = residentRequestLast;
    const auto streamOutputs = streamLoad.outputs(streamInputs);

    RtlSchedulerInputs schedulerInputs;
    schedulerInputs.startWrite = startWrite;
    schedulerInputs.loadDone = streamOutputs.loadDone;
    schedulerInputs.registerLoadDone = residentRequestLast;
    schedulerInputs.updateFinished = execute.updateFinished();
    schedulerInputs.writeFinished = outputWriteback.writeFinished();
    schedulerInputs.lastFlowTimeClear =
        inputFeeder.lastFlowTimeClear();

    RtlResidentFillInputs readInputs;
    readInputs.readRequestValid =
        residentRequestValid || streamOutputs.readEnable;
    readInputs.readRequestLast =
        residentRequestLast || streamOutputs.readLast;
    readInputs.coreState = scheduler.coreState();

    RtlInputFeederInputs inputFeederInputs;
    inputFeederInputs.coreState = scheduler.coreState();
    inputFeederInputs.inputSwitch = scheduler.inputSwitch();
    inputFeederInputs.memoryDataValid = readPath.memoryDataValid();
    inputFeederInputs.lastFlowTime = scheduler.lastFlowTime();

    RtlSaEnableInputs saEnableInputs;
    saEnableInputs.dataAValid = inputFeeder.dataAValid();
    saEnableInputs.dataBValid = inputFeeder.dataBValid();
    saEnableInputs.inputSwitch = inputFeeder.outputInputSwitch();
    const bool acceptedSaEnable = saEnablePath.saEnable(saEnableInputs);

    RtlExecuteUpdateInputs executeInputs;
    executeInputs.enable = acceptedSaEnable;
    executeInputs.currentInstructionOutput =
        scheduler.lastInstruction() &&
        (config.scheduler.saFlowMode & 0x2) == 0;
    executeInputs.resultLast = resultSerializer.resultLast();

    RtlResultSerializerInputs serializerInputs;
    serializerInputs.internalFinish = execute.internalFinish();

    RtlOutputWritebackInputs outputInputs;
    outputInputs.resultValid = resultSerializer.resultValid();
    outputInputs.coreState = scheduler.coreState();

    RtlSramWriteTransportInputs writeInputs;
    writeInputs.nativeWriteValid = outputWriteback.writeValid();
    writeInputs.nativeWriteLast = outputWriteback.writeDataLast();
    writeInputs.crossbarStart = startWrite;
    writeInputs.crossbarDone = scheduler.commandDone();

    scheduler.tick(schedulerInputs);
    residentLoad.tick(start);
    streamLoad.tick(streamInputs);
    readPath.tick(readInputs);
    inputFeeder.tick(inputFeederInputs);
    saEnablePath.tick(saEnableInputs);
    execute.tick(executeInputs);
    resultSerializer.tick(serializerInputs);
    outputWriteback.tick(outputInputs);
    writeTransport.tick(writeInputs);

    // The shared pre-edge snapshot is already one edge behind the address
    // producer's registered output, matching mem_ctrl's selected SRAM
    // request. Do not add another register here.
    memoryReadAcceptedReg = readInputs.readRequestValid;
    memoryReadAcceptedLastReg =
        readInputs.readRequestValid && readInputs.readRequestLast;
    memoryReadAcceptedStreamReg =
        streamOutputs.readEnable && !residentRequestValid;

    currentStreamReadEnable =
        streamLoad.outputs(streamInputs).readEnable;
    currentSaEnable = saEnablePath.saEnable(
        {inputFeeder.dataAValid(), inputFeeder.dataBValid(),
         inputFeeder.outputInputSwitch()});

    residentReads += residentLoad.requestValid() ? 1 : 0;
    streamReads += currentStreamReadEnable ? 1 : 0;
    registerFileReads +=
        inputFeeder.registerFileReadValid() ? 1 : 0;
    operandAInputs += inputFeeder.dataAValid() ? 1 : 0;
    operandBInputs += inputFeeder.dataBValid() ? 1 : 0;
    acceptedSaInputs += acceptedSaEnable ? 1 : 0;
    results += resultSerializer.resultValid() ? 1 : 0;
    nativeWrites += outputWriteback.writeValid() ? 1 : 0;
    physicalWrites +=
        writeTransport.memoryWriteAccepted() ? 1 : 0;

    if (memoryReadAcceptedReg) {
        (memoryReadAcceptedStreamReg ?
             streamReadStage : residentReadStage).observe(currentEdge);
    }
    if (inputFeeder.dataAValid()) {
        operandAStage.observe(currentEdge);
    }
    if (inputFeeder.dataBValid()) {
        operandBStage.observe(currentEdge);
    }
    if (resultSerializer.resultValid()) {
        resultStage.observe(currentEdge);
    }
    if (writeTransport.memCtrlWriteValid()) {
        memoryWriteStage.observe(currentEdge);
    }
    if (scheduler.commandDone() && !commandDoneSeen) {
        commandDoneSeen = true;
        commandDoneAt = currentEdge;
    }
    ++currentEdge;
}

uint64_t
predictRtlCommandDoneEdge(const RtlCommandDriverConfig &config)
{
    RtlCommandDriverSkeleton driver(config);
    constexpr uint64_t MaxPredictionEdges = 100'000'000;
    for (uint64_t edge = 0; edge < MaxPredictionEdges; ++edge) {
        driver.tick(edge == 0);
        if (driver.commandDoneObserved()) {
            return driver.commandDoneEdge();
        }
    }
    throw std::runtime_error(
        "RTL command-driver did not reach command_done during preflight");
}

void
validateRtlSequentialCommandWindows(
    const std::vector<RtlSequentialCommandWindow> &commands)
{
    for (size_t index = 0; index + 1 < commands.size(); ++index) {
        const auto &current = commands[index];
        const auto &next = commands[index + 1];
        const uint64_t doneEdge =
            predictRtlCommandDoneEdge(current.driver);
        if (doneEdge >
            std::numeric_limits<uint64_t>::max() - current.startCycle) {
            throw std::invalid_argument(
                "SAU sequential CSR fixture completion cycle overflows");
        }
        const uint64_t completionCycle = current.startCycle + doneEdge;
        if (next.startCycle <= completionCycle) {
            throw std::invalid_argument(
                "SAU sequential CSR fixture command " +
                std::to_string(next.commandId) + " starts at cycle " +
                std::to_string(next.startCycle) + " before command " +
                std::to_string(current.commandId) +
                " is complete and write-visible at cycle " +
                std::to_string(completionCycle));
        }
    }
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
