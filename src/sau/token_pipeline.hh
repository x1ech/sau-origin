#ifndef __SAU_TOKEN_PIPELINE_HH__
#define __SAU_TOKEN_PIPELINE_HH__

#include <cstddef>
#include <cstdint>
#include <deque>

#include "sau/types.hh"

namespace gem5::sau
{

class TokenBuffer
{
  public:
    explicit TokenBuffer(size_t capacity);

    bool canPush() const;
    void push(PipelineToken token);
    const PipelineToken &front() const;
    void pop();
    size_t size() const;

  private:
    const size_t capacity;
    std::deque<PipelineToken> tokens;
};

class ArrayPipeline
{
  public:
    ArrayPipeline(Cycles fillLatency, Cycles initiationInterval,
                  size_t maxInFlight);

    bool canAccept(Cycles now) const;
    void accept(uint64_t commandId, uint32_t index, bool last, Cycles now);
    bool hasReady(Cycles now) const;
    PipelineToken takeReady(Cycles now);
    size_t inFlight() const;

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
