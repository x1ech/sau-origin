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
    : total(totalInputs),
      skew(bSkew),
      burst(burstBeats),
      flow(flowBeats),
      burstGapCycles(burstGap),
      flowGapCycles(flowGap)
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
    return nextA < total && static_cast<uint64_t>(nextA) < bWindow;
}

bool
ArrayInputScheduler::canIssueB() const
{
    if (cooldownCycles != Cycles(0)) {
        return false;
    }
    const uint64_t bWindow =
        static_cast<uint64_t>(nextB) + static_cast<uint64_t>(skew);
    return nextB < total &&
        (nextA == total || static_cast<uint64_t>(nextA) >= bWindow);
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
    if (nextB < total && nextB % burst == 0) {
        pendingCooldownCycles =
            nextB % flow == 0 ? flowGapCycles : burstGapCycles;
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
    return total;
}

bool
ArrayInputScheduler::complete() const
{
    return nextA == total && nextB == total;
}

} // namespace gem5::sau
