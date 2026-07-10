#include "sau/sau_model.hh"

#include <algorithm>
#include <cassert>
#include <limits>
#include <stdexcept>

#include "base/logging.hh"
#include "sau/command.hh"
#include "sim/sim_exit.hh"
#include "sim/system.hh"

namespace gem5::sau
{
namespace
{

// 从 Sau.py 的 synthetic 参数构造 SauCommand。
// 首阶段只支持 int8 GEMM，不通过 ISA 注入命令。
SauCommand
buildStartupCommand(const SauModelParams &params)
{
    const uint64_t work_items =
        static_cast<uint64_t>(params.a_beats) * params.flow_loops *
        params.instruction_loops;
    if (work_items > std::numeric_limits<uint32_t>::max()) {
        throw std::invalid_argument(
            "synthetic SAU work item count exceeds its representation");
    }

    return {
        params.command_id,
        Operation::Gemm,
        Precision::Int8,
        {
            params.a_base,
            static_cast<uint32_t>(params.a_beats),
            static_cast<uint32_t>(params.beat_bytes),
            static_cast<uint32_t>(params.a_flow_stride),
            0,    // A 暂不使用 instruction stride（合并到 beat stride 中）
        },
        {
            params.b_base,
            static_cast<uint32_t>(params.b_beats),
            static_cast<uint32_t>(params.b_stride_bytes),
            static_cast<uint32_t>(params.b_flow_stride),
            0,    // B 暂不使用 instruction stride
        },
        {
            params.output_base,
            static_cast<uint32_t>(params.output_beats),
            static_cast<uint32_t>(params.beat_bytes),
            0,    // output 不使用 flow stride（写回不随 flow 变化）
            static_cast<uint32_t>(params.output_instruction_stride),
        },
        static_cast<uint32_t>(params.flow_loops),
        static_cast<uint32_t>(params.instruction_loops),
        static_cast<uint32_t>(work_items),
    };
}

const char *
streamName(StreamKind stream)
{
    switch (stream) {
      case StreamKind::OperandA:
        return "operand_a";
      case StreamKind::OperandB:
        return "operand_b";
      case StreamKind::Output:
        return "output";
    }

    throw std::invalid_argument("unknown SAU stream kind");
}

} // anonymous namespace

// ==================== SauModel 构造 ====================

SauModel::SauModel(const Params &params)
    : ClockedObject(params),                       // 注册时钟域
      system(params.system),
      requestorId(system->getRequestorId(this)),  // 从 System 获取全局唯一 ID
      memoryPort(name() + ".memory", *this, requestorId, params.beat_bytes),
      beatBytes(params.beat_bytes),
      readIssueWidth(params.read_issue_width),
      writeIssueWidth(params.write_issue_width),
      maxOutstandingReads(params.max_outstanding_reads),
      maxOutstandingWrites(params.max_outstanding_writes),
      inputBufferEntries(params.input_buffer_entries),
      outputBufferEntries(params.output_buffer_entries),
      arrayFillCycles(params.array_fill_cycles),
      arrayIiCycles(params.array_ii_cycles),
      arrayCapacity(params.array_capacity),
      arrayInputStartDelayCycles(params.array_input_start_delay_cycles),
      arrayInputBurstBeats(params.array_input_burst_beats),
      arrayInputBurstGapCycles(params.array_input_burst_gap_cycles),
      arrayInputFlowGapCycles(params.array_input_flow_gap_cycles),
      arrayInputSkewCycles(params.array_input_skew_cycles),
      bReadStartAheadBeats(params.b_read_start_ahead_beats),
      resultFlowGapCycles(params.result_flow_gap_cycles),
      writebackStartDelayCycles(params.writeback_start_delay_cycles),
      completionDelayCycles(params.completion_delay_cycles),
      commandStartCycles(params.command_start_cycles),
      calibrationMemory(params.calibration_memory),
      calibrationReadLatencyCycles(params.calibration_read_latency_cycles),
      commandCount(params.command_count),
      interCommandGapCycles(params.inter_command_gap_cycles),
      aCommandStride(params.a_command_stride),
      bCommandStride(params.b_command_stride),
      outputCommandStride(params.output_command_stride),
      exitOnDone(params.exit_on_done),
      startupCommand(buildStartupCommand(params)),  // 将 Python 参数转为 SauCommand
      outputBuffer(outputBufferEntries),
      arrayPipeline(arrayFillCycles, arrayIiCycles, arrayCapacity),
      traceWriter(params.trace_file),               // 打开 trace CSV 文件
      startupEvent([this] { submitNextCommand(); }, name() + ".startup"),
      tickEvent([this] { tick(); }, name() + ".tick"),
      stats(this, commandCount)                     // 统计组挂靠在 SauModel 下
{
    // 参数合法性检查（构造时一次性验证）
    panic_if(beatBytes != 32,
             "first SAU milestone requires a 32-byte memory beat");
    panic_if(readIssueWidth == 0 || writeIssueWidth == 0,
             "SAU issue widths must be nonzero");
    panic_if(maxOutstandingReads == 0 || maxOutstandingWrites == 0,
             "SAU outstanding limits must be nonzero");
    panic_if(inputBufferEntries == 0 || outputBufferEntries == 0,
             "SAU buffer capacities must be nonzero");
    panic_if(arrayFillCycles == Cycles(0) || arrayIiCycles == Cycles(0) ||
                 arrayCapacity == 0,
             "SAU array timing and capacity must be nonzero");
    panic_if(arrayInputBurstBeats == 0,
             "SAU array-input burst length must be nonzero");
    panic_if(calibrationMemory && calibrationReadLatencyCycles == Cycles(0),
             "SAU calibration-memory read latency must be nonzero");
    panic_if(commandCount == 0, "SAU synthetic command count must be nonzero");
}

// ==================== gem5 SimObject 标准接口 ====================

Port &
SauModel::getPort(const std::string &ifName, PortID idx)
{
    // gem5 在拓扑连接时调用，通过名称查找端口
    if (ifName == "memory") {
        panic_if(idx != InvalidPortID, "SAU memory port is not a vector port");
        return memoryPort;          // 返回唯一的 memory 端口
    }
    return ClockedObject::getPort(ifName, idx); // 委托基类处理其他端口
}

void
SauModel::startup()
{
    // startup() 发生在 gem5 重置统计项之前。把首条命令排到第一个
    // 时钟边沿，使 commandsAccepted 与后续命令使用同一个统计窗口。
    schedule(startupEvent, nextCycle());
}

void
SauModel::submitCommand(const SauCommand &command)
{
    // 首阶段只支持单命令排队（不支持流水线执行）
    if (activeCommand) {
        throw std::logic_error("SAU already has an active command");
    }

    // 校验并记录命令
    validateCommand(command, beatBytes);
    activeCommand = command;
    readGenerator.emplace(command);
    aRegisterFile.emplace(command);
    arrayInputScheduler.emplace(
        command.workItems, arrayInputSkewCycles, arrayInputBurstBeats,
        arrayInputsPerFlow(), arrayInputBurstGapCycles,
        arrayInputFlowGapCycles);
    resultScheduler.emplace(
        expectedOutputBeats(), outputBeatsPerFlow(), arrayFillCycles,
        resultFlowGapCycles);
    arrayPipeline.reset();
    availableB.clear();
    visibleMemoryResponses.clear();
    nextResultIndex = 0;
    nextWriteIndex = 0;
    acceptedReadBeats = 0;
    visibleReadBeats = 0;
    arrayAdmissions = 0;
    resultsProduced = 0;
    writesAccepted = 0;
    bReadCooldownCycles = Cycles(0);
    readAcceptedTraceA = 0;
    readAcceptedTraceB = 0;
    readResponseTraceA = 0;
    readResponseTraceB = 0;
    lastResultCycle.reset();
    lastWriteCycle.reset();
    firstReadCycle.reset();
    firstArrayInputCycle.reset();
    firstResultCycle.reset();
    calibrationReadResponses.clear();
    phase = Phase::OperandLoad;
    commandStartCycle = sauCycle;

    // 统计和 trace
    ++stats.commandsAccepted;
    traceWriter.emit(sauCycle, EventKind::PhaseChanged, command.id, "none",
                     0, 0, phase);
    traceWriter.emit(sauCycle, EventKind::CommandAccepted, command.id, "none",
                     0, 0, phase);

    // The feeder delay is applied by issueReads() in command-local cycles.
    // Keep the clock active so that delay itself is represented in the trace.
    schedule(tickEvent, clockEdge(Cycles(1)));
}

// ==================== 核心调度：每个时钟边沿执行 ====================

void
SauModel::tick()
{
    bool acceptedCommandThisTick = false;
    if (!activeCommand && nextCommandStartCycle &&
        Cycles(sauCycle) >= *nextCommandStartCycle) {
        nextCommandStartCycle.reset();
        submitNextCommand();
        acceptedCommandThisTick = true;
    }

    if (acceptedCommandThisTick) {
        ++sauCycle;
        if (hasPendingWork() && !tickEvent.scheduled()) {
            schedule(tickEvent, clockEdge(Cycles(1)));
        }
        return;
    }

    panic_if(memoryPort.hasBlockedPacket() && memoryPort.canIssue(),
             "blocked packet must stop new issue");
    panic_if(outstandingReads() > maxOutstandingReads && !calibrationMemory,
             "read outstanding limit exceeded");
    panic_if(outstandingWrites() > maxOutstandingWrites && !calibrationMemory,
             "write outstanding limit exceeded");

    // 没有活跃命令 → 空转返回
    if (!activeCommand) {
        auto responses = takeVisibleReadResponses();
        panic_if(!responses.empty(),
                 "SAU memory response arrived without an active command");
        ++sauCycle;
        if (hasPendingWork() && !tickEvent.scheduled()) {
            schedule(tickEvent, clockEdge(Cycles(1)));
        }
        return;
    }

    issueReads();

    auto responses = takeVisibleReadResponses();
    for (const auto &beat : responses) {
        visibleMemoryResponses.push_back(beat);
        auto &traceBeat = beat.stream == StreamKind::OperandA ?
            readResponseTraceA : readResponseTraceB;
        traceWriter.emit(
            sauCycle, EventKind::ReadResponseVisible, activeCommand->id,
            streamName(beat.stream), 0, traceBeat++, phase);
    }

    consumeResponses();
    advanceArray();
    produceResults();
    issueWrites();
    updatePhase();
    accountCycle();
    checkConservation();

    ++sauCycle;
    if (hasPendingWork() && !tickEvent.scheduled()) {
        schedule(tickEvent, clockEdge(Cycles(1)));
    }
}

void
SauModel::consumeResponses()
{
    for (const auto &beat : visibleMemoryResponses) {
        ++visibleReadBeats;
        if (beat.stream == StreamKind::OperandA) {
            aRegisterFile->load(beat);
        } else if (beat.stream == StreamKind::OperandB) {
            availableB.push_back(beat);
        } else {
            panic("unexpected SAU read response stream");
        }
    }
    visibleMemoryResponses.clear();
    stats.maxInputBufferOccupancy = std::max(
        static_cast<unsigned>(stats.maxInputBufferOccupancy.value()),
        static_cast<unsigned>(availableB.size()));
}

void
SauModel::advanceArray()
{
    if (!activeCommand || !arrayInputScheduler ||
        arrayInputScheduler->complete()) {
        return;
    }
    if (commandCycle() < arrayInputStartDelayCycles) {
        return;
    }

    const bool bWanted = arrayInputScheduler->canIssueB();
    bool bProgressed = false;
    bool progressed = false;

    // RTL trace order is B before A when both streams enter the array in the
    // same cycle.  Keep that ordering while allowing A to run ahead by the
    // configured skew window.
    if (bWanted) {
        bProgressed = advanceArrayB();
        progressed = bProgressed || progressed;
    }

    const bool aWanted = arrayInputScheduler->canIssueA();
    if (aWanted) {
        progressed = advanceArrayA(bProgressed) || progressed;
    }

    if (!progressed && (aWanted || bWanted)) {
        if (arrayPipeline.full()) {
            ++stats.stallArrayCapacity;
        } else if (bWanted && availableB.empty()) {
            ++stats.stallInputStarvation;
        } else if (aWanted && !instructionReadyForArrayIndex(
                       arrayInputScheduler->issuedA())) {
            ++stats.stallInputStarvation;
        }
    }

    arrayInputScheduler->advanceCycle();
}

bool
SauModel::advanceArrayB()
{
    assert(activeCommand);
    assert(arrayInputScheduler);

    if (!arrayInputScheduler->canIssueB()) {
        return false;
    }
    if (availableB.empty()) {
        return false;
    }
    if (!arrayPipeline.canAccept(commandCycle())) {
        return false;
    }
    const auto bBeat = availableB.front();
    const uint32_t arrayIndex = arrayInputScheduler->issueB();
    availableB.pop_front();
    const bool lastWork = arrayIndex + 1 == activeCommand->workItems;

    if (phase == Phase::OperandLoad) {
        transitionTo(Phase::ArrayActive);
    }
    if (resultScheduler && !resultScheduler->started()) {
        resultScheduler->start(commandCycle());
    }
    if (lastWork) {
        transitionTo(Phase::ArrayDrain);
    }

    traceWriter.emit(
        sauCycle, EventKind::ArrayInputAccepted, activeCommand->id,
        streamName(bBeat.stream), 0, arrayIndex, phase);

    if (!firstArrayInputCycle) {
        firstArrayInputCycle = commandCycle();
        stats.firstArrayInputOffset[activeCommandIndex] =
            static_cast<uint64_t>(*firstArrayInputCycle);
    }

    arrayPipeline.accept(activeCommand->id, arrayIndex, lastWork,
                         commandCycle());

    ++arrayAdmissions;
    return true;
}

bool
SauModel::advanceArrayA(bool allowPipelineBypass)
{
    assert(activeCommand);
    assert(arrayInputScheduler);

    if (!arrayInputScheduler->canIssueA()) {
        return false;
    }

    const uint32_t arrayIndex = arrayInputScheduler->issuedA();
    if (!instructionReadyForArrayIndex(arrayIndex)) {
        return false;
    }
    const bool pipelineCanAccept = arrayPipeline.canAccept(commandCycle());
    const bool canAcceptAsAdditional =
        allowPipelineBypass && arrayPipeline.canAcceptAdditional();
    if (!pipelineCanAccept && !canAcceptAsAdditional) {
        return false;
    }

    const Beat aBeat = makeArrayABeat(arrayIndex);
    const uint32_t issuedIndex = arrayInputScheduler->issueA();
    assert(issuedIndex == arrayIndex);

    if (phase == Phase::OperandLoad) {
        transitionTo(Phase::ArrayActive);
    }
    if (resultScheduler && !resultScheduler->started()) {
        resultScheduler->start(commandCycle());
    }

    traceWriter.emit(
        sauCycle, EventKind::ArrayInputAccepted, activeCommand->id,
        streamName(aBeat.stream), 0, aBeat.index, phase);

    if (!firstArrayInputCycle) {
        firstArrayInputCycle = commandCycle();
        stats.firstArrayInputOffset[activeCommandIndex] =
            static_cast<uint64_t>(*firstArrayInputCycle);
    }

    if (pipelineCanAccept) {
        arrayPipeline.accept(activeCommand->id, aBeat.index, aBeat.last,
                             commandCycle());
    } else {
        arrayPipeline.acceptAdditional(activeCommand->id, aBeat.index,
                                       aBeat.last, commandCycle());
    }

    return true;
}

void
SauModel::produceResults()
{
    while (arrayPipeline.hasReady(commandCycle())) {
        arrayPipeline.takeReady(commandCycle());
    }

    if (!activeCommand || !resultScheduler || resultScheduler->complete() ||
        !resultFlowReady()) {
        return;
    }
    resultScheduler->deferUntil(commandCycle());
    if (!resultScheduler->canProduce(commandCycle())) {
        return;
    }
    if (!outputBuffer.canPush()) {
        ++stats.stallOutputBufferFull;
        return;
    }

    const uint32_t resultIndex = resultScheduler->produce(commandCycle());
    const bool last = resultIndex + 1 == expectedOutputBeats();
    outputBuffer.push({activeCommand->id, resultIndex, commandCycle(), last});
    lastResultCycle = commandCycle();
    if (!firstResultCycle) {
        firstResultCycle = commandCycle();
        stats.firstResultOffset[activeCommandIndex] =
            static_cast<uint64_t>(*firstResultCycle);
    }
    stats.lastResultOffset[activeCommandIndex] =
        static_cast<uint64_t>(*lastResultCycle);

    traceWriter.emit(sauCycle, EventKind::ResultProduced,
                     activeCommand->id, "output", 0, resultIndex, phase);

    ++resultsProduced;
    nextResultIndex = resultScheduler->produced();
    stats.maxOutputBufferOccupancy = std::max(
        static_cast<unsigned>(stats.maxOutputBufferOccupancy.value()),
        static_cast<unsigned>(outputBuffer.size()));
}

void
SauModel::issueWrites()
{
    if (!activeCommand || outputBuffer.size() == 0 ||
        memoryBlocked()) {
        return;
    }
    // RTL calibration keeps all output tokens until the result stream is
    // complete. A smaller FIFO cannot satisfy that policy, so in that DSE
    // regime it begins draining once tokens are available. The calibrated
    // default (256 slots for 256 outputs) keeps the RTL path unchanged.
    const bool waitForFullResultStream =
        outputBufferEntries >= expectedOutputBeats();
    if ((waitForFullResultStream &&
         resultsProduced != expectedOutputBeats()) || !lastResultCycle ||
        static_cast<uint64_t>(commandCycle()) <
            static_cast<uint64_t>(*lastResultCycle +
                                  writebackStartDelayCycles)) {
        return;
    }
    if (!calibrationMemory && outstandingWrites() >= maxOutstandingWrites) {
        ++stats.stallOutstandingWriteLimit;
        return;
    }

    unsigned issued = 0;
    const unsigned issueLimit = calibrationMemory ? 1 : writeIssueWidth;
    while (issued < issueLimit && outputBuffer.size() > 0 &&
           (calibrationMemory || outstandingWrites() < maxOutstandingWrites) &&
           memoryCanIssue()) {
        const auto &token = outputBuffer.front();
        const uint32_t beatIndex = token.index;
        Beat writeBeat{
            StreamKind::Output,
            activeCommand->output.base +
                static_cast<Addr>(beatIndex) *
                    activeCommand->output.strideBytes,
            beatIndex,
            beatIndex + 1 == expectedOutputBeats(),
        };

        if (phase != Phase::Writeback) {
            transitionTo(Phase::Writeback);
        }

        const bool acceptedOrBlocked = calibrationMemory ?
            true : memoryPort.trySend(writeBeat, true);
        if (calibrationMemory) {
            requestAccepted(writeBeat, true);
        }
        outputBuffer.pop();
        ++nextWriteIndex;
        ++issued;
        if (!acceptedOrBlocked) {
            ++stats.stallRequestRetry;
            break;
        }
    }
}

void
SauModel::issueReads()
{
    if (!activeCommand || !readGenerator || readGenerator->empty() ||
        memoryBlocked()) {
        return;
    }
    // The feeder starts after command acceptance.  This is distinct from
    // physical event scheduling: it defines the command-local trace boundary
    // between command acceptance and the first external read.
    if (commandCycle() < commandStartCycles) {
        return;
    }
    if (!calibrationMemory && outstandingReads() >= maxOutstandingReads) {
        ++stats.stallOutstandingReadLimit;
        return;
    }

    unsigned issued = 0;
    const unsigned issueLimit = calibrationMemory ? 1 : readIssueWidth;
    while (issued < issueLimit && !readGenerator->empty() &&
           (calibrationMemory || outstandingReads() < maxOutstandingReads) &&
           memoryCanIssue()) {
        const Beat beat = readGenerator->front();
        if (beat.stream == StreamKind::OperandB && bReadInCooldown()) {
            break;
        }
        if (!canIssueReadBeat(beat)) {
            break;
        }
        const bool acceptedOrBlocked = calibrationMemory ?
            true : memoryPort.trySend(beat, false);
        if (calibrationMemory) {
            calibrationReadResponses.push_back({
                commandCycle() + calibrationReadLatencyCycles, beat});
            requestAccepted(beat, false);
            if (beat.stream == StreamKind::OperandB) {
                applyBReadCooldown();
            }
        }
        readGenerator->pop();
        ++issued;
        if (!acceptedOrBlocked) {
            ++stats.stallRequestRetry;
            break;
        }
    }
}

bool
SauModel::bReadInCooldown()
{
    if (!calibrationMemory || bReadCooldownCycles == Cycles(0)) {
        return false;
    }

    bReadCooldownCycles =
        Cycles(static_cast<uint64_t>(bReadCooldownCycles) - 1);
    return true;
}

void
SauModel::applyBReadCooldown()
{
    if (!calibrationMemory || !activeCommand ||
        readAcceptedTraceB == 0 || readAcceptedTraceB >=
            activeCommand->operandB.beats * activeCommand->flowLoops *
                activeCommand->instructionLoops) {
        return;
    }

    if (readAcceptedTraceB % activeCommand->operandB.beats == 0) {
        bReadCooldownCycles = arrayInputFlowGapCycles;
    } else if (readAcceptedTraceB % arrayInputBurstBeats == 0) {
        bReadCooldownCycles = arrayInputBurstGapCycles;
    }
}

std::vector<Beat>
SauModel::takeVisibleReadResponses()
{
    if (!calibrationMemory) {
        return memoryPort.takeVisibleResponses();
    }

    std::vector<Beat> responses;
    if (!calibrationReadResponses.empty() &&
        calibrationReadResponses.front().visibleCycle <= commandCycle()) {
        responses.push_back(calibrationReadResponses.front().beat);
        calibrationReadResponses.pop_front();
        responseAvailable();
    }
    return responses;
}

bool
SauModel::memoryBlocked() const
{
    return !calibrationMemory && memoryPort.hasBlockedPacket();
}

bool
SauModel::memoryCanIssue() const
{
    return calibrationMemory || memoryPort.canIssue();
}

unsigned
SauModel::outstandingReads() const
{
    if (calibrationMemory) {
        return calibrationReadResponses.size();
    }
    return memoryPort.outstandingReads();
}

unsigned
SauModel::outstandingWrites() const
{
    if (calibrationMemory) {
        return 0;
    }
    return memoryPort.outstandingWrites();
}

bool
SauModel::canIssueReadBeat(const Beat &beat) const
{
    if (beat.stream != StreamKind::OperandB || bReadStartAheadBeats == 0) {
        return true;
    }

    if (!activeCommand || !arrayInputScheduler) {
        return false;
    }

    const uint32_t aPreloadBeats =
        activeCommand->operandA.beats * activeCommand->instructionLoops;
    return readResponseTraceA >= aPreloadBeats &&
        arrayInputScheduler->issuedA() >= bReadStartAheadBeats;
}

void
SauModel::updatePhase()
{
    if (!activeCommand) {
        return;
    }

    if (phase == Phase::ArrayActive &&
        arrayAdmissions == activeCommand->workItems) {
        transitionTo(Phase::ArrayDrain);
    }

    if (commandLocallyComplete()) {
        transitionTo(Phase::Complete);
        traceWriter.emit(sauCycle, EventKind::CommandComplete,
                         activeCommand->id, "none", 0, 0, phase);
        stats.completeOffset[activeCommandIndex] =
            static_cast<uint64_t>(commandCycle());
        ++stats.commandsCompleted;
        const bool moreCommands = nextCommandIndex < commandCount;
        activeCommand.reset();
        readGenerator.reset();
        aRegisterFile.reset();
        arrayInputScheduler.reset();
        resultScheduler.reset();
        availableB.clear();
        if (moreCommands) {
            nextCommandStartCycle =
                Cycles(sauCycle) + interCommandGapCycles;
        } else if (exitOnDone) {
            exitSimLoop("SAU command complete");
        }
        if (!moreCommands) {
            signalDrainDone();
        }
    }
}

void
SauModel::accountCycle()
{
    if (!activeCommand) {
        return;
    }

    ++stats.commandCycles;
    stats.averageOutstandingReadCount = outstandingReads();
    stats.averageOutstandingWriteCount = outstandingWrites();
    stats.averageInputBufferOccupancy = availableB.size();
    stats.averageOutputBufferOccupancy = outputBuffer.size();
    switch (phase) {
      case Phase::OperandLoad:
        ++stats.operandLoadCycles;
        break;
      case Phase::ArrayActive:
        ++stats.arrayActiveCycles;
        break;
      case Phase::ArrayDrain:
        ++stats.arrayDrainCycles;
        break;
      case Phase::Writeback:
        ++stats.writebackCycles;
        break;
      case Phase::Idle:
      case Phase::Complete:
        break;
    }
}

void
SauModel::transitionTo(Phase newPhase)
{
    if (phase == newPhase) {
        return;
    }
    phase = newPhase;
    traceWriter.emit(sauCycle, EventKind::PhaseChanged, activeCommand->id,
                     "none", 0, 0, phase);
}

void
SauModel::checkConservation() const
{
    assert(visibleReadBeats <= acceptedReadBeats);
    assert(resultsProduced <= arrayAdmissions);
    assert(writesAccepted <= resultsProduced);
    assert(writesAccepted <= nextWriteIndex);
    assert(nextWriteIndex <= resultsProduced);
    assert(outputBuffer.size() + writesAccepted <= resultsProduced);
    if (aRegisterFile) {
        assert(arrayAdmissions <= aRegisterFile->totalArrayInputBeats());
    }
    if (arrayInputScheduler) {
        assert(arrayInputScheduler->issuedB() == arrayAdmissions);
        assert(arrayInputScheduler->issuedB() <=
               arrayInputScheduler->issuedA());
        assert(arrayInputScheduler->issuedA() <=
               arrayInputScheduler->totalInputs());
    }
    if (resultScheduler) {
        assert(resultScheduler->produced() == resultsProduced);
        assert(resultScheduler->produced() <= resultScheduler->total());
    }
    assert(availableB.size() <= visibleReadBeats);
}

bool
SauModel::hasPendingWork() const
{
    return startupEvent.scheduled() || activeCommand || memoryBlocked() ||
        outstandingReads() != 0 ||
        outstandingWrites() != 0 ||
        !visibleMemoryResponses.empty() ||
        nextCommandStartCycle.has_value();
}

bool
SauModel::commandLocallyComplete() const
{
    return activeCommand &&
        writesAccepted == expectedOutputBeats() &&
        outputBuffer.size() == 0 &&
        arrayPipeline.inFlight() == 0 &&
        arrayInputScheduler && arrayInputScheduler->complete() &&
        resultScheduler && resultScheduler->complete() &&
        lastWriteCycle &&
        static_cast<uint64_t>(commandCycle()) >=
            static_cast<uint64_t>(*lastWriteCycle + completionDelayCycles) &&
        (!readGenerator || readGenerator->empty()) &&
        visibleReadBeats == acceptedReadBeats &&
        visibleMemoryResponses.empty() &&
        !memoryBlocked() &&
        outstandingReads() == 0 &&
        outstandingWrites() == 0;
}

Cycles
SauModel::commandCycle() const
{
    assert(sauCycle >= commandStartCycle);
    return Cycles(sauCycle - commandStartCycle);
}

SauCommand
SauModel::buildCommandForIndex(uint32_t index) const
{
    auto command = startupCommand;
    command.id = startupCommand.id + index;
    command.operandA.base += static_cast<Addr>(index) * aCommandStride;
    command.operandB.base += static_cast<Addr>(index) * bCommandStride;
    command.output.base += static_cast<Addr>(index) * outputCommandStride;
    return command;
}

void
SauModel::submitNextCommand()
{
    panic_if(nextCommandIndex >= commandCount,
             "SAU synthetic command index exceeds command count");
    activeCommandIndex = nextCommandIndex;
    submitCommand(buildCommandForIndex(nextCommandIndex++));
}

uint32_t
SauModel::expectedOutputBeats() const
{
    if (!activeCommand) {
        return 0;
    }
    return activeCommand->output.beats * activeCommand->instructionLoops;
}

uint32_t
SauModel::outputBeatsPerFlow() const
{
    assert(activeCommand);
    const uint32_t totalFlows =
        activeCommand->flowLoops * activeCommand->instructionLoops;
    const uint32_t totalOutputs = expectedOutputBeats();
    if (totalOutputs % totalFlows != 0) {
        throw std::invalid_argument(
            "SAU output beats must divide evenly across flows");
    }
    return totalOutputs / totalFlows;
}

uint32_t
SauModel::arrayInputsPerFlow() const
{
    assert(activeCommand);
    const uint32_t totalFlows =
        activeCommand->flowLoops * activeCommand->instructionLoops;
    if (activeCommand->workItems % totalFlows != 0) {
        throw std::invalid_argument(
            "SAU work items must divide evenly across flows");
    }
    return activeCommand->workItems / totalFlows;
}

bool
SauModel::resultFlowReady() const
{
    if (!activeCommand || !resultScheduler || resultScheduler->complete()) {
        return false;
    }

    const uint64_t flow =
        resultScheduler->produced() / outputBeatsPerFlow();
    const uint64_t requiredInputs = (flow + 1) * arrayInputsPerFlow();
    return arrayAdmissions >= requiredInputs;
}

bool
SauModel::instructionReadyForArrayIndex(uint32_t index) const
{
    if (!activeCommand || !aRegisterFile) {
        return false;
    }

    const uint32_t perInstruction =
        activeCommand->operandA.beats * activeCommand->flowLoops;
    const uint32_t instruction = index / perInstruction;
    return instruction < activeCommand->instructionLoops &&
        aRegisterFile->instructionReady(instruction);
}

Beat
SauModel::makeArrayABeat(uint32_t index) const
{
    assert(activeCommand);
    assert(aRegisterFile);

    const uint32_t beatsPerFlow = activeCommand->operandA.beats;
    const uint32_t perInstruction = beatsPerFlow * activeCommand->flowLoops;
    const uint32_t instruction = index / perInstruction;
    const uint32_t withinInstruction = index % perInstruction;
    const uint32_t flow = withinInstruction / beatsPerFlow;
    const uint32_t beat = withinInstruction % beatsPerFlow;

    return aRegisterFile->arrayInputBeat(instruction, flow, beat, index);
}

void
SauModel::requestAccepted(const Beat &beat, bool write)
{
    panic_if(!activeCommand,
             "SAU memory request accepted without an active command");

    if (write) {
        ++stats.writeRequests;
        stats.writeBytes += beatBytes;
        stats.maxOutstandingWriteCount = std::max(
            static_cast<unsigned>(
                stats.maxOutstandingWriteCount.value()),
            outstandingWrites());
    } else {
        ++stats.readRequests;
        stats.readBytes += beatBytes;
        stats.maxOutstandingReadCount = std::max(
            static_cast<unsigned>(
                stats.maxOutstandingReadCount.value()),
            outstandingReads());
    }

    uint32_t traceBeat = beat.index;
    if (write) {
        ++writesAccepted;
        if (writesAccepted == expectedOutputBeats()) {
            lastWriteCycle = commandCycle();
        }
        stats.lastWriteOffset[activeCommandIndex] =
            static_cast<uint64_t>(commandCycle());
    } else {
        ++acceptedReadBeats;
        if (!firstReadCycle) {
            firstReadCycle = commandCycle();
            stats.firstReadOffset[activeCommandIndex] =
                static_cast<uint64_t>(*firstReadCycle);
        }
        auto &counter = beat.stream == StreamKind::OperandA ?
            readAcceptedTraceA : readAcceptedTraceB;
        traceBeat = counter++;
    }

    traceWriter.emit(sauCycle,
                     write ? EventKind::WriteAccepted :
                         EventKind::ReadAccepted,
                     activeCommand->id, streamName(beat.stream),
                     beat.address, traceBeat, phase);
}

void
SauModel::responseAvailable()
{
    if (!tickEvent.scheduled()) {
        schedule(tickEvent, nextCycle());
    }
}

// ==================== Drain（仿真暂停/checkpoint 支持） ====================

DrainState
SauModel::drain()
{
    if (!hasPendingWork()) {
        if (tickEvent.scheduled()) {
            deschedule(tickEvent);
        }
        return DrainState::Drained;
    }

    if (!tickEvent.scheduled()) {
        schedule(tickEvent, clockEdge(Cycles(1)));
    }
    return DrainState::Draining;
}

void
SauModel::drainResume()
{
    // 恢复仿真：如果有活跃命令且 tick 未调度，则重新启动
    if (hasPendingWork() && !tickEvent.scheduled()) {
        schedule(tickEvent, clockEdge(Cycles(1)));
    } else if (!hasPendingWork() && tickEvent.scheduled()) {
        deschedule(tickEvent);
    }
}

// ==================== 统计注册 ====================

SauModel::SauStats::SauStats(statistics::Group *parent, unsigned commandCount)
    : statistics::Group(parent),
      ADD_STAT(commandsAccepted, statistics::units::Count::get(),
               "Commands accepted"),
      ADD_STAT(commandsCompleted, statistics::units::Count::get(),
               "Commands completed"),
      ADD_STAT(commandCycles, statistics::units::Cycle::get(),
               "Cycles with an active command"),
      ADD_STAT(operandLoadCycles, statistics::units::Cycle::get(),
               "Cycles in operand-load phase"),
      ADD_STAT(arrayActiveCycles, statistics::units::Cycle::get(),
               "Cycles in array-active phase"),
      ADD_STAT(arrayDrainCycles, statistics::units::Cycle::get(),
               "Cycles in array-drain phase"),
      ADD_STAT(writebackCycles, statistics::units::Cycle::get(),
               "Cycles in writeback phase"),
      ADD_STAT(readRequests, statistics::units::Count::get(),
               "Read requests accepted"),
      ADD_STAT(readBytes, statistics::units::Byte::get(), "Bytes read"),
      ADD_STAT(writeRequests, statistics::units::Count::get(),
               "Write requests accepted"),
      ADD_STAT(writeBytes, statistics::units::Byte::get(), "Bytes written"),
      ADD_STAT(maxOutstandingReadCount, statistics::units::Count::get(),
               "Maximum outstanding reads"),
      ADD_STAT(maxOutstandingWriteCount, statistics::units::Count::get(),
               "Maximum outstanding writes"),
      ADD_STAT(maxInputBufferOccupancy, statistics::units::Count::get(),
               "Maximum input-buffer occupancy"),
      ADD_STAT(maxOutputBufferOccupancy, statistics::units::Count::get(),
               "Maximum output-buffer occupancy"),
      ADD_STAT(averageOutstandingReadCount, statistics::units::Count::get(),
               "Time-average outstanding reads"),
      ADD_STAT(averageOutstandingWriteCount, statistics::units::Count::get(),
               "Time-average outstanding writes"),
      ADD_STAT(averageInputBufferOccupancy, statistics::units::Count::get(),
               "Time-average input-buffer occupancy"),
      ADD_STAT(averageOutputBufferOccupancy, statistics::units::Count::get(),
               "Time-average output-buffer occupancy"),
      ADD_STAT(stallRequestRetry, statistics::units::Cycle::get(),
               "Cycles stalled by memory request retry"),
      ADD_STAT(stallOutstandingReadLimit, statistics::units::Cycle::get(),
               "Cycles stalled by the outstanding-read limit"),
      ADD_STAT(stallOutstandingWriteLimit, statistics::units::Cycle::get(),
               "Cycles stalled by the outstanding-write limit"),
      ADD_STAT(stallInputStarvation, statistics::units::Cycle::get(),
               "Cycles stalled by missing input tokens"),
      ADD_STAT(stallOutputBufferFull, statistics::units::Cycle::get(),
               "Cycles stalled by a full output buffer"),
      ADD_STAT(stallWritebackBlocked, statistics::units::Cycle::get(),
               "Cycles stalled by blocked writeback"),
      ADD_STAT(stallArrayCapacity, statistics::units::Cycle::get(),
               "Cycles stalled by full array in-flight capacity"),
      ADD_STAT(firstReadOffset, statistics::units::Cycle::get(),
               "Command acceptance to first read, indexed by command"),
      ADD_STAT(firstArrayInputOffset, statistics::units::Cycle::get(),
               "Command acceptance to first array input, indexed by command"),
      ADD_STAT(firstResultOffset, statistics::units::Cycle::get(),
               "Command acceptance to first result, indexed by command"),
      ADD_STAT(lastResultOffset, statistics::units::Cycle::get(),
               "Command acceptance to last result, indexed by command"),
      ADD_STAT(lastWriteOffset, statistics::units::Cycle::get(),
               "Command acceptance to last write, indexed by command"),
      ADD_STAT(completeOffset, statistics::units::Cycle::get(),
               "Command acceptance to completion, indexed by command"),
      ADD_STAT(arrayUtilization, statistics::units::Ratio::get(),
               "Fraction of active command cycles with array activity",
               arrayActiveCycles / commandCycles)
{
    firstReadOffset.init(commandCount);
    firstArrayInputOffset.init(commandCount);
    firstResultOffset.init(commandCount);
    lastResultOffset.init(commandCount);
    lastWriteOffset.init(commandCount);
    completeOffset.init(commandCount);
}

} // namespace gem5::sau
