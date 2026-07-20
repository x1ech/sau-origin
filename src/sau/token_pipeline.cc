#include "sau/token_pipeline.hh"

#include <cassert>
#include <utility>

namespace gem5::sau
{

TokenBuffer::TokenBuffer(size_t capacity)
    : capacity(capacity)
{
}

bool
TokenBuffer::canPush() const
{
    return tokens.size() < capacity;
}

void
TokenBuffer::push(PipelineToken token)
{
    assert(canPush());
    tokens.push_back(std::move(token));
}

const PipelineToken &
TokenBuffer::front() const
{
    assert(!tokens.empty());
    return tokens.front();
}

void
TokenBuffer::pop()
{
    assert(!tokens.empty());
    tokens.pop_front();
}

size_t
TokenBuffer::size() const
{
    return tokens.size();
}

ArrayPipeline::ArrayPipeline(
    Cycles fillLatency, Cycles initiationInterval, size_t maxInFlight)
    : fillLatency(fillLatency),
      initiationInterval(initiationInterval),
      maxInFlight(maxInFlight)
{
}

void
ArrayPipeline::configure(Cycles fillLatency, Cycles initiationInterval)
{
    assert(tokens.empty());
    assert(fillLatency != Cycles(0));
    assert(initiationInterval != Cycles(0));
    this->fillLatency = fillLatency;
    this->initiationInterval = initiationInterval;
}

void
ArrayPipeline::reset()
{
    assert(tokens.empty());
    hasAccepted = false;
    nextAcceptCycle = Cycles(0);
}

void
ArrayPipeline::resetForCommand()
{
    tokens.clear();
    hasAccepted = false;
    nextAcceptCycle = Cycles(0);
}

bool
ArrayPipeline::canAccept(Cycles now) const
{
    const bool intervalElapsed =
        !hasAccepted ||
        static_cast<uint64_t>(now) >=
            static_cast<uint64_t>(nextAcceptCycle);
    return tokens.size() < maxInFlight && intervalElapsed;
}

void
ArrayPipeline::accept(
    uint64_t commandId, uint32_t index, bool last, Cycles now)
{
    assert(canAccept(now));
    tokens.push_back({
        commandId,
        index,
        now + fillLatency,
        last,
    });
    hasAccepted = true;
    nextAcceptCycle = now + initiationInterval;
}

bool
ArrayPipeline::canAcceptAdditional() const
{
    return tokens.size() < maxInFlight;
}

void
ArrayPipeline::acceptAdditional(
    uint64_t commandId, uint32_t index, bool last, Cycles now)
{
    assert(canAcceptAdditional());
    tokens.push_back({
        commandId,
        index,
        now + fillLatency,
        last,
    });
}

bool
ArrayPipeline::full() const
{
    return tokens.size() >= maxInFlight;
}

bool
ArrayPipeline::hasReady(Cycles now) const
{
    return !tokens.empty() &&
        static_cast<uint64_t>(tokens.front().readyCycle) <=
            static_cast<uint64_t>(now);
}

PipelineToken
ArrayPipeline::takeReady(Cycles now)
{
    assert(hasReady(now));
    auto token = std::move(tokens.front());
    tokens.pop_front();
    return token;
}

size_t
ArrayPipeline::inFlight() const
{
    return tokens.size();
}

} // namespace gem5::sau
