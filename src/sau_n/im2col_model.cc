#include "sau_n/im2col_model.hh"

#include <algorithm>
#include <stdexcept>

#include "sau_n/im2col_address.hh"

namespace gem5::sau_n
{
namespace
{

uint64_t
bitCount(uint16_t value)
{
    uint64_t count = 0;
    while (value != 0) {
        count += value & 1U;
        value >>= 1;
    }
    return count;
}

} // anonymous namespace

SramRequest
arbitrateBanks(
    const LaneRequests &laneRequests, const LaneBits &laneDone)
{
    SramRequest request;
    for (std::size_t lane = 0; lane < Im2ColLanes; ++lane) {
        const auto &candidate = laneRequests[lane];
        if (!candidate.valid || laneDone[lane]) {
            continue;
        }
        if (candidate.bank >= SpBanks || candidate.row >= SpBankEntries ||
            candidate.dstLane >= BlockSize || candidate.laneSel >= BlockSize) {
            throw std::out_of_range("lane request exceeds fixed hardware bounds");
        }
        if (!request.valid[candidate.bank]) {
            request.valid[candidate.bank] = true;
            request.address[candidate.bank] = candidate.row;
        }
    }
    return request;
}

PeriodicReady::PeriodicReady(uint64_t period_, uint64_t highCycles_)
    : period(period_), highCycles(highCycles_)
{
    if (period == 0) {
        throw std::invalid_argument("ready period must be at least one");
    }
    if (highCycles == 0 || highCycles > period) {
        throw std::invalid_argument(
            "ready high cycles must be in [1, ready period]");
    }
}

bool
PeriodicReady::at(uint64_t cycle) const
{
    return cycle % period < highCycles;
}

Im2ColModel::Im2ColModel(const ResolvedConfig &config)
    : resolved(config), dimensions(validateAndDerive(resolved))
{
    scratchpad.preload(resolved);
}

Im2ColModel::Im2ColModel(
    const ResolvedConfig &config,
    const BankedScratchpad &preloadedScratchpad)
    : resolved(config), dimensions(validateAndDerive(resolved)),
      scratchpad(preloadedScratchpad)
{
}

Im2ColModel::IssuedVector
Im2ColModel::buildIssuedVector(const Iterators &iterators) const
{
    IssuedVector issued;
    const ChwAddressMapper mapper(resolved);

    for (uint64_t lane = 0; lane < BlockSize; ++lane) {
        uint64_t localH = 0;
        uint64_t localW = lane;
        uint64_t outH = iterators.oh;
        uint64_t outW = iterators.owBase + lane;
        if (resolved.w <= BlockSize) {
            localH = lane / resolved.w;
            localW = lane % resolved.w;
            outH = iterators.oh + localH;
            outW = localW;
        }

        const bool shapeValid = resolved.w > BlockSize ||
            localH < dimensions.rowsPerWord;
        const bool outputValid = outH < resolved.outH &&
            outW < resolved.outW;
        if (!shapeValid || !outputValid || iterators.c >= resolved.c) {
            continue;
        }

        const uint64_t paddedH = checkedAdd(
            checkedMultiply(outH, resolved.strideH, "padded H"),
            checkedMultiply(
                iterators.kh, resolved.dilationH, "padded H"),
            "padded H");
        const uint64_t paddedW = checkedAdd(
            checkedMultiply(outW, resolved.strideW, "padded W"),
            checkedMultiply(
                iterators.kw, resolved.dilationW, "padded W"),
            "padded W");
        const bool padding = paddedH < resolved.padTop ||
            paddedW < resolved.padLeft ||
            paddedH - resolved.padTop >= resolved.h ||
            paddedW - resolved.padLeft >= resolved.w;
        const uint16_t laneBit = static_cast<uint16_t>(1U << lane);
        if (padding) {
            issued.intermediate.mask |= laneBit;
            continue;
        }

        const uint64_t inputH = paddedH - resolved.padTop;
        const uint64_t inputW = paddedW - resolved.padLeft;
        const auto address = mapper.locate(
            iterators.n, iterators.c, inputH, inputW);
        issued.requests[lane] = {
            true,
            address.bank,
            address.row,
            address.laneSel,
            static_cast<uint8_t>(lane),
        };
    }
    return issued;
}

void
Im2ColModel::collectResponses(
    Registers &next, const Registers &old,
    const SramRequest &request, const SramResponse &response) const
{
    for (std::size_t bank = 0; bank < ScratchpadBanks; ++bank) {
        if (!response.valid[bank]) {
            continue;
        }
        for (std::size_t lane = 0; lane < Im2ColLanes; ++lane) {
            const auto &laneRequest = old.laneRequests[lane];
            if (laneRequest.valid && !old.laneDone[lane] &&
                laneRequest.bank == bank &&
                laneRequest.row == request.address[bank]) {
                next.intermediate.data[laneRequest.dstLane] =
                    response.data[bank];
                next.intermediate.mask |= static_cast<uint16_t>(
                    1U << laneRequest.dstLane);
                next.laneDone[lane] = true;
            }
        }
    }
}

bool
Im2ColModel::allLanesDone(const Registers &value) const
{
    for (std::size_t lane = 0; lane < Im2ColLanes; ++lane) {
        if (value.laneRequests[lane].valid && !value.laneDone[lane]) {
            return false;
        }
    }
    return true;
}

bool
Im2ColModel::advanceIterators(Iterators &iterators) const
{
    if (iterators.kw + 1 < resolved.kernelW) {
        ++iterators.kw;
        return false;
    }
    iterators.kw = 0;
    if (iterators.kh + 1 < resolved.kernelH) {
        ++iterators.kh;
        return false;
    }
    iterators.kh = 0;
    if (iterators.c + 1 < resolved.c) {
        ++iterators.c;
        return false;
    }
    iterators.c = 0;
    if (resolved.w > BlockSize &&
        iterators.owBase + BlockSize < resolved.outW) {
        iterators.owBase += BlockSize;
        return false;
    }
    iterators.owBase = 0;
    if (resolved.w <= BlockSize &&
        iterators.oh + dimensions.rowsPerWord < resolved.outH) {
        iterators.oh += dimensions.rowsPerWord;
        return false;
    }
    if (resolved.w > BlockSize && iterators.oh + 1 < resolved.outH) {
        ++iterators.oh;
        return false;
    }
    iterators.oh = 0;
    if (iterators.n + 1 < resolved.n) {
        ++iterators.n;
        return false;
    }
    return true;
}

void
Im2ColModel::updatePushLaneStats(const Registers &old)
{
    const uint64_t presented = bitCount(old.intermediate.mask);
    uint64_t sramReads = 0;
    for (const auto &request : old.laneRequests) {
        sramReads += request.valid;
    }
    if (sramReads > presented) {
        throw std::logic_error("SRAM lanes exceed presented lanes");
    }
    counters.presentedLanes = checkedAdd(
        counters.presentedLanes, presented, "presented lane count");
    counters.sramReadLanes = checkedAdd(
        counters.sramReadLanes, sramReads, "SRAM-read lane count");
    counters.paddingZeroLanes = checkedAdd(
        counters.paddingZeroLanes, presented - sramReads,
        "padding-zero lane count");
    counters.invalidLanes = checkedAdd(
        counters.invalidLanes, BlockSize - presented,
        "invalid lane count");
}

void
Im2ColModel::updateConflictStats(const Registers &old)
{
    uint64_t maximumRows = 0;
    for (uint64_t bank = 0; bank < SpBanks; ++bank) {
        std::array<uint16_t, Im2ColLanes> rows{};
        uint64_t rowCount = 0;
        for (const auto &request : old.laneRequests) {
            if (!request.valid || request.bank != bank) {
                continue;
            }
            const auto begin = rows.begin();
            const auto end = begin + static_cast<std::ptrdiff_t>(rowCount);
            if (std::find(begin, end, request.row) == end) {
                rows[rowCount++] = request.row;
            }
        }
        if (rowCount > 1) {
            counters.bankRowConflicts = checkedAdd(
                counters.bankRowConflicts, rowCount - 1,
                "bank row conflict count");
        }
        maximumRows = std::max(maximumRows, rowCount);
    }
    if (maximumRows > 1) {
        counters.extraCollectCycles = checkedAdd(
            counters.extraCollectCycles, maximumRows - 1,
            "extra collect cycle count");
    }
}

void
Im2ColModel::checkInvariants(const Registers &value) const
{
    if (value.fifoCount > FifoDepth) {
        throw std::logic_error("FIFO count exceeds fixed depth");
    }
    if (counters.popCount != counters.handshakeCount) {
        throw std::logic_error("FIFO pop and handshake counts diverged");
    }
    if (counters.pushCount < counters.popCount ||
        counters.pushCount - counters.popCount != value.fifoCount) {
        throw std::logic_error("FIFO push/pop conservation failed");
    }
    if (counters.pushCount > dimensions.expectedVectors ||
        counters.handshakeCount > counters.pushCount) {
        throw std::logic_error("feed vector conservation failed");
    }
}

Im2ColCycle
Im2ColModel::tick(bool feedReady)
{
    const Registers old = registers;
    const SramRequest request = arbitrateBanks(
        old.laneRequests, old.laneDone);
    const SramResponse response =
        scratchpad.combinationalResponse(request);
    const bool feedValid = old.fifoCount != 0;
    const bool fifoPop = feedValid && feedReady;
    const bool fifoPush = old.state == Im2ColState::Push &&
        old.fifoCount != FifoDepth;

    Im2ColCycle observation;
    observation.cycle = cycleNumber;
    observation.state = old.state;
    observation.busy = old.state != Im2ColState::Idle;
    observation.done = old.done;
    observation.fifoCount = old.fifoCount;
    observation.fifoReadPointer = old.fifoReadPointer;
    observation.fifoWritePointer = old.fifoWritePointer;
    observation.request = request;
    observation.response = response;
    observation.feedValid = feedValid;
    observation.feedReady = feedReady;
    observation.feed = feedValid ? old.fifo[old.fifoReadPointer] : FeedVector{};
    observation.fifoPush = fifoPush;
    observation.fifoPop = fifoPop;

    if (old.done && !rtlDoneAt) {
        rtlDoneAt = cycleNumber;
    }
    if ((rtlDoneAt || old.done) && old.fifoCount == 0 && !drainedAt) {
        if (counters.pushCount != dimensions.expectedVectors ||
            counters.handshakeCount != dimensions.expectedVectors) {
            throw std::logic_error(
                "drained before all expected vectors were transferred");
        }
        drainedAt = cycleNumber;
        observation.drained = true;
    } else {
        observation.drained = drainedAt.has_value();
    }

    counters.fifoOccupancySamples = checkedAdd(
        counters.fifoOccupancySamples, 1, "FIFO occupancy sample count");
    counters.fifoOccupancySum = checkedAdd(
        counters.fifoOccupancySum, old.fifoCount,
        "FIFO occupancy sum");
    counters.fifoPeak = std::max<uint64_t>(
        counters.fifoPeak, old.fifoCount);
    if (old.state == Im2ColState::Push && old.fifoCount == FifoDepth) {
        counters.fifoFullStallCycles = checkedAdd(
            counters.fifoFullStallCycles, 1, "FIFO full stall count");
    }
    if (feedValid && !feedReady) {
        counters.backpressureCycles = checkedAdd(
            counters.backpressureCycles, 1, "backpressure cycle count");
    }
    for (std::size_t bank = 0; bank < ScratchpadBanks; ++bank) {
        if (request.valid[bank]) {
            counters.bankRequestCycles[bank] = checkedAdd(
                counters.bankRequestCycles[bank], 1,
                "bank request cycle count");
        }
    }

    Registers next = old;
    next.done = false;

    if (fifoPush) {
        next.fifo[old.fifoWritePointer] = old.intermediate;
        next.fifoWritePointer = static_cast<uint8_t>(
            (old.fifoWritePointer + 1) % FifoDepth);
        counters.pushCount = checkedAdd(
            counters.pushCount, 1, "FIFO push count");
        updatePushLaneStats(old);
        updateConflictStats(old);
    }
    if (fifoPop) {
        next.fifoReadPointer = static_cast<uint8_t>(
            (old.fifoReadPointer + 1) % FifoDepth);
        counters.popCount = checkedAdd(
            counters.popCount, 1, "FIFO pop count");
        counters.handshakeCount = checkedAdd(
            counters.handshakeCount, 1, "feed handshake count");
    }
    if (fifoPush && !fifoPop) {
        next.fifoCount = old.fifoCount + 1;
    } else if (!fifoPush && fifoPop) {
        next.fifoCount = old.fifoCount - 1;
    }

    switch (old.state) {
      case Im2ColState::Idle:
        break;
      case Im2ColState::Issue: {
        const auto issued = buildIssuedVector(old.iterators);
        next.laneRequests = issued.requests;
        next.laneDone.fill(false);
        next.intermediate = issued.intermediate;
        next.state = Im2ColState::Collect;
        break;
      }
      case Im2ColState::Collect:
        collectResponses(next, old, request, response);
        if (allLanesDone(old)) {
            next.state = Im2ColState::Push;
        }
        break;
      case Im2ColState::Push:
        if (old.fifoCount != FifoDepth) {
            next.state = Im2ColState::Next;
        }
        break;
      case Im2ColState::Next:
        next.state = advanceIterators(next.iterators) ?
            Im2ColState::Done : Im2ColState::Issue;
        break;
      case Im2ColState::Done:
        next.done = true;
        next.state = Im2ColState::Idle;
        break;
    }

    registers = next;
    checkInvariants(registers);
    cycleNumber = checkedAdd(cycleNumber, 1, "model cycle");
    return observation;
}

std::optional<uint64_t>
Im2ColModel::postDoneDrainCycles() const
{
    if (!rtlDoneAt || !drainedAt) {
        return std::nullopt;
    }
    return *drainedAt - *rtlDoneAt;
}

} // namespace gem5::sau_n
