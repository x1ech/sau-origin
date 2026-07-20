#ifndef __SAU_ARRAY_INPUT_SCHEDULER_HH__
#define __SAU_ARRAY_INPUT_SCHEDULER_HH__

#include <cstdint>

#include "sau/types.hh"

namespace gem5::sau
{

class ArrayInputScheduler
{
  public:
    ArrayInputScheduler(uint32_t totalInputs, uint32_t bSkew);
    ArrayInputScheduler(uint32_t totalInputs, uint32_t bSkew,
                        uint32_t burstBeats, uint32_t flowBeats,
                        Cycles burstGap, Cycles flowGap);
    ArrayInputScheduler(uint32_t totalAInputs, uint32_t totalBInputs,
                        uint32_t bSkew, uint32_t burstBeats,
                        uint32_t flowBeats, Cycles burstGap,
                        Cycles firstFlowGap, Cycles secondFlowGap,
                        Cycles steadyFlowGap);

    bool canIssueA() const;
    bool canIssueB() const;

    uint32_t issueA();
    uint32_t issueB();
    void advanceCycle();

    uint32_t issuedA() const;
    uint32_t issuedB() const;
    uint32_t totalInputs() const;
    uint32_t totalAInputs() const;
    uint32_t totalBInputs() const;
    bool complete() const;

  private:
    const uint32_t totalA;
    const uint32_t totalB;
    const uint32_t skew;
    const uint32_t burst;
    const uint32_t flow;
    const Cycles burstGapCycles;
    const Cycles firstFlowGapCycles;
    const Cycles secondFlowGapCycles;
    const Cycles steadyFlowGapCycles;
    uint32_t nextA = 0;
    uint32_t nextB = 0;
    Cycles cooldownCycles = Cycles(0);
    Cycles pendingCooldownCycles = Cycles(0);

    Cycles flowGapAfter(uint32_t completedFlows) const;
};

} // namespace gem5::sau

#endif // __SAU_ARRAY_INPUT_SCHEDULER_HH__
