#include "sau/sau_model.hh"

#include <algorithm>
#include <cassert>
#include <limits>
#include <stdexcept>

#include "base/logging.hh"
#include "sau/command.hh"
#include "sau/csr_fixture.hh"
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
        static_cast<uint64_t>(params.b_beats) * params.flow_loops *
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

RtlCommandDriverConfig
buildRtlCommandDriverConfig(const SauCommand &command,
                            const TimingPolicy &policy,
                            const RtlTimingParameters &rtl)
{
    const auto &address = command.operandBAddress;
    const uint64_t streamedBeats =
        static_cast<uint64_t>(address.xCount) * address.yCount *
        address.flowCount * address.instructionCount;
    if (rtl.saSize != 32 || !address.enabled ||
        address.xCount != 1 || address.yCount != rtl.saSize ||
        address.flowCount != command.flowLoops ||
        address.instructionCount != policy.scheduleInstructions ||
        streamedBeats != command.workItems ||
        policy.residentLoadBeats % rtl.saSize != 0 ||
        policy.residentLoadBeats / rtl.saSize == 0 ||
        policy.residentLoadBeats / rtl.saSize > 64 ||
        policy.scheduleInstructions > 64 ||
        policy.outputBeats !=
            static_cast<uint64_t>(rtl.saSize) *
                policy.scheduleInstructions ||
        policy.transMode != 0x1 || policy.reuseMode != 0x1) {
        throw std::invalid_argument(
            "unsupported strict RTL command-driver shape");
    }

    const uint32_t residentXBurst =
        policy.residentLoadBeats / rtl.saSize;
    return {
        {rtl.saSize, command.flowLoops, policy.scheduleInstructions,
         policy.transMode, policy.reuseMode,
         command.control.saFlowMode, false},
        {residentXBurst, rtl.saSize, 1},
        {address.xCount, address.yCount, address.flowCount,
         address.instructionCount},
        {address.xCount, address.yCount, address.flowCount,
         address.instructionCount, rtl.saSize, rtl.sramDelay,
         rtl.memAddressDelay, rtl.memCtrlDelay, rtl.registerDelay},
        {rtl.saSize, command.flowLoops, 0, false},
        {rtl.saSize, 4, 4},
        {1, rtl.saSize, 1, policy.scheduleInstructions,
         policy.scheduleInstructions, rtl.saSize, 1},
        rtl.sramDelay,
    };
}

SauScheduleState
projectedScheduleState(RtlCoreState state)
{
    switch (state) {
      case RtlCoreState::Idle:
        return SauScheduleState::Idle;
      case RtlCoreState::RegisterLoad:
        return SauScheduleState::ResidentLoad;
      case RtlCoreState::TransposeLoad:
        return SauScheduleState::TransposeSetup;
      case RtlCoreState::ReuseLoad:
        return SauScheduleState::FlowExecute;
      case RtlCoreState::TransposeClip:
        return SauScheduleState::FlowBoundary;
      case RtlCoreState::FirstLoad:
      case RtlCoreState::DOut:
      case RtlCoreState::RegisterUnload:
        return SauScheduleState::DrainAndWriteback;
    }
    throw std::invalid_argument("unknown RTL core state");
}

std::string_view
projectedTransitionCause(RtlCoreState state)
{
    switch (state) {
      case RtlCoreState::Idle:
        return "command_complete";
      case RtlCoreState::RegisterLoad:
        return "register_load";
      case RtlCoreState::TransposeLoad:
        return "register_load_done";
      case RtlCoreState::ReuseLoad:
        return "transpose_complete";
      case RtlCoreState::TransposeClip:
        return "flow_boundary";
      case RtlCoreState::FirstLoad:
        return "final_flow";
      case RtlCoreState::DOut:
        return "drain_results";
      case RtlCoreState::RegisterUnload:
        return "writeback_drained";
    }
    throw std::invalid_argument("unknown RTL core state");
}

std::string
projectedInputSwitchName(uint8_t inputSwitch)
{
    switch (inputSwitch & 0x3) {
      case 0:
        return "00";
      case 1:
        return "01";
      case 2:
        return "10";
      case 3:
        return "11";
    }
    throw std::invalid_argument("unknown RTL input switch");
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
      strictTiming(params.strict_timing),
      fixtureReplay(!params.csr_fixture.empty()),
      rtlTiming({params.rtl_sa_size, params.rtl_register_depth,
                 params.rtl_sram_delay, params.rtl_sram_data_width,
                 params.rtl_mem_address_delay,
                 params.rtl_memctrl_delay,
                 params.rtl_register_file_address_delay,
                 params.rtl_register_delay}),
      rtlStorageTiming(RtlStorageTiming::derive(rtlTiming)),
      commandCount(params.command_count),
      interCommandGapCycles(params.inter_command_gap_cycles),
      aCommandStride(params.a_command_stride),
      bCommandStride(params.b_command_stride),
      outputCommandStride(params.output_command_stride),
      exitOnDone(params.exit_on_done),
      memoryImageFile(params.memory_image_file),
      memoryImageBase(params.memory_image_base),
      memoryImageWordBytes(params.memory_image_word_bytes),
      functionalMemoryBase(params.functional_memory_base),
      functionalMemorySize(params.functional_memory_size),
      functionalMemoryFill(params.functional_memory_fill),
      finalMemoryDumpFile(params.final_memory_dump_file),
      finalMemoryDumpBase(params.final_memory_dump_base),
      finalMemoryDumpSize(params.final_memory_dump_size),
      boundaryTraceFile(params.boundary_trace_file),
      startupCommand(buildStartupCommand(params)),  // 将 Python 参数转为 SauCommand
      outputBuffer(outputBufferEntries),
      arrayPipeline(arrayFillCycles, arrayIiCycles, arrayCapacity),
      traceWriter(params.trace_file),               // 打开 trace CSV 文件
      timingLedger(params.timing_ledger_file),
      stateTraceWriter(params.state_trace_file),
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
    panic_if(strictTiming && !calibrationMemory,
             "strict SAU timing requires calibration_memory");
    panic_if(strictTiming && params.csr_fixture.empty(),
             "strict SAU timing requires a CSR fixture directory");
    panic_if(strictTiming && beatBytes != rtlStorageTiming.beatBytes,
             "strict SAU timing beat size differs from RTL storage contract");
    panic_if(strictTiming &&
                 (readIssueWidth != rtlStorageTiming.issueWidth ||
                  writeIssueWidth != rtlStorageTiming.issueWidth),
             "strict SAU timing requires the RTL single-issue SRAM port");
    if (fixtureReplay) {
        fixtureCommands = loadCsrFixture(params.csr_fixture, rtlTiming);
        panic_if(fixtureCommands.size() != commandCount,
                 "SAU CSR fixture command count does not match command_count");
        for (const auto &entry : fixtureCommands) {
            // PLAN3 fail-fast contract: a legal raw configuration whose
            // resource path is not executable yet must stop before
            // producing any result or timing statistic.
            if (entry.decoded.maturity != ValidationMaturity::ResourceTimed) {
                fatal("SAU command %d is rtl_legal_unimplemented: %s",
                      entry.decoded.command.id,
                      entry.decoded.maturityReason);
            }
        }
        for (size_t index = 0; index < fixtureCommands.size(); ++index) {
            const auto &command =
                fixtureCommands[index].decoded.command;
            if ((command.control.saFlowMode & 0x2) == 0) {
                continue;
            }
            panic_if(index + 1 == fixtureCommands.size(),
                     "SAU retain command %d has no following command to "
                     "consume its PE accumulator state",
                     command.id);
            panic_if(command.instructionLoops != 1,
                     "SAU strict retain runtime currently requires one "
                     "instruction tile per command");
        }
    }
    panic_if(functionalMemorySize == 0 &&
                 (!memoryImageFile.empty() || !finalMemoryDumpFile.empty()),
             "SAU memory image and final dump require a declared "
             "functional memory range");
    panic_if(params.functional_memory_fill > 0xff,
             "SAU functional memory fill must be one byte");
    if (functionalMemorySize != 0) {
        // Timing-memory runs hand the data authority to the zero-reset
        // downstream memory at startup; a nonzero hole fill could not be
        // reproduced there.
        panic_if(!calibrationMemory && functionalMemoryFill != 0,
                 "SAU timing-memory runs require a zero hole fill value");
        try {
            functionalMemory.emplace(functionalMemoryBase,
                                     functionalMemorySize,
                                     functionalMemoryFill);
            if (!memoryImageFile.empty()) {
                memoryImageBytes =
                    functionalMemory->loadLittleEndianWordHexFile(
                        memoryImageFile, memoryImageBase,
                        memoryImageWordBytes);
            }
        } catch (const std::invalid_argument &error) {
            fatal("%s", error.what());
        }
        if (!finalMemoryDumpFile.empty()) {
            panic_if(finalMemoryDumpSize == 0,
                     "SAU final memory dump range must be nonzero");
            panic_if(!functionalMemory->contains(finalMemoryDumpBase,
                                                 finalMemoryDumpSize),
                     "SAU final memory dump range is outside the "
                     "functional memory range");
        }
    }
    if (!params.timing_ledger_file.empty() && !timingLedger.is_open()) {
        throw std::runtime_error("failed to open SAU timing ledger: " +
                                 params.timing_ledger_file);
    }
    if (timingLedger.is_open()) {
        timingLedger << "command_id,term,cycles,source\n";
    }
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
    preloadTimingMemoryImage();
    // startup() 发生在 gem5 重置统计项之前。把首条命令排到第一个
    // 时钟边沿，使 commandsAccepted 与后续命令使用同一个统计窗口。
    schedule(startupEvent, nextCycle());
}

