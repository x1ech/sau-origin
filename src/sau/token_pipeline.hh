#ifndef __SAU_TOKEN_PIPELINE_HH__
#define __SAU_TOKEN_PIPELINE_HH__

#include <cstddef>
#include <cstdint>
#include <deque>

#include "sau/types.hh"

namespace gem5::sau
{

// 有界 FIFO，模拟阵列输入端缓冲。
// 满了反压上游，实现容量限流。
class TokenBuffer
{
  public:
    explicit TokenBuffer(size_t capacity);

    bool canPush() const;              // 还有空位吗
    void push(PipelineToken token);    // 入队
    const PipelineToken &front() const;// 看队首（不取走）
    void pop();                        // 取走队首
    size_t size() const;               // 当前排队数

  private:
    const size_t capacity;
    std::deque<PipelineToken> tokens;
};

// 收缩阵列流水时序建模。
// 不存计算数据，只推 token 并延迟 readyCycle。
// fillLatency：第一个结果等待拍数
// initiationInterval(II)：相邻两个结果的最小间隔
// maxInFlight：阵列内最多同时驻留 token 数
class ArrayPipeline
{
  public:
    ArrayPipeline(Cycles fillLatency, Cycles initiationInterval,
                  size_t maxInFlight);

    // Start a command-local timing epoch after the previous command drains.
    void reset();
    bool canAccept(Cycles now) const;                                  // mit-in-flight 未满 && II 已过
    void accept(uint64_t commandId, uint32_t index, bool last, Cycles now);// 入队，记录 readyCycle = now + fillLatency
    bool canAcceptAdditional() const;                                  // 同拍附加输入，不消耗 II
    void acceptAdditional(uint64_t commandId, uint32_t index, bool last,
                          Cycles now);
    bool full() const;
    bool hasReady(Cycles now) const;                                   // 队首 token 是否已到就绪时间
    PipelineToken takeReady(Cycles now);                               // 取出就绪 token
    size_t inFlight() const;                                           // 阵列内剩余未出 token 数

  private:
    const Cycles fillLatency;
    const Cycles initiationInterval;
    const size_t maxInFlight;
    std::deque<PipelineToken> tokens;
    bool hasAccepted = false;
    Cycles nextAcceptCycle = Cycles(0);
};

} // namespace gem5::sau

#endif // __SAU_TOKEN_PIPELINE_HH__
