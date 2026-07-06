#include "sau/sau_model.hh"

#include <algorithm>
#include <limits>
#include <stdexcept>

#include "base/logging.hh"
#include "sau/command.hh"
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
      commandStartCycles(params.command_start_cycles),
      exitOnDone(params.exit_on_done),
      startupCommand(buildStartupCommand(params)),  // 将 Python 参数转为 SauCommand
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
        traceWriter.emit(
            sauCycle, EventKind::ReadResponseVisible, activeCommand->id,
            streamName(beat.stream), 0, beat.index, phase);
    }

    // 没有活跃命令 → 空转返回
    if (!activeCommand) {
        return;
    }

    // 统计当前 phase 的累计周期
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

    // 【骨架阶段】tick() 未集成任何调度逻辑
    // 任务 8 完成后将替换为：
    //   consumeResponses() → advanceArray() → produceResults()
    //   → issueWrites() → issueReads() → updatePhase() → accountCycle()

    ++sauCycle;
    schedule(tickEvent, clockEdge(Cycles(1))); // 下一拍继续 tick
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

    traceWriter.emit(
        sauCycle,
        write ? EventKind::WriteAccepted : EventKind::ReadAccepted,
        activeCommand->id, streamName(beat.stream), beat.address,
        beat.index, phase);
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
    // 【骨架阶段】无条件停止 tick 并报告 Drained
    // 任务 8 完成后需检查 outstanding 请求是否全部完成
    if (tickEvent.scheduled()) {
        deschedule(tickEvent);
    }
    return DrainState::Drained;
}

void
SauModel::drainResume()
{
    // 恢复仿真：如果有活跃命令且 tick 未调度，则重新启动
    if (activeCommand && !tickEvent.scheduled()) {
        schedule(tickEvent, clockEdge(Cycles(1)));
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