void
SauModel::preloadTimingMemoryImage()
{
    // Strict/calibration runs keep the local FunctionalMemory as their
    // only data authority; nothing is preloaded downstream.
    if (!functionalMemory || calibrationMemory) {
        return;
    }
    if (memoryImageBytes != 0) {
        const std::vector<uint8_t> image =
            functionalMemory->dumpRange(memoryImageBase, memoryImageBytes);
        memoryPort.writeFunctional(memoryImageBase, image.data(),
                                   image.size());
    }
    // From here on the downstream memory is the run's only data
    // authority; releasing the staging copy enforces that there is no
    // second runtime mirror.
    functionalMemory.reset();
}

void
SauModel::commitStrictWrite(const Beat &writeBeat,
                            const WriteBeat256 &writePayload)
{
    if (!functionalMemory) {
        return;
    }
    panic_if(!functionalMemory->contains(writeBeat.address, beatBytes),
             "SAU strict write outside the functional memory range: %#x",
             writeBeat.address);
    // The accepted local edge is the strict commit boundary. Keep the
    // external-memory byte view distinct from the output datapath type even
    // though both carry the same 32 address-ordered bytes.
    MemoryBeat256 memoryPayload;
    memoryPayload.bytes = writePayload.bytes;
    functionalMemory->writeBeat(writeBeat.address, memoryPayload);
    ++stats.payloadWriteBeats;
}

void
SauModel::flushPayloadStats()
{
    if (!payloadDatapath) {
        return;
    }
    stats.transposerInputRows += payloadDatapath->transposerInputRows();
    stats.transposerOutputColumns +=
        payloadDatapath->transposerOutputColumns();
    stats.transposerInputStalls +=
        payloadDatapath->transposerInputStalls();
    stats.transposerOutputStalls +=
        payloadDatapath->transposerOutputStalls();
    stats.transposerPayloadUnderflows +=
        payloadDatapath->payloadUnderflows();
    stats.transposerBusyCycles += payloadDatapath->transposerBusyCycles();
    stats.transposerMaxBankOccupancy = std::max(
        static_cast<unsigned>(stats.transposerMaxBankOccupancy.value()),
        payloadDatapath->transposerMaxOccupancy());
    const auto firstRow = payloadDatapath->firstRowEdge();
    const auto firstColumn = payloadDatapath->firstColumnEdge();
    if (firstRow && firstColumn) {
        stats.transposerFirstInToFirstOut[activeCommandIndex] =
            *firstColumn - *firstRow;
    }
}

void
SauModel::dumpFinalMemory()
{
    if (finalMemoryDumpFile.empty()) {
        return;
    }
    std::vector<uint8_t> bytes(finalMemoryDumpSize, 0);
    if (functionalMemory) {
        functionalMemory->read(finalMemoryDumpBase, bytes.data(),
                               bytes.size());
    } else {
        // Timing-memory authority: commandLocallyComplete() has already
        // required zero outstanding writes, so this functional readback
        // observes every committed write.
        memoryPort.readFunctional(finalMemoryDumpBase, bytes.data(),
                                  bytes.size());
    }
    try {
        FunctionalMemory::writeByteHexFile(finalMemoryDumpFile, bytes);
    } catch (const std::invalid_argument &error) {
        fatal("%s", error.what());
    }
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
    // ArrayPipeline tokens account for capacity/II within one command. A
    // reduced-output command can complete before every shadow token reaches
    // its nominal ready cycle, so clear the previous command's epoch before
    // configuring the next command's derived fill timing.
    arrayPipeline.resetForCommand();
    // In strict mode the RTL command driver owns admission and result timing.
    // Keep this pipeline only as bounded shadow-token accounting; a one-cycle
    // fill/II prevents the retired aggregate timing policy from gating RTL
    // pulses. Non-strict DSE retains its configured timing behavior.
    arrayPipeline.configure(
        strictTiming ? Cycles(1) : activeArrayFillCycles(),
        strictTiming ? Cycles(1) : arrayIiCycles);
    activeCommand = command;
    scheduleState.beginCommand();
    lastTracedScheduleState.reset();
    lastTracedInputSwitch.clear();
    traceScheduleState = SauScheduleState::ResidentLoad;
    transposeCompleteCycle.reset();
    inputSwitchVisibleCycle.reset();
    inputSwitchResetVisibleCycle.reset();
    traceInstructionTransitionCycle.reset();
    traceInstructionIndex = 0;
    projectedInputSwitch = "00";
    readGenerator.emplace(command);
    aRegisterFile.emplace(command, activeArrayInputABeats());
    if (strictTiming) {
        panic_if(!timingPolicy(),
                 "strict SAU command lacks an active timing policy");
        // releaseA/releaseB and ResultScheduler::produce() consume driver
        // pulses directly. Keep only their structural extents in strict mode;
        // the neutral timing fields must not become a second scheduler.
        arrayInputScheduler.emplace(
            activeArrayInputABeats(), activeArrayInputBBeats(),
            0, 1, 1, Cycles(0), Cycles(0), Cycles(0), Cycles(0));
        resultScheduler.emplace(
            expectedOutputBeats(), outputBeatsPerInstruction(),
            Cycles(0), Cycles(0));
        rtlCommandDriver.emplace(buildRtlCommandDriverConfig(
            command, *timingPolicy(), rtlTiming));
        rtlCommandDriverStarted = false;
        rtlDriverEdge = 0;
        // PLAN3 Step 3 runtime integration: with a functional memory
        // authority the strict driver edges also move real payloads
        // through the input/transposer resources.
        if (functionalMemory) {
            payloadDatapath.emplace(
                deriveResourceConfigs(command.control), retainedArrayState);
            retainedArrayState.reset();
            if (!boundaryTraceFile.empty() && activeCommandIndex == 0) {
                boundaryTrace.emplace(boundaryTraceFile);
                boundaryTrace->emitZero("sau_sram_rdata", 0);
                boundaryTrace->emitZero("core_register_data_out", 0);
                boundaryTrace->emitZero(
                    "u_trans2sa_top.trans0_outCol", 0);
                boundaryTrace->emitZero(
                    "u_trans2sa_top.trans1_outCol", 0);
            }
        } else {
            payloadDatapath.reset();
        }
    } else {
        arrayInputScheduler.emplace(
            activeArrayInputABeats(), activeArrayInputBBeats(),
            activeArrayInputSkewCycles(),
            activeArrayInputBurstBeats(), arrayInputsPerInstruction(),
            activeArrayInputBurstGapCycles(),
            activeArrayInputFlowGapCycles(1),
            activeArrayInputFlowGapCycles(2),
            activeArrayInputFlowGapCycles(3));
        resultScheduler.emplace(
            expectedOutputBeats(), outputBeatsPerInstruction(),
            activeArrayFillCycles(),
            activeResultFlowGapCycles());
        rtlCommandDriver.reset();
        rtlCommandDriverStarted = false;
    }
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
    emitTimingLedger(command);
    emitScheduleState("command_accepted");

    // The feeder delay is applied by issueReads() in command-local cycles.
    // Keep the clock active so that delay itself is represented in the trace.
    schedule(tickEvent, clockEdge(Cycles(1)));
}

