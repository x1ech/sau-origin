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
            static_cast<uint32_t>(params.beat_bytes),
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
      resultFlowGapCycles(params.result_flow_gap_cycles),
      writebackStartDelayCycles(params.writeback_start_delay_cycles),
      completionDelayCycles(params.completion_delay_cycles),
      commandStartCycles(params.command_start_cycles),
      exitOnDone(params.exit_on_done),
      startupCommand(buildStartupCommand(params)),  // 将 Python 参数转为 SauCommand
      outputBuffer(outputBufferEntries),
      arrayPipeline(arrayFillCycles, arrayIiCycles, arrayCapacity),
      traceWriter(params.trace_file),               // 打开 trace CSV 文件
      tickEvent([this] { tick(); }, name() + ".tick"),
      stats(this)                                   // 统计组挂靠在 SauModel 下
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
    // 仿真启动时自动注入 synthetic 命令
    submitCommand(startupCommand);
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
    availableB.clear();
    visibleMemoryResponses.clear();
    nextResultIndex = 0;
    nextWriteIndex = 0;
    acceptedReadBeats = 0;
    visibleReadBeats = 0;
    arrayAdmissions = 0;
    resultsProduced = 0;
    writesAccepted = 0;
    readAcceptedTraceA = 0;
    readAcceptedTraceB = 0;
    readResponseTraceA = 0;
    readResponseTraceB = 0;
    lastResultCycle.reset();
    lastWriteCycle.reset();
    phase = Phase::OperandLoad;
    sauCycle = 0;

    // 统计和 trace
    ++stats.commandsAccepted;
    traceWriter.emit(sauCycle, EventKind::PhaseChanged, command.id, "none",
                     0, 0, phase);
    traceWriter.emit(sauCycle, EventKind::CommandAccepted, command.id, "none",
                     0, 0, phase);

    // commandStartCycles 拍后开始第一次 tick
    // clockEdge(n) = 当前时钟边沿 + n 个时钟周期
    schedule(tickEvent, clockEdge(commandStartCycles));
}

// ==================== 核心调度：每个时钟边沿执行 ====================

