#ifndef __SAU_SAU_MODEL_HH__
#define __SAU_SAU_MODEL_HH__

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "base/statistics.hh"
#include "params/SauModel.hh"
#include "sau/a_register_file.hh"
#include "sau/address_generator.hh"
#include "sau/array_input_scheduler.hh"
#include "sau/memory_port.hh"
#include "sau/result_scheduler.hh"
#include "sau/token_pipeline.hh"
#include "sau/trace_writer.hh"
#include "sau/types.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"

namespace gem5
{

class System;

namespace sau
{

// SAU 周期级行为模型的顶层 SimObject。
// 继承 ClockedObject 拥有独立时钟域，tick() 每拍推进流水。
// 下属组件：MemoryPort（内存通信）、TraceWriter（事件记录）、统计组、
// AddressGenerator / TokenBuffer / ArrayPipeline（待集成）。
class SauModel : public ClockedObject, private SauMemoryPortOwner
{
  private:
    // ========== 硬件参数（从 Sau.py 注入，构造后不可变） ==========
    System *const system;             // 所属 System 对象（用于分配 requestor ID）
    const RequestorID requestorId;    // 全局唯一请求者 ID
    SauMemoryPort memoryPort;         // 唯一内存端口

    const unsigned beatBytes;         // 单 beat 字节数
    const unsigned readIssueWidth;    // 每拍最多发送多少个读请求
    const unsigned writeIssueWidth;   // 每拍最多发送多少个写请求
    const unsigned maxOutstandingReads;  // 最多未完成读请求数
    const unsigned maxOutstandingWrites; // 最多未完成写请求数
    const unsigned inputBufferEntries;   // TokenBuffer 容量
    const unsigned outputBufferEntries;  // OutputBuffer 容量
    const Cycles arrayFillCycles;     // 阵列填充延迟（fill latency）
    const Cycles arrayIiCycles;       // 阵列启动间隔（initiation interval）
    const unsigned arrayCapacity;     // 阵列最大 in-flight token 数
    const Cycles arrayInputStartDelayCycles;
    const unsigned arrayInputBurstBeats;
    const Cycles arrayInputBurstGapCycles;
    const Cycles arrayInputFlowGapCycles;
    const unsigned arrayInputSkewCycles; // B 相对 resident A 的输入滞后
    const unsigned bReadStartAheadBeats;
    const Cycles resultFlowGapCycles;
    const Cycles writebackStartDelayCycles;
    const Cycles completionDelayCycles;
    const Cycles commandStartCycles;  // 命令接收后延迟多少拍开始发第一笔读
    const bool calibrationMemory;
    const Cycles calibrationReadLatencyCycles;
    const unsigned commandCount;
    const Cycles interCommandGapCycles;
    const Addr aCommandStride;
    const Addr bCommandStride;
    const Addr outputCommandStride;
    const bool exitOnDone;            // 命令完成后是否自动退出仿真

    // ========== 运行时状态 ==========
    const SauCommand startupCommand;          // 启动时自动注入的 synthetic 命令
    std::optional<SauCommand> activeCommand;  // 当前正在执行的命令（空 = idle）
    // 当前命令所处的 SAU 执行阶段
    Phase phase = Phase::Idle;
    uint64_t sauCycle = 0;                    // SAU 全局 trace 周期
    uint64_t commandStartCycle = 0;           // 当前命令的起始 trace 周期
    uint32_t nextCommandIndex = 0;
    std::optional<Cycles> nextCommandStartCycle;
    struct ScheduledReadResponse
    {
        Cycles visibleCycle;
        Beat beat;
    };
    std::deque<ScheduledReadResponse> calibrationReadResponses;
    std::vector<Beat> visibleMemoryResponses;
    std::optional<AddressGenerator> readGenerator;
    std::optional<ARegisterFileIn> aRegisterFile;
    std::optional<ArrayInputScheduler> arrayInputScheduler;
    std::optional<ResultScheduler> resultScheduler;
    std::deque<Beat> availableB;
    TokenBuffer outputBuffer;
    ArrayPipeline arrayPipeline;

    uint32_t nextResultIndex = 0;
    uint32_t nextWriteIndex = 0;