// ==================== 核心调度：每个时钟边沿执行 ====================

void
SauModel::tick()
{
    externalRequestsIssuedThisCycle = 0;
    bool acceptedCommandThisTick = false;
    if (!activeCommand && nextCommandStartCycle &&
        Cycles(sauCycle) >= *nextCommandStartCycle) {
        nextCommandStartCycle.reset();
        submitNextCommand();
        acceptedCommandThisTick = true;
    }

    if (acceptedCommandThisTick) {
        advanceRtlCommandDriver();
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

    advanceRtlCommandDriver();
    if (memoryBlocked()) {
        // A rejected packet stalls the shared port until retry delivery;
        // every such cycle is a memory-retry stall for attribution.
        noteStall(StallCause::MemoryRetry);
    }
    issueReads();
    advanceScheduleProjection();

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
        if (payloadDatapath && functionalMemory) {
            // The mem_ctrl-visible edge carries the real 256-bit
            // payload from the strict data authority.
            panic_if(rtlDriverEdge < 2,
                     "SAU payload became visible before the RTL SRAM delay");
            const uint64_t memVisibleEdge = rtlDriverEdge - 1;
            MemoryBeat256 payload;
            functionalMemory->read(beat.address, payload.bytes.data(),
                                   payload.bytes.size());
            payloadDatapath->onMemoryDataVisible(
                memVisibleEdge, payload,
                beat.stream == StreamKind::OperandB);
            ++stats.payloadReadBeats;
            if (boundaryTrace && activeCommandIndex == 0) {
                // rdata surfaces one edge before the mem_ctrl-visible
                // core_register_data_out tap (SRAM_DELAY vs
                // SRAM_DELAY + 1).
                boundaryTrace->emit("sau_sram_rdata",
                                    memVisibleEdge - 1, payload);
                boundaryTrace->emit("core_register_data_out",
                                    memVisibleEdge, payload);
            }
        }
        if (beat.stream == StreamKind::OperandA) {
            if (!scheduleState.canLoadResident()) {
                panic("SAU schedule rejected an Operand-A resident load");
            }
            aRegisterFile->load(beat);
            if (anyResidentAReady() && !scheduleState.onResidentReady()) {
                panic("SAU schedule rejected a resident-ready transition");
            }
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
    if (rtlCommandDriver) {
        const bool bWanted = rtlCommandDriver->dataBValid();
        const bool aWanted = rtlCommandDriver->dataAValid();
        const bool bProgressed = bWanted ? advanceArrayB(true) : false;
        const bool aProgressed =
            aWanted ? advanceArrayA(bProgressed, true) : false;
        panic_if(bWanted && !bProgressed,
                 "strict SAU could not release an RTL operand-B token");
        panic_if(aWanted && !aProgressed,
                 "strict SAU could not release an RTL operand-A token");
        return;
    }
    if (commandCycle() < activeArrayInputStartDelayCycles()) {
        return;
    }

    const bool bWanted = scheduleState.canAdmitArrayB() &&
        arrayInputScheduler->canIssueB();
    bool bProgressed = false;
    bool progressed = false;

    // RTL trace order is B before A when both streams enter the array in the
    // same cycle.  Keep that ordering while allowing A to run ahead by the
    // configured skew window.
    if (bWanted) {
        bProgressed = advanceArrayB();
        progressed = bProgressed || progressed;
    }

    const bool aWanted = scheduleState.canAdmitArrayA() &&
        arrayInputScheduler->canIssueA();
    if (aWanted) {
        progressed = advanceArrayA(bProgressed) || progressed;
    }

    if (!progressed && (aWanted || bWanted)) {
        if (arrayPipeline.full()) {
            ++stats.stallArrayCapacity;
            noteStall(StallCause::ArrayBackpressure);
        } else if (bWanted && availableB.empty()) {
            ++stats.stallInputStarvation;
            // A missing operand while its read is in flight is response
            // latency/starvation; otherwise the input side itself has
            // not produced the token yet.
            noteStall(outstandingReads() > 0 ?
                      StallCause::ResponseStarvation :
                      StallCause::InputBackpressure);
        } else if (aWanted && !instructionReadyForArrayIndex(
                       arrayInputScheduler->issuedA())) {
            ++stats.stallInputStarvation;
            noteStall(outstandingReads() > 0 ?
                      StallCause::ResponseStarvation :
                      StallCause::InputBackpressure);
        }
    }

    arrayInputScheduler->advanceCycle();
}

bool
SauModel::advanceArrayB(bool rtlRelease)
{
    assert(activeCommand);
    assert(arrayInputScheduler);

    if (!rtlRelease && !arrayInputScheduler->canIssueB()) {
        return false;
    }
    if (!scheduleState.canAdmitArrayB()) {
        return false;
    }
    if (availableB.empty()) {
        return false;
    }
    if (!arrayPipeline.canAccept(commandCycle())) {
        return false;
    }
    const auto bBeat = availableB.front();
    const uint32_t arrayIndex = rtlRelease ?
        arrayInputScheduler->releaseB() :
        arrayInputScheduler->issueB();
    availableB.pop_front();
    const bool lastWork = arrayIndex + 1 == activeCommand->workItems;
    const bool flowBoundary = !lastWork &&
        (arrayIndex + 1) % arrayInputsPerInstruction() == 0;
    if (!scheduleState.onArrayBAdmitted(flowBoundary, lastWork)) {
        panic("SAU schedule rejected an Operand-B array admission");
    }
    emitScheduleState(lastWork ? "final_b_work_accepted" :
                      flowBoundary ? "flow_boundary" :
                      "array_b_admitted");

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
SauModel::advanceArrayA(bool allowPipelineBypass, bool rtlRelease)
{
    assert(activeCommand);
    assert(arrayInputScheduler);

    if (!rtlRelease && !arrayInputScheduler->canIssueA()) {
        return false;
    }
    if (!scheduleState.canAdmitArrayA()) {
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
    const uint32_t issuedIndex = rtlRelease ?
        arrayInputScheduler->releaseA() :
        arrayInputScheduler->issueA();
    assert(issuedIndex == arrayIndex);
    if (!scheduleState.onArrayAAdmitted()) {
        panic("SAU schedule rejected an Operand-A array admission");
    }
    emitScheduleState("array_a_admitted");

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

    if (!activeCommand || !resultScheduler || resultScheduler->complete()) {
        return;
    }
    if (!scheduleState.canReleaseResult()) {
        panic_if(rtlCommandDriver && rtlCommandDriver->resultValid(),
                 "strict SAU schedule rejected an RTL result token");
        return;
    }
    if (rtlCommandDriver) {
        if (!rtlCommandDriver->resultValid()) {
            return;
        }
    } else {
        if (!resultFlowReady()) {
            return;
        }
        resultScheduler->deferUntil(commandCycle());
        if (!resultScheduler->canProduce(commandCycle())) {
            return;
        }
    }
    if (!outputBuffer.canPush()) {
        panic_if(rtlCommandDriver,
                 "strict SAU output buffer blocked an RTL result token");
        ++stats.stallOutputBufferFull;
        noteStall(StallCause::OutputBackpressure);
        return;
    }

    const uint32_t resultIndex = rtlCommandDriver ?
        resultScheduler->produce() :
        resultScheduler->produce(commandCycle());
    const bool last = resultIndex + 1 == expectedOutputBeats();
    outputBuffer.push({activeCommand->id, resultIndex, commandCycle(), last});
    lastResultCycle = commandCycle();
    if (last && !rtlCommandDriver) {
        if (!inputSwitchResetVisibleCycle) {
            inputSwitchResetVisibleCycle = commandCycle() +
                activeInputSwitchResetVisibleDelayCycles();
        }
    }
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
    if (!activeCommand || memoryBlocked()) {
        return;
    }
    if (!scheduleState.canIssueWriteback()) {
        panic_if(rtlCommandDriver &&
                     rtlCommandDriver->memoryWriteRequestValid(),
                 "strict SAU schedule rejected an RTL write request");
        return;
    }
    if (outputBuffer.size() == 0) {
        panic_if(rtlCommandDriver &&
                     rtlCommandDriver->memoryWriteRequestValid(),
                 "strict SAU lacks a result token for an RTL write request");
        return;
    }
    if (rtlCommandDriver) {
        if (!rtlCommandDriver->memoryWriteRequestValid()) {
            return;
        }
    } else {
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
                                      activeWritebackStartDelayCycles())) {
            return;
        }
    }
    if (!calibrationMemory && outstandingWrites() >= maxOutstandingWrites) {
        ++stats.stallOutstandingWriteLimit;
        noteStall(StallCause::OutstandingLimit);
        return;
    }
    if (strictTiming &&
        externalRequestsIssuedThisCycle >= activeStorageIssueWidth()) {
        return;
    }

    unsigned issued = 0;
    const unsigned issueLimit = strictTiming ? activeStorageIssueWidth() :
        (calibrationMemory ? 1 : writeIssueWidth);
    while (issued < issueLimit && outputBuffer.size() > 0 &&
           (calibrationMemory || outstandingWrites() < maxOutstandingWrites) &&
           memoryCanIssue()) {
        const auto &token = outputBuffer.front();
        const uint32_t beatIndex = token.index;
        const Addr scheduledAddress =
            activeCommand->output.base +
            static_cast<Addr>(beatIndex) *
                activeCommand->output.strideBytes;
        const OutputRegisterUnload *writePayload = nullptr;
        if (rtlCommandDriver && payloadDatapath) {
            panic_if(!payloadDatapath->hasWritePayload(),
                     "strict SAU lacks an output payload for an RTL write "
                     "request");
            writePayload = &payloadDatapath->nextWritePayload();
            panic_if(writePayload->logicalAddress != beatIndex,
                     "strict SAU output payload index differs from the "
                     "result token");
            panic_if(writePayload->address != scheduledAddress,
                     "strict SAU output payload address differs from the "
                     "command write address");
        }
        Beat writeBeat{
            StreamKind::Output,
            writePayload ? writePayload->address : scheduledAddress,
            beatIndex,
            writePayload ? writePayload->last :
                beatIndex + 1 == expectedOutputBeats(),
        };
        panic_if(
            rtlCommandDriver &&
                writeBeat.last !=
                    rtlCommandDriver->memoryWriteRequestLast(),
            "strict SAU write-last differs from RTL command driver");

        if (phase != Phase::Writeback) {
            transitionTo(Phase::Writeback);
        }

        const bool acceptedOrBlocked = calibrationMemory ?
            true : memoryPort.trySend(
                writeBeat, true,
                writePayload ? writePayload->data.bytes.data() : nullptr);
        if (calibrationMemory) {
            requestAccepted(writeBeat, true);
            panic_if(rtlCommandDriver && functionalMemory && !writePayload,
                     "strict SAU functional write lacks output payload");
            if (writePayload) {
                commitStrictWrite(writeBeat, writePayload->data);
            } else if (functionalMemory) {
                // Preserve the legacy timing-only/DSE zero-data contract.
                commitStrictWrite(writeBeat, WriteBeat256{});
            }
        }
        if (writePayload) {
            // trySend() has copied the payload into its Packet even when the
            // timing request is blocked, so ownership can now leave the
            // datapath queue.
            payloadDatapath->takeWritePayload();
        }
        outputBuffer.pop();
        ++nextWriteIndex;
        ++issued;
        if (!acceptedOrBlocked) {
            ++stats.stallRequestRetry;
            noteStall(StallCause::MemoryRetry);
            break;
        }
    }
}

void
SauModel::issueReads()
{
    if (!activeCommand || !readGenerator || memoryBlocked()) {
        return;
    }
    if (readGenerator->empty()) {
        panic_if(rtlCommandDriver &&
                     rtlCommandDriver->memoryReadRequestValid(),
                 "strict SAU lacks an address for an RTL read request");
        return;
    }
    if (rtlCommandDriver) {
        if (!rtlCommandDriver->memoryReadRequestValid()) {
            return;
        }
    } else {
        // The feeder starts after command acceptance. This is distinct from
        // physical event scheduling: it defines the command-local trace
        // boundary between command acceptance and the first external read.
        if (commandCycle() < activeCommandStartCycles()) {
            return;
        }
    }
    if (!calibrationMemory && outstandingReads() >= maxOutstandingReads) {
        ++stats.stallOutstandingReadLimit;
        noteStall(StallCause::OutstandingLimit);
        return;
    }

    unsigned issued = 0;
    const unsigned issueLimit = strictTiming ? activeStorageIssueWidth() :
        (calibrationMemory ? 1 : readIssueWidth);
    while (issued < issueLimit && !readGenerator->empty() &&
           (calibrationMemory || outstandingReads() < maxOutstandingReads) &&
           memoryCanIssue()) {
        const Beat beat = readGenerator->front();
        if (!scheduleState.canIssueRead(beat.stream)) {
            panic_if(rtlCommandDriver,
                     "strict SAU schedule rejected an RTL read request");
            break;
        }
        panic_if(
            rtlCommandDriver &&
                (beat.stream == StreamKind::OperandB) !=
                    rtlCommandDriver->memoryReadRequestIsStream(),
            "strict SAU read stream differs from RTL command driver");
        if (!rtlCommandDriver && beat.stream == StreamKind::OperandB &&
            bReadInCooldown()) {
            break;
        }
        if (!rtlCommandDriver && !canIssueReadBeat(beat)) {
            break;
        }
        const bool acceptedOrBlocked = calibrationMemory ?
            true : memoryPort.trySend(beat, false);
        if (calibrationMemory) {
            const Cycles acceptedCycle = commandCycle();
            const Cycles visibleCycle =
                acceptedCycle + activeReadVisibleLatencyCycles();
            panic_if(strictTiming && !calibrationReadResponses.empty() &&
                         visibleCycle <
                             calibrationReadResponses.back().visibleCycle,
                     "strict SAU SRAM responses must remain ordered");
            calibrationReadResponses.push_back({
                acceptedCycle, visibleCycle, beat});
            requestAccepted(beat, false);
            if (!rtlCommandDriver &&
                beat.stream == StreamKind::OperandB) {
                applyBReadCooldown();
            }
        }
        readGenerator->pop();
        ++issued;
        if (!acceptedOrBlocked) {
            ++stats.stallRequestRetry;
            noteStall(StallCause::MemoryRetry);
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

    if (readAcceptedTraceB % arrayInputsPerInstruction() == 0) {
        bReadCooldownCycles = activeArrayInputFlowGapCycles(
            readAcceptedTraceB / arrayInputsPerInstruction());
    } else if (readAcceptedTraceB % activeArrayInputBurstBeats() == 0) {
        bReadCooldownCycles = activeArrayInputBurstGapCycles();
    }
}

std::vector<Beat>
SauModel::takeVisibleReadResponses()
{
    if (!calibrationMemory) {
        // Timing-only scheduling has no functional-datapath consumer for
        // the returned payload yet (PLAN3 Step 3+); only the beat
        // metadata advances the schedule.
        std::vector<Beat> responses;
        for (const auto &response : memoryPort.takeVisibleResponses()) {
            responses.push_back(response.beat);
        }
        return responses;
    }

    std::vector<Beat> responses;
    if (!calibrationReadResponses.empty() &&
        calibrationReadResponses.front().visibleCycle <= commandCycle()) {
        const auto &response = calibrationReadResponses.front();
        panic_if(strictTiming && response.visibleCycle != commandCycle(),
                 "strict SAU SRAM response missed its fixed visible cycle");
        panic_if(strictTiming &&
                     response.visibleCycle !=
                         response.acceptedCycle +
                             activeReadVisibleLatencyCycles(),
                 "strict SAU SRAM response violates accepted-to-visible "
                 "latency");
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
    if (beat.stream != StreamKind::OperandB) {
        return true;
    }

    if (!strictTiming) {
        // A timing-memory run can backpressure future reads. Reserve slots for
        // both returned tokens and accepted responses which are still in
        // flight, otherwise a delayed response burst can overflow the FIFO.
        assert(readAcceptedTraceB >= readResponseTraceB);
        const uint64_t pendingBResponses =
            readAcceptedTraceB - readResponseTraceB;
        const uint64_t reservedBSlots =
            availableB.size() + pendingBResponses;
        if (reservedBSlots >= activeBStagingBeats()) {
            return false;
        }
    }

    if (activeBReadStartAheadBeats() == 0) {
        return true;
    }

    if (!activeCommand || !arrayInputScheduler) {
        return false;
    }

    const uint32_t aPreloadBeats =
        activeCommand->operandA.beats * activeCommand->instructionLoops;
    return readResponseTraceA >= aPreloadBeats &&
        arrayInputScheduler->issuedA() >= activeBReadStartAheadBeats();
}

Cycles
SauModel::activeReadVisibleLatencyCycles() const
{
    return timingPolicy() ? timingPolicy()->storage.readVisibleLatencyCycles :
                            calibrationReadLatencyCycles;
}

unsigned
SauModel::activeStorageIssueWidth() const
{
    return timingPolicy() ? timingPolicy()->storage.issueWidth :
                            rtlStorageTiming.issueWidth;
}

unsigned
SauModel::activeBStagingBeats() const
{
    return strictTiming && timingPolicy() ? timingPolicy()->bStagingBeats :
                                           inputBufferEntries;
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
        if (rtlCommandDriver) {
            panic_if(
                rtlCommandDriver->residentReadTokens() !=
                    timingPolicy()->residentLoadBeats ||
                rtlCommandDriver->streamReadTokens() !=
                    activeCommand->workItems ||
                rtlCommandDriver->acceptedSaTokens() !=
                    activeCommand->workItems ||
                rtlCommandDriver->resultTokens() !=
                    expectedOutputBeats() ||
                rtlCommandDriver->physicalWriteTokens() !=
                    expectedOutputBeats(),
                "strict SAU command-driver token conservation failed");
            panic_if(
                payloadDatapath &&
                    (payloadDatapath->serializerInputStalls() != 0 ||
                     payloadDatapath->serializerOutputUnderflows() != 0 ||
                     payloadDatapath->outputRegisterUpdates() !=
                         expectedOutputBeats() ||
                     payloadDatapath->outputUnloadBeats() !=
                         expectedOutputBeats() ||
                     payloadDatapath->hasWritePayload()),
                "strict SAU functional output-resource conservation failed");
            emitRtlStageLedger(*activeCommand);
        }
        flushPayloadStats();
        traceScheduleState = SauScheduleState::Complete;
        scheduleState.completeCommand();
        emitScheduleState("command_complete");
        transitionTo(Phase::Complete);
        traceWriter.emit(sauCycle, EventKind::CommandComplete,
                         activeCommand->id, "none", 0, 0, phase);
        if (boundaryTrace) {
            boundaryTrace->flush();
        }
        stats.completeOffset[activeCommandIndex] =
            static_cast<uint64_t>(commandCycle());
        ++stats.commandsCompleted;
        const bool moreCommands = nextCommandIndex < commandCount;
        const bool retainArray =
            (activeCommand->control.saFlowMode & 0x2) != 0;
        if (payloadDatapath && retainArray) {
            retainedArrayState =
                payloadDatapath->finishRetainedArrayState();
        } else if (!retainArray) {
            retainedArrayState.reset();
        }
        activeCommand.reset();
        readGenerator.reset();
        aRegisterFile.reset();
        arrayInputScheduler.reset();
        resultScheduler.reset();
        rtlCommandDriver.reset();
        rtlCommandDriverStarted = false;
        payloadDatapath.reset();
        availableB.clear();
        if (moreCommands) {
            if (strictTiming) {
                const auto &firstFixtureCommand = fixtureCommands.front();
                const auto &nextFixtureCommand =
                    fixtureCommands.at(nextCommandIndex);
                const Cycles nextStart = Cycles(
                    nextFixtureCommand.startCycle -
                    firstFixtureCommand.startCycle);
                panic_if(nextStart <= Cycles(sauCycle),
                         "SAU CSR fixture overlaps an active command");
                nextCommandStartCycle = nextStart;
            } else {
                nextCommandStartCycle =
                    Cycles(sauCycle) + interCommandGapCycles;
            }
        } else if (exitOnDone) {
            exitSimLoop("SAU command complete");
        }
        if (!moreCommands) {
            dumpFinalMemory();
            signalDrainDone();
        }
    }
}

void
SauModel::accountCycle()
{
    const unsigned causes = rawStallCauses;
    rawStallCauses = 0;
    if (!activeCommand) {
        return;
    }

    if (causes != 0) {
        // Attribute the stalled cycle once, to the highest-priority raw
        // cause in the frozen Step 0 order (lowest enumerator wins).
        for (unsigned cause = 0;
             cause < static_cast<unsigned>(StallCause::NumCauses);
             ++cause) {
            if (causes & (1u << cause)) {
                ++stats.primaryStallCycles[cause];
                break;
            }
        }
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
    assert(!strictTiming ||
           externalRequestsIssuedThisCycle <= activeStorageIssueWidth());
    assert(availableB.size() <= activeBStagingBeats());
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
               arrayInputScheduler->totalAInputs());
        assert(arrayInputScheduler->issuedB() <=
               arrayInputScheduler->totalBInputs());
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

const TimingPolicy *
SauModel::timingPolicy() const
{
    return activeTimingPolicy ? &*activeTimingPolicy : nullptr;
}

Cycles
SauModel::activeArrayFillCycles() const
{
    return timingPolicy() ? timingPolicy()->arrayFillCycles : arrayFillCycles;
}

Cycles
SauModel::activeInputSwitchVisibleDelayCycles() const
{
    return timingPolicy() ? timingPolicy()->inputSwitchVisibleDelayCycles :
                            Cycles(0);
}

Cycles
SauModel::activeInputSwitchResetVisibleDelayCycles() const
{
    return timingPolicy() ?
        timingPolicy()->inputSwitchResetVisibleDelayCycles : Cycles(0);
}

Cycles
SauModel::activeTraceFlowExecuteCycles() const
{
    if (timingPolicy() && timingPolicy()->shortDirectDOutPath) {
        return timingPolicy()->shortExecuteCycles(traceInstructionIndex);
    }
    if (timingPolicy()) {
        return timingPolicy()->flowExecuteCycles;
    }
    return Cycles(arrayInputsPerInstruction() - activeBReadStartAheadBeats());
}

Cycles
SauModel::activeTraceShortDrainCycles() const
{
    panic_if(!timingPolicy() || !timingPolicy()->shortDirectDOutPath,
             "short D_OUT duration requested for a non-short RTL path");
    return timingPolicy()->shortDrainCycles(traceInstructionIndex);
}

Cycles
SauModel::activeTraceFlowBoundaryCycles() const
{
    // TRANSPOSE_CLIP holds one SA_SIZE row before scheduler.sv enters D_OUT;
    // include the state-register edge that commits the new state.
    return Cycles(static_cast<uint64_t>(rtlTiming.saSize) +
                  rtlTiming.schedulerStateRegisterDelay);
}

Cycles
SauModel::activeArrayInputStartDelayCycles() const
{
    return timingPolicy() ? timingPolicy()->arrayInputStartDelayCycles :
                            arrayInputStartDelayCycles;
}

unsigned
SauModel::activeArrayInputBurstBeats() const
{
    return timingPolicy() ? timingPolicy()->arrayInputBurstBeats :
                            arrayInputBurstBeats;
}

Cycles
SauModel::activeArrayInputBurstGapCycles() const
{
    return timingPolicy() ? timingPolicy()->arrayInputBurstGapCycles :
                            arrayInputBurstGapCycles;
}

Cycles
SauModel::activeArrayInputFlowGapCycles(uint32_t completedFlows) const
{
    return timingPolicy() ?
        timingPolicy()->arrayInputFlowGapAfter(completedFlows) :
                            arrayInputFlowGapCycles;
}

uint32_t
SauModel::activeArrayInputABeats() const
{
    panic_if(!activeCommand, "array-input A extent requires an active command");
    return timingPolicy() ? timingPolicy()->arrayInputABeats :
                            activeCommand->workItems;
}

uint32_t
SauModel::activeArrayInputBBeats() const
{
    panic_if(!activeCommand, "array-input B extent requires an active command");
    return timingPolicy() ? timingPolicy()->arrayInputBBeats :
                            activeCommand->workItems;
}

unsigned
SauModel::activeArrayInputSkewCycles() const
{
    return timingPolicy() ? timingPolicy()->arrayInputSkewCycles :
                            arrayInputSkewCycles;
}

unsigned
SauModel::activeBReadStartAheadBeats() const
{
    return timingPolicy() ? timingPolicy()->bReadStartAheadBeats :
                            bReadStartAheadBeats;
}

Cycles
SauModel::activeResultFlowGapCycles() const
{
    return timingPolicy() ? timingPolicy()->resultFlowGapCycles :
                            resultFlowGapCycles;
}

Cycles
SauModel::activeWritebackStartDelayCycles() const
{
    if (!timingPolicy()) {
        return writebackStartDelayCycles;
    }
    // register_file_out can overlap its REGISTER_UNLOAD activation with the
    // tail of result production.  Once the propagated input switch is already
    // clear, that activation edge is no longer on the write critical path.
    return projectedInputSwitch == "00" ?
        timingPolicy()->earlyUnloadWritebackStartDelayCycles :
        timingPolicy()->writebackStartDelayCycles;
}

Cycles
SauModel::activeCompletionDelayCycles() const
{
    return timingPolicy() ? timingPolicy()->completionDelayCycles :
                            completionDelayCycles;
}

Cycles
SauModel::activeCommandStartCycles() const
{
    return timingPolicy() ? timingPolicy()->commandStartCycles :
                            commandStartCycles;
}

void
SauModel::emitTimingLedger(const SauCommand &command)
{
    if (!timingLedger.is_open() || !timingPolicy()) {
        return;
    }
    for (const auto &entry : timingPolicy()->ledger) {
        timingLedger << command.id << ',' << entry.term << ','
                     << static_cast<uint64_t>(entry.cycles) << ','
                     << entry.source << '\n';
    }
    timingLedger.flush();
}

void
SauModel::emitRtlStageLedger(const SauCommand &command)
{
    if (!timingLedger.is_open() || !rtlCommandDriver) {
        return;
    }

    const auto emitWindow =
        [this, &command](std::string_view term,
                         const RtlStageWindow &window,
                         std::string_view source) {
            panic_if(!window.observed,
                     "strict SAU stage ledger missed an observed window");
            timingLedger << command.id << ",actual_" << term
                         << "_first_edge," << window.firstEdge << ','
                         << source << '\n';
            timingLedger << command.id << ",actual_" << term
                         << "_last_edge," << window.lastEdge << ','
                         << source << '\n';
            timingLedger << command.id << ",actual_" << term
                         << "_span," << window.span() << ','
                         << source << '\n';
        };

    emitWindow("resident_read", rtlCommandDriver->residentReadWindow(),
               "RtlCommandDriver shared SRAM request");
    emitWindow("stream_read", rtlCommandDriver->streamReadWindow(),
               "RtlCommandDriver shared SRAM request");
    emitWindow("operand_a", rtlCommandDriver->operandAWindow(),
               "RtlCommandDriver feeder data_A_valid");
    emitWindow("operand_b", rtlCommandDriver->operandBWindow(),
               "RtlCommandDriver feeder data_B_valid");
    if (expectedOutputBeats() != 0) {
        emitWindow("result", rtlCommandDriver->resultWindow(),
                   "RtlCommandDriver result_final_valid");
        emitWindow("memory_write", rtlCommandDriver->memoryWriteWindow(),
                   "RtlCommandDriver mem_ctrl write request");
    }
    panic_if(!rtlCommandDriver->commandDoneObserved(),
             "strict SAU stage ledger missed command_done");
    timingLedger << command.id << ",actual_command_done_edge,"
                 << rtlCommandDriver->commandDoneEdge()
                 << ",RtlCommandDriver scheduler command_done\n";
    timingLedger.flush();
}

void
SauModel::advanceRtlCommandDriver()
{
    if (!rtlCommandDriver) {
        return;
    }
    rtlCommandDriver->tick(!rtlCommandDriverStarted);
    rtlCommandDriverStarted = true;
    ++rtlDriverEdge;
    if (!payloadDatapath) {
        return;
    }
    payloadDatapath->beginCycle();
    // The registered driver pulses of this edge move the payload side.
    if (rtlCommandDriver->registerFileReadValid()) {
        payloadDatapath->onRegisterFileReadValid(rtlDriverEdge);
    }
    if (rtlCommandDriver->dataAValid()) {
        payloadDatapath->onOperandAValid(rtlDriverEdge);
    }
    if (rtlCommandDriver->dataBValid()) {
        payloadDatapath->onOperandBValid(rtlDriverEdge);
    }
    if (rtlCommandDriver->saEnable()) {
        payloadDatapath->onSaEnable(rtlDriverEdge);
    }
    if (rtlCommandDriver->arrayRowScoreValid() &&
        !payloadDatapath->arrayOutputRequested()) {
        payloadDatapath->requestArrayOutput();
    }
    payloadDatapath->sampleCycle(rtlDriverEdge);
    if (rtlCommandDriver->resultValid()) {
        payloadDatapath->onResultValid(rtlDriverEdge);
    }
    payloadDatapath->onRegisterUnload(
        rtlDriverEdge,
        rtlCommandDriver->coreState() == RtlCoreState::RegisterUnload);
    if (!boundaryTrace || activeCommandIndex != 0) {
        return;
    }

    const auto &events = payloadDatapath->boundaryEvents();
    const auto boundaryCycle = [](uint64_t driverEdge) {
        panic_if(driverEdge == 0,
                 "payload boundary event precedes the command anchor");
        return driverEdge - 1;
    };
    if (events.operandA) {
        boundaryTrace->emit(
            "data_A", boundaryCycle(events.operandA->edge),
            events.operandA->data);
    }
    if (events.operandB) {
        boundaryTrace->emit(
            "data_B", boundaryCycle(events.operandB->edge),
            events.operandB->data);
    }
    const auto emitTransposer =
        [this, &boundaryCycle](const char *suffix,
               const TransposerBoundaryTransfer &transfer) {
            const std::string signal =
                "u_trans2sa_top.trans" + std::to_string(transfer.bank) +
                suffix;
            boundaryTrace->emit(
                signal, boundaryCycle(transfer.edge), transfer.data);
        };
    if (events.transposerInput) {
        emitTransposer("_inRow", *events.transposerInput);
    }
    if (events.transposerOutput) {
        emitTransposer("_outCol", *events.transposerOutput);
    }
    if (events.transposerPrefetch) {
        emitTransposer("_outCol", *events.transposerPrefetch);
    }
    if (events.arrayInput) {
        boundaryTrace->emit(
            "u_trans2sa_top.sa_data_active_left",
            boundaryCycle(events.arrayInput->edge),
            beatFromOperand(events.arrayInput->activations));
        boundaryTrace->emit(
            "u_trans2sa_top.sa_in_weight_above",
            boundaryCycle(events.arrayInput->edge),
            beatFromOperand(events.arrayInput->weights));
    }
    if (events.arrayOutput) {
        OutputVector32x16 row;
        for (unsigned lane = 0; lane < BeatLanes; ++lane) {
            row.lanes[lane] = events.arrayOutput->data.lanes[lane];
        }
        boundaryTrace->emit(
            "u_trans2sa_top.out_sum_final_q",
            boundaryCycle(events.arrayOutput->edge), row);
    }
}

void
SauModel::advanceRtlScheduleProjection()
{
    assert(rtlCommandDriver);
    const auto coreState = rtlCommandDriver->coreState();
    if (coreState == RtlCoreState::Idle) {
        return;
    }

    const auto nextState = projectedScheduleState(coreState);
    const auto nextInputSwitch = projectedInputSwitchName(
        rtlCommandDriver->outputInputSwitch());
    const bool switchOnly =
        nextState == traceScheduleState &&
        nextInputSwitch != projectedInputSwitch;
    traceScheduleState = nextState;
    projectedInputSwitch = nextInputSwitch;
    emitScheduleState(switchOnly ? "input_switch_visible" :
                      projectedTransitionCause(coreState));
}

void
SauModel::advanceScheduleProjection()
{
    if (!activeCommand) {
        return;
    }
    if (rtlCommandDriver) {
        advanceRtlScheduleProjection();
        return;
    }

    if (transposeCompleteCycle &&
        commandCycle() >= *transposeCompleteCycle &&
        scheduleState.state() == SauScheduleState::TransposeSetup) {
        if (!scheduleState.onTransposeComplete()) {
            panic("SAU schedule rejected the RTL transpose-complete guard");
        }
        inputSwitchVisibleCycle = commandCycle() +
            activeInputSwitchVisibleDelayCycles();
        traceScheduleState = SauScheduleState::FlowExecute;
        scheduleTraceInstructionTransition(true);
        emitScheduleState("transpose_complete");
    }

    if (traceInstructionTransitionCycle &&
        commandCycle() >= *traceInstructionTransitionCycle) {
        const uint32_t totalInstructions = scheduleInstructionCount();
        switch (traceScheduleState) {
          case SauScheduleState::FlowExecute:
            if (timingPolicy() && timingPolicy()->shortDirectDOutPath) {
                traceScheduleState = SauScheduleState::DrainAndWriteback;
                if (traceInstructionIndex + 1 == totalInstructions) {
                    traceInstructionTransitionCycle.reset();
                    if (timingPolicy()->earlyFinalUnload) {
                        inputSwitchResetVisibleCycle = commandCycle() +
                            timingPolicy()->
                                finalDrainToInputSwitchResetCycles;
                    }
                } else {
                    traceInstructionTransitionCycle = commandCycle() +
                        activeTraceShortDrainCycles();
                }
                emitScheduleState("drain_results");
                break;
            }
            if (traceInstructionIndex + 1 == totalInstructions) {
                traceScheduleState = SauScheduleState::DrainAndWriteback;
                traceInstructionTransitionCycle.reset();
                if (timingPolicy() && timingPolicy()->earlyFinalUnload) {
                    inputSwitchResetVisibleCycle = commandCycle() +
                        timingPolicy()->finalDrainToInputSwitchResetCycles;
                }
                emitScheduleState("final_flow");
            } else {
                traceScheduleState = SauScheduleState::FlowBoundary;
                traceInstructionTransitionCycle = commandCycle() +
                    activeTraceFlowBoundaryCycles();
                emitScheduleState("flow_boundary");
            }
            break;
          case SauScheduleState::FlowBoundary:
            traceScheduleState = SauScheduleState::DrainAndWriteback;
            traceInstructionTransitionCycle = commandCycle() + Cycles(1);
            emitScheduleState("drain_results");
            break;
          case SauScheduleState::DrainAndWriteback:
            ++traceInstructionIndex;
            traceScheduleState = SauScheduleState::FlowExecute;
            scheduleTraceInstructionTransition(false);
            emitScheduleState("transpose_complete");
            break;
          default:
            break;
        }
    }

    if (inputSwitchVisibleCycle &&
        commandCycle() >= *inputSwitchVisibleCycle &&
        projectedInputSwitch != "01") {
        projectedInputSwitch = "01";
        emitScheduleState("input_switch_visible");
    }

    if (inputSwitchResetVisibleCycle &&
        commandCycle() >= *inputSwitchResetVisibleCycle &&
        projectedInputSwitch != "00") {
        projectedInputSwitch = "00";
        inputSwitchVisibleCycle.reset();
        emitScheduleState("input_switch_visible");
    }
}

void
SauModel::scheduleTraceInstructionTransition(bool firstInstruction)
{
    Cycles executeCycles = activeTraceFlowExecuteCycles();
    if (firstInstruction &&
        !(timingPolicy() && timingPolicy()->shortDirectDOutPath)) {
        // The TRANSPOSE_LOAD -> REUSE_LOAD edge is also the first sampled
        // REUSE_LOAD frame, so its first data_last guard is one edge earlier.
        executeCycles = executeCycles -
            Cycles(rtlTiming.schedulerStateRegisterDelay);
    }
    traceInstructionTransitionCycle = commandCycle() + executeCycles;
}

std::string
SauModel::scheduleInputSwitch() const
{
    switch (traceScheduleState) {
      case SauScheduleState::Idle:
      case SauScheduleState::ResidentLoad:
      case SauScheduleState::TransposeSetup:
      case SauScheduleState::Complete:
        return "00";
      case SauScheduleState::FlowExecute:
      case SauScheduleState::FlowBoundary:
      case SauScheduleState::DrainAndWriteback:
        return projectedInputSwitch;
    }
    panic("unknown SAU schedule state while tracing input switch");
}

void
SauModel::emitScheduleState(std::string_view cause)
{
    if (!activeCommand || !stateTraceWriter.enabled()) {
        return;
    }
    const auto state = traceScheduleState;
    const auto inputSwitch = scheduleInputSwitch();
    if (lastTracedScheduleState && *lastTracedScheduleState == state &&
        lastTracedInputSwitch == inputSwitch) {
        return;
    }
    stateTraceWriter.emit(sauCycle, activeCommand->id, state, inputSwitch,
                          cause);
    lastTracedScheduleState = state;
    lastTracedInputSwitch = inputSwitch;
}

bool
SauModel::commandLocallyComplete() const
{
    const uint32_t outputBeats = expectedOutputBeats();
    return activeCommand &&
        (!rtlCommandDriver || rtlCommandDriver->commandDone()) &&
        writesAccepted == outputBeats &&
        outputBuffer.size() == 0 &&
        (!payloadDatapath || !payloadDatapath->hasWritePayload()) &&
        // ArrayPipeline occupancy is a capacity/II accounting mechanism.
        // ResultScheduler and accepted writeback define architectural
        // completion; waiting for every shadow admission token to age by the
        // full fill latency double-counts the RTL result pipeline when output
        // extent is smaller than input work (the N-sweep case).
        arrayInputScheduler && arrayInputScheduler->complete() &&
        resultScheduler && resultScheduler->complete() &&
        (outputBeats == 0 ||
         (lastWriteCycle &&
          (rtlCommandDriver ||
           static_cast<uint64_t>(commandCycle()) >=
               static_cast<uint64_t>(*lastWriteCycle +
                                     activeCompletionDelayCycles())))) &&
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
    if (fixtureReplay) {
        return fixtureCommands.at(index).decoded.command;
    }
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
    if (fixtureReplay) {
        activeTimingPolicy =
            fixtureCommands.at(nextCommandIndex).decoded.timingPolicy;
    } else {
        activeTimingPolicy.reset();
    }
    submitCommand(buildCommandForIndex(nextCommandIndex++));
}

uint32_t
SauModel::expectedOutputBeats() const
{
    if (!activeCommand) {
        return 0;
    }
    if ((activeCommand->control.saFlowMode & 0x2) != 0) {
        return 0;
    }
    return activeCommand->output.beats * activeCommand->instructionLoops;
}

uint32_t
SauModel::scheduleInstructionCount() const
{
    assert(activeCommand);
    return effectiveScheduleInstructions(*activeCommand);
}

uint32_t
SauModel::outputBeatsPerInstruction() const
{
    assert(activeCommand);
    const uint32_t totalInstructions = scheduleInstructionCount();
    const uint32_t totalOutputs =
        activeCommand->output.beats * activeCommand->instructionLoops;
    if (totalOutputs % totalInstructions != 0) {
        throw std::invalid_argument(
            "SAU output beats must divide evenly across schedule instructions");
    }
    return totalOutputs / totalInstructions;
}

uint32_t
SauModel::arrayInputsPerInstruction() const
{
    assert(activeCommand);
    const uint32_t totalInstructions = scheduleInstructionCount();
    if (activeCommand->workItems % totalInstructions != 0) {
        throw std::invalid_argument(
            "SAU work items must divide evenly across schedule instructions");
    }
    return activeCommand->workItems / totalInstructions;
}

bool
SauModel::resultFlowReady() const
{
    if (!activeCommand || !resultScheduler || resultScheduler->complete()) {
        return false;
    }

    const uint64_t flow =
        resultScheduler->produced() / outputBeatsPerInstruction();
    const uint64_t requiredInputs =
        (flow + 1) * arrayInputsPerInstruction();
    return arrayAdmissions >= requiredInputs;
}

bool
SauModel::anyResidentAReady() const
{
    if (!activeCommand || !aRegisterFile) {
        return false;
    }

    for (uint32_t instruction = 0;
         instruction < activeCommand->instructionLoops; ++instruction) {
        if (aRegisterFile->instructionReady(instruction)) {
            return true;
        }
    }
    return false;
}

bool
SauModel::instructionReadyForArrayIndex(uint32_t index) const
{
    if (!activeCommand || !aRegisterFile) {
        return false;
    }

    const uint32_t perInstruction =
        activeCommand->workItems / activeCommand->instructionLoops;
    const uint32_t instruction =
        (index / perInstruction) % activeCommand->instructionLoops;
    return instruction < activeCommand->instructionLoops &&
        aRegisterFile->instructionReady(instruction);
}

Beat
SauModel::makeArrayABeat(uint32_t index) const
{
    assert(activeCommand);
    assert(aRegisterFile);

    const uint32_t perInstruction =
        activeCommand->workItems / activeCommand->instructionLoops;
    const uint32_t instruction =
        (index / perInstruction) % activeCommand->instructionLoops;
    const uint32_t withinInstruction = index % perInstruction;
    const uint32_t flow =
        (withinInstruction / activeCommand->operandA.beats) %
        activeCommand->flowLoops;
    const uint32_t beat = withinInstruction % activeCommand->operandA.beats;

    return aRegisterFile->arrayInputBeat(instruction, flow, beat, index);
}

void
SauModel::requestAccepted(const Beat &beat, bool write)
{
    panic_if(!activeCommand,
             "SAU memory request accepted without an active command");
    if (strictTiming) {
        ++externalRequestsIssuedThisCycle;
        panic_if(externalRequestsIssuedThisCycle > activeStorageIssueWidth(),
                 "strict SAU issued more than one shared SRAM request in a "
                 "cycle");
        panic_if(write && readGenerator && !readGenerator->empty(),
                 "strict SAU write bypassed a pending higher-priority read");
        const uint32_t expectedAReads =
            activeCommand->operandA.beats * activeCommand->instructionLoops;
        panic_if(!write && beat.stream == StreamKind::OperandB &&
                     readAcceptedTraceA < expectedAReads,
                 "strict SAU issued Operand-B before all Operand-A preload "
                 "requests");
    }

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
        if (beat.stream == StreamKind::OperandA &&
            !scheduleState.onAReadAccepted(beat.last)) {
            panic("SAU schedule rejected an Operand-A read acceptance");
        }
        if (beat.stream == StreamKind::OperandA && beat.last) {
            if (!rtlCommandDriver) {
                // The legacy/non-strict projection derives TRANSPOSE_LOAD
                // from the final resident request. Strict state is sampled
                // directly from the per-tick RTL command driver.
                transposeCompleteCycle = commandCycle() +
                    Cycles(activeArrayInputBurstBeats());
                traceScheduleState = SauScheduleState::TransposeSetup;
                emitScheduleState("register_load_done");
            }
        }
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
      ADD_STAT(primaryStallCycles, statistics::units::Cycle::get(),
               "Stalled cycles attributed once to the frozen "
               "primary-cause priority"),
      ADD_STAT(payloadReadBeats, statistics::units::Count::get(),
               "Strict read beats consumed with real payloads"),
      ADD_STAT(payloadWriteBeats, statistics::units::Count::get(),
               "Strict write beats committed with real payloads"),
      ADD_STAT(transposerInputRows, statistics::units::Count::get(),
               "Operand rows accepted into the transposer banks"),
      ADD_STAT(transposerOutputColumns, statistics::units::Count::get(),
               "Operand-bank columns consumed at SA-enable edges"),
      ADD_STAT(transposerInputStalls, statistics::units::Count::get(),
               "Operand rows arriving with no bank able to take input"),
      ADD_STAT(transposerOutputStalls, statistics::units::Count::get(),
               "SA-enable edges with no drainable bank column"),
      ADD_STAT(transposerPayloadUnderflows,
               statistics::units::Count::get(),
               "Operand edges whose payload queue was empty"),
      ADD_STAT(transposerBusyCycles, statistics::units::Cycle::get(),
               "Cycles with at least one occupied transposer bank"),
      ADD_STAT(transposerMaxBankOccupancy,
               statistics::units::Count::get(),
               "Maximum rows held across the T0/T1 banks"),
      ADD_STAT(transposerFirstInToFirstOut,
               statistics::units::Cycle::get(),
               "First bank row to first column out, indexed by command"),
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
    primaryStallCycles.init(6);
    primaryStallCycles.subname(0, "memory_retry");
    primaryStallCycles.subname(1, "response_starvation");
    primaryStallCycles.subname(2, "outstanding_limit");
    primaryStallCycles.subname(3, "input_backpressure");
    primaryStallCycles.subname(4, "array_backpressure");
    primaryStallCycles.subname(5, "output_backpressure");
    transposerFirstInToFirstOut.init(commandCount);
    firstReadOffset.init(commandCount);
    firstArrayInputOffset.init(commandCount);
    firstResultOffset.init(commandCount);
    lastResultOffset.init(commandCount);
    lastWriteOffset.init(commandCount);
    completeOffset.init(commandCount);
}

} // namespace gem5::sau
