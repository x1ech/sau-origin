#ifndef __SAU_SAU_MODEL_HH__
#define __SAU_SAU_MODEL_HH__

#include <cstdint>
#include <optional>
#include <string>

#include "base/statistics.hh"
#include "mem/port.hh"
#include "params/SauModel.hh"
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
class SauModel : public ClockedObject
{
  private:
    // ========== 嵌套类：SAU 专用内存端口 ==========
    // 继承 RequestPort，实现 timing 模式下的读写请求发送/响应接收/retry 协议。
    class MemoryPort : public RequestPort
    {
      public:
        MemoryPort(const std::string &name, SauModel &owner);

      protected:
        bool recvTimingResp(PacketPtr packet) override; // 收到读响应或写完成
        void recvReqRetry() override;                    // 下游通知可以重发被拒请求

      private:
        SauModel &owner; // 回指所属 SauModel，回调时需访问其内部状态
    };

    // ========== 硬件参数（从 Sau.py 注入，构造后不可变） ==========
    MemoryPort memoryPort;            // 唯一内存端口
    System *const system;             // 所属 System 对象（用于分配 requestor ID）
    const RequestorID requestorId;    // 全局唯一请求者 ID

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
    const Cycles commandStartCycles;  // 命令接收后延迟多少拍开始发第一笔读
    const bool exitOnDone;            // 命令完成后是否自动退出仿真

    // ========== 运行时状态 ==========
    const SauCommand startupCommand;          // 启动时自动注入的 synthetic 命令
    std::optional<SauCommand> activeCommand;  // 当前正在执行的命令（空 = idle）
    // 当前命令所处的 SAU 执行阶段
    Phase phase = Phase::Idle;
    uint64_t sauCycle = 0;                    // SAU 内部周期计数（相对命令开始时刻）

    TraceWriter traceWriter;                  // CSV 事件日志输出
    EventFunctionWrapper tickEvent;           // gem5 事件：每个时钟边沿触发 tick()

    // ========== 核心调度（每个时钟边沿执行一次） ==========
    void tick();

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
