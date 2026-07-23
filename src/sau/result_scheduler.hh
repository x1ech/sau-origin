#ifndef __SAU_RESULT_SCHEDULER_HH__
#define __SAU_RESULT_SCHEDULER_HH__

#include <cstdint>
#include <optional>

#include "sau/types.hh"

namespace gem5::sau
{

class ResultScheduler
{
  public:
    ResultScheduler(uint32_t totalOutputs, uint32_t outputsPerFlow,
                    Cycles fillLatency, Cycles flowGap);

    void start(Cycles firstArrayCycle);
    bool started() const;
    // Delay the current and all later result slots until an input dependency
    // becomes available, preserving result spacing and flow gaps.
    void deferUntil(Cycles now);
    bool canProduce(Cycles now) const;
    uint32_t produce();
    uint32_t produce(Cycles now);
    uint32_t produced() const;
    uint32_t total() const;
    bool complete() const;

  private:
    Cycles scheduledCycle(uint32_t outputIndex) const;

    const uint32_t totalOutputs;
    const uint32_t perFlow;
    const Cycles fillLatency;
    const Cycles flowGapCycles;
    std::optional<Cycles> firstArrayInputCycle;
    uint32_t nextOutput = 0;
};

} // namespace gem5::sau

#endif // __SAU_RESULT_SCHEDULER_HH__
