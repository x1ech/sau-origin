#include "sau/result_scheduler.hh"

#include <cassert>
#include <stdexcept>

namespace gem5::sau
{

ResultScheduler::ResultScheduler(
    uint32_t totalOutputs, uint32_t outputsPerFlow, Cycles fillLatency,
    Cycles flowGap)
    : totalOutputs(totalOutputs),
      perFlow(outputsPerFlow),
      fillLatency(fillLatency),
      flowGapCycles(flowGap)
{
    if (outputsPerFlow == 0) {
        throw std::invalid_argument(
            "SAU result schedule flow extent must be nonzero");
    }
}

void
ResultScheduler::start(Cycles firstArrayCycle)
{
    if (!firstArrayInputCycle) {
        firstArrayInputCycle = firstArrayCycle;
    }
}

bool
ResultScheduler::started() const
{
    return firstArrayInputCycle.has_value();
}

void
ResultScheduler::deferUntil(Cycles now)
{
    assert(started());
    if (complete()) {
        return;
    }

    const Cycles scheduled = scheduledCycle(nextOutput);
    if (now > scheduled) {
        *firstArrayInputCycle += now - scheduled;
    }
}

bool
ResultScheduler::canProduce(Cycles now) const
{
    return started() && nextOutput < totalOutputs &&
        static_cast<uint64_t>(now) >=
            static_cast<uint64_t>(scheduledCycle(nextOutput));
}

uint32_t
ResultScheduler::produce()
{
    assert(nextOutput < totalOutputs);
    return nextOutput++;
}

uint32_t
ResultScheduler::produce(Cycles now)
{
    assert(canProduce(now));
    return produce();
}

uint32_t
ResultScheduler::produced() const
{
    return nextOutput;
}

uint32_t
ResultScheduler::total() const
{
    return totalOutputs;
}

bool
ResultScheduler::complete() const
{
    return nextOutput == totalOutputs;
}

Cycles
ResultScheduler::scheduledCycle(uint32_t outputIndex) const
{
    assert(firstArrayInputCycle);
    const uint32_t flow = outputIndex / perFlow;
    const uint32_t indexInFlow = outputIndex % perFlow;
    const uint64_t offset =
        static_cast<uint64_t>(fillLatency) +
        static_cast<uint64_t>(flow) *
            (static_cast<uint64_t>(perFlow) +
             static_cast<uint64_t>(flowGapCycles)) +
        indexInFlow;
    return *firstArrayInputCycle + Cycles(offset);
}

} // namespace gem5::sau
