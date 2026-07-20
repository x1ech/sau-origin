#include "sau/array_input_scheduler.hh"

#include <cassert>

namespace gem5::sau
{

ArrayInputScheduler::ArrayInputScheduler(uint32_t totalInputs, uint32_t bSkew)
    : ArrayInputScheduler(
          totalInputs, bSkew, totalInputs == 0 ? 1 : totalInputs,
          totalInputs == 0 ? 1 : totalInputs, Cycles(0), Cycles(0))
{
}

ArrayInputScheduler::ArrayInputScheduler(
    uint32_t totalInputs, uint32_t bSkew, uint32_t burstBeats,
    uint32_t flowBeats, Cycles burstGap, Cycles flowGap)
    : ArrayInputScheduler(
          totalInputs, totalInputs, bSkew, burstBeats, flowBeats, burstGap,
          flowGap, flowGap, flowGap)
{
}

ArrayInputScheduler::ArrayInputScheduler(
    uint32_t totalAInputs, uint32_t totalBInputs, uint32_t bSkew,
    uint32_t burstBeats, uint32_t flowBeats, Cycles burstGap,
    Cycles firstFlowGap, Cycles secondFlowGap, Cycles steadyFlowGap)
    : totalA(totalAInputs),
      totalB(totalBInputs),
      skew(bSkew),
      burst(burstBeats),
      flow(flowBeats),
      burstGapCycles(burstGap),
      firstFlowGapCycles(firstFlowGap),
      secondFlowGapCycles(secondFlowGap),
      steadyFlowGapCycles(steadyFlowGap)
{
    assert(burst != 0);
    assert(flow != 0);
}

bool
ArrayInputScheduler::canIssueA() const
{
    if (cooldownCycles != Cycles(0)) {
        return false;
    }
    const uint64_t bWindow =
        static_cast<uint64_t>(nextB) + static_cast<uint64_t>(skew);
    return nextA < totalA && static_cast<uint64_t>(nextA) < bWindow;
}

bool
ArrayInputScheduler::canIssueB() const
{
    if (cooldownCycles != Cycles(0)) {
        return false;
    }
    const uint64_t bWindow =
        static_cast<uint64_t>(nextB) + static_cast<uint64_t>(skew);
    return nextB < totalB &&
        (nextA == totalA || static_cast<uint64_t>(nextA) >= bWindow);
}

uint32_t
ArrayInputScheduler::issueA()
{
    assert(canIssueA());
    return nextA++;
}

uint32_t
ArrayInputScheduler::issueB()
{
    assert(canIssueB());
    const uint32_t issued = nextB++;
    if (nextB < totalB && nextB % burst == 0) {
        pendingCooldownCycles = nextB % flow == 0 ?
            flowGapAfter(nextB / flow) : burstGapCycles;
    }
    return issued;
}

void
ArrayInputScheduler::advanceCycle()
{
    if (pendingCooldownCycles != Cycles(0)) {
        cooldownCycles = pendingCooldownCycles;
        pendingCooldownCycles = Cycles(0);
        return;
    }

    if (cooldownCycles != Cycles(0)) {
        cooldownCycles = Cycles(static_cast<uint64_t>(cooldownCycles) - 1);
    }
}

uint32_t
ArrayInputScheduler::issuedA() const
{
    return nextA;
}

uint32_t
ArrayInputScheduler::issuedB() const
{
    return nextB;
}

uint32_t
ArrayInputScheduler::totalInputs() const
{
    return totalB;
}

uint32_t
ArrayInputScheduler::totalAInputs() const
{
    return totalA;
}

uint32_t
ArrayInputScheduler::totalBInputs() const
{
    return totalB;
}

bool
ArrayInputScheduler::complete() const
{
    return nextA == totalA && nextB == totalB;
}

Cycles
ArrayInputScheduler::flowGapAfter(uint32_t completedFlows) const
{
    if (completedFlows <= 1) {
        return firstFlowGapCycles;
    }
    if (completedFlows == 2) {
        return secondFlowGapCycles;
    }
    return steadyFlowGapCycles;
}

} // namespace gem5::sau