    uint64_t acceptedReadBeats = 0;
    uint64_t visibleReadBeats = 0;
    uint64_t arrayAdmissions = 0;
    uint64_t resultsProduced = 0;
    uint64_t writesAccepted = 0;
    Cycles bReadCooldownCycles = Cycles(0);
    uint32_t readAcceptedTraceA = 0;
    uint32_t readAcceptedTraceB = 0;
    uint32_t readResponseTraceA = 0;
    uint32_t readResponseTraceB = 0;
    std::optional<Cycles> lastResultCycle;
    std::optional<Cycles> lastWriteCycle;

    TraceWriter traceWriter;                  // CSV 事件日志输出
    EventFunctionWrapper tickEvent;           // gem5 事件：每个时钟边沿触发 tick()

    // ========== 核心调度（每个时钟边沿执行一次） ==========
    void tick();
    void consumeResponses();
    void advanceArray();
    bool advanceArrayB();
    bool advanceArrayA(bool allowPipelineBypass);
    void produceResults();
    void issueWrites();
    void issueReads();
    bool bReadInCooldown();
    void applyBReadCooldown();
    std::vector<Beat> takeVisibleReadResponses();
    bool memoryBlocked() const;
    bool memoryCanIssue() const;
    unsigned outstandingReads() const;
    unsigned outstandingWrites() const;
    bool canIssueReadBeat(const Beat &beat) const;
    void updatePhase();
    void accountCycle();
    void transitionTo(Phase newPhase);
    void checkConservation() const;
    bool hasPendingWork() const;
    bool commandLocallyComplete() const;
    Cycles commandCycle() const;
    SauCommand buildCommandForIndex(uint32_t index) const;
    void submitNextCommand();
    uint32_t expectedOutputBeats() const;
    uint32_t outputBeatsPerFlow() const;
    uint32_t arrayInputsPerFlow() const;
    bool resultFlowReady() const;
    bool instructionReadyForArrayIndex(uint32_t index) const;
    Beat makeArrayABeat(uint32_t index) const;
    void requestAccepted(const Beat &beat, bool write) override;
    void responseAvailable() override;

    // ========== 统计组 ==========
    struct SauStats : public statistics::Group
    {
        explicit SauStats(statistics::Group *parent);

        statistics::Scalar commandsAccepted;       // 已接受的命令数
        statistics::Scalar commandsCompleted;      // 已完成的命令数
        statistics::Scalar commandCycles;          // 活跃命令总周期
        statistics::Scalar operandLoadCycles;      // 操作数加载阶段的周期
        statistics::Scalar arrayActiveCycles;      // 阵列活跃阶段的周期
        statistics::Scalar arrayDrainCycles;       // 阵列排空阶段的周期
        statistics::Scalar writebackCycles;        // 写回阶段的周期
        statistics::Scalar readRequests;           // 已接受的读请求数
        statistics::Scalar readBytes;              // 读取字节总数
        statistics::Scalar writeRequests;          // 已接受的写请求数
        statistics::Scalar writeBytes;             // 写入字节总数
        statistics::Scalar maxOutstandingReadCount; // 最大未完成读请求数
        statistics::Scalar maxOutstandingWriteCount;
        statistics::Scalar maxInputBufferOccupancy; // 输入 buffer 最大占用
        statistics::Scalar maxOutputBufferOccupancy;
        statistics::Scalar stallRequestRetry;      // 因 port retry 等待的周期
        statistics::Scalar stallOutstandingReadLimit;
        statistics::Scalar stallOutstandingWriteLimit;
        statistics::Scalar stallInputStarvation;   // 因缺少输入 token 的等待周期
        statistics::Scalar stallOutputBufferFull;  // 因输出 buffer 满的等待周期
        statistics::Scalar stallWritebackBlocked;  // 因写回被反压的等待周期
        // arrayActiveCycles / commandCycles
        statistics::Formula arrayUtilization;
    } stats;

  public:
    PARAMS(SauModel);

    explicit SauModel(const Params &params);

    // ========== gem5 SimObject 标准接口 ==========
    Port &getPort(const std::string &ifName,
                  PortID idx = InvalidPortID) override; // 返回 memoryPort
    void startup() override; // 仿真启动时注入 synthetic 命令
    DrainState drain() override; // 暂停仿真（用于 checkpoint）
    void drainResume() override; // 恢复仿真

    void submitCommand(const SauCommand &command); // 外部注入命令的入口
};

} // namespace sau
} // namespace gem5

#endif // __SAU_SAU_MODEL_HH__