void
SauModel::tick()
{
    panic_if(memoryPort.hasBlockedPacket() && memoryPort.canIssue(),
             "blocked packet must stop new issue");
    panic_if(memoryPort.outstandingReads() > maxOutstandingReads,
             "read outstanding limit exceeded");
    panic_if(memoryPort.outstandingWrites() > maxOutstandingWrites,
             "write outstanding limit exceeded");

    auto responses = memoryPort.takeVisibleResponses();
    panic_if(!responses.empty() && !activeCommand,
             "SAU memory response arrived without an active command");
    for (const auto &beat : responses) {
        visibleMemoryResponses.push_back(beat);
        auto &traceBeat = beat.stream == StreamKind::OperandA ?
            readResponseTraceA : readResponseTraceB;
        traceWriter.emit(
            sauCycle, EventKind::ReadResponseVisible, activeCommand->id,
            streamName(beat.stream), 0, traceBeat++, phase);
    }

    // 没有活跃命令 → 空转返回
    if (!activeCommand) {
        return;
    }

    consumeResponses();
    advanceArray();
    produceResults();
    issueWrites();
    issueReads();
    updatePhase();
    accountCycle();
    checkConservation();

    ++sauCycle;
    if (hasPendingWork()) {
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
    if (Cycles(sauCycle) < arrayInputStartDelayCycles) {
        return;
    }

    const bool bWanted = arrayInputScheduler->canIssueB();
    bool progressed = false;

    // RTL trace order is B before A when both streams enter the array in the
    // same cycle.  Keep that ordering while allowing A to run ahead by the
    // configured skew window.
    if (bWanted) {
        progressed = advanceArrayB() || progressed;
    }

    const bool aWanted = arrayInputScheduler->canIssueA();
    if (aWanted) {
        progressed = advanceArrayA() || progressed;
    }

    if (!progressed && (aWanted || bWanted)) {
        if (bWanted && availableB.empty()) {
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
    const auto bBeat = availableB.front();
    const uint32_t arrayIndex = arrayInputScheduler->issueB();
    availableB.pop_front();
    const bool lastWork = arrayIndex + 1 == activeCommand->workItems;

    if (phase == Phase::OperandLoad) {
        transitionTo(Phase::ArrayActive);
    }
    if (resultScheduler && !resultScheduler->started()) {
        resultScheduler->start(Cycles(sauCycle));
    }
    if (lastWork) {
        transitionTo(Phase::ArrayDrain);
    }

    traceWriter.emit(
        sauCycle, EventKind::ArrayInputAccepted, activeCommand->id,
        streamName(bBeat.stream), 0, arrayIndex, phase);

    arrayPipeline.accept(activeCommand->id, arrayIndex, lastWork,
                         Cycles(sauCycle));

    ++arrayAdmissions;
    return true;
}

bool
SauModel::advanceArrayA()
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
    if (!arrayPipeline.canAccept(Cycles(sauCycle))) {
        return false;
    }

    const Beat aBeat = makeArrayABeat(arrayIndex);
    const uint32_t issuedIndex = arrayInputScheduler->issueA();
    assert(issuedIndex == arrayIndex);

    if (phase == Phase::OperandLoad) {
        transitionTo(Phase::ArrayActive);
    }
    if (resultScheduler && !resultScheduler->started()) {
        resultScheduler->start(Cycles(sauCycle));
    }

    traceWriter.emit(
        sauCycle, EventKind::ArrayInputAccepted, activeCommand->id,
        streamName(aBeat.stream), 0, aBeat.index, phase);

    arrayPipeline.accept(activeCommand->id, aBeat.index, aBeat.last,
                         Cycles(sauCycle));

    return true;
}

void
SauModel::produceResults()
{
    while (activeCommand && resultScheduler &&
           arrayPipeline.hasReady(Cycles(sauCycle))) {
        const bool resultDue = !resultScheduler->complete() &&
            resultScheduler->canProduce(Cycles(sauCycle));
        if (resultDue && !outputBuffer.canPush()) {
            ++stats.stallOutputBufferFull;
            return;
        }

        auto token = arrayPipeline.takeReady(Cycles(sauCycle));
        if (!resultDue) {
            continue;
        }

        const uint32_t resultIndex =
            resultScheduler->produce(Cycles(sauCycle));
        token.index = resultIndex;
        token.last = resultIndex + 1 == expectedOutputBeats();
        outputBuffer.push(token);
        lastResultCycle = Cycles(sauCycle);

        traceWriter.emit(sauCycle, EventKind::ResultProduced,
                         activeCommand->id, "output", 0, token.index, phase);

        ++resultsProduced;
        nextResultIndex = resultScheduler->produced();
        stats.maxOutputBufferOccupancy = std::max(
            static_cast<unsigned>(stats.maxOutputBufferOccupancy.value()),
            static_cast<unsigned>(outputBuffer.size()));
    }
}

void
SauModel::issueWrites()
{
    if (!activeCommand || outputBuffer.size() == 0 ||
        memoryPort.hasBlockedPacket()) {
        return;
    }
    if (resultsProduced != expectedOutputBeats() || !lastResultCycle ||
        static_cast<uint64_t>(sauCycle) <
            static_cast<uint64_t>(*lastResultCycle +
                                  writebackStartDelayCycles)) {
        return;
    }
    if (memoryPort.outstandingWrites() >= maxOutstandingWrites) {
        ++stats.stallOutstandingWriteLimit;
        return;
    }

    unsigned issued = 0;
    while (issued < writeIssueWidth && outputBuffer.size() > 0 &&
           memoryPort.outstandingWrites() < maxOutstandingWrites &&
           memoryPort.canIssue()) {
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

        const bool acceptedOrBlocked = memoryPort.trySend(writeBeat, true);
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
        memoryPort.hasBlockedPacket()) {
        return;
    }
    if (memoryPort.outstandingReads() >= maxOutstandingReads) {
        ++stats.stallOutstandingReadLimit;
        return;
    }

    unsigned issued = 0;
    while (issued < readIssueWidth && !readGenerator->empty() &&
           memoryPort.outstandingReads() < maxOutstandingReads &&
           memoryPort.canIssue()) {
        const Beat beat = readGenerator->front();
        const bool acceptedOrBlocked = memoryPort.trySend(beat, false);
        readGenerator->pop();
        ++issued;
        if (!acceptedOrBlocked) {
            ++stats.stallRequestRetry;
            break;
        }
    }
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
        ++stats.commandsCompleted;
        activeCommand.reset();
        readGenerator.reset();
        aRegisterFile.reset();
        arrayInputScheduler.reset();
        resultScheduler.reset();
        availableB.clear();
        if (exitOnDone) {
            exitSimLoop("SAU command complete");
        }
        signalDrainDone();
    }
}

void
SauModel::accountCycle()
{
    if (!activeCommand) {
        return;
    }

    ++stats.commandCycles;
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
    return activeCommand || memoryPort.hasBlockedPacket() ||
        memoryPort.outstandingReads() != 0 ||
        memoryPort.outstandingWrites() != 0 ||
        !visibleMemoryResponses.empty();
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
        static_cast<uint64_t>(sauCycle) >=
            static_cast<uint64_t>(*lastWriteCycle + completionDelayCycles) &&
        (!readGenerator || readGenerator->empty()) &&
        visibleReadBeats == acceptedReadBeats &&
        visibleMemoryResponses.empty() &&
        !memoryPort.hasBlockedPacket() &&
        memoryPort.outstandingReads() == 0 &&
        memoryPort.outstandingWrites() == 0;
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
            memoryPort.outstandingWrites());
    } else {
        ++stats.readRequests;
        stats.readBytes += beatBytes;
        stats.maxOutstandingReadCount = std::max(
            static_cast<unsigned>(
                stats.maxOutstandingReadCount.value()),
            memoryPort.outstandingReads());
    }

    uint32_t traceBeat = beat.index;
    if (write) {
        ++writesAccepted;
        if (writesAccepted == expectedOutputBeats()) {
            lastWriteCycle = Cycles(sauCycle);
        }
    } else {
        ++acceptedReadBeats;
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

SauModel::SauStats::SauStats(statistics::Group *parent)
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
      ADD_STAT(arrayUtilization, statistics::units::Ratio::get(),
               "Fraction of active command cycles with array activity",
               arrayActiveCycles / commandCycles)
{
}

} // namespace gem5::sau
