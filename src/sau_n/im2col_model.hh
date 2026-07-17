#ifndef __SAU_N_IM2COL_MODEL_HH__
#define __SAU_N_IM2COL_MODEL_HH__

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "sau_n/banked_scratchpad.hh"
#include "sau_n/im2col_types.hh"

namespace gem5::sau_n
{

inline constexpr std::size_t Im2ColLanes =
    static_cast<std::size_t>(BlockSize);
inline constexpr std::size_t Im2ColFifoDepth =
    static_cast<std::size_t>(FifoDepth);

struct LaneRequest
{
    bool valid = false;
    uint8_t bank = 0;
    uint16_t row = 0;
    uint8_t laneSel = 0;
    uint8_t dstLane = 0;
};

using LaneRequests = std::array<LaneRequest, Im2ColLanes>;
using LaneBits = std::array<bool, Im2ColLanes>;

struct FeedVector
{
    std::array<uint8_t, Im2ColLanes> data{};
    uint16_t mask = 0;

    bool operator==(const FeedVector &other) const
    {
        return data == other.data && mask == other.mask;
    }
};

SramRequest arbitrateBanks(
    const LaneRequests &laneRequests, const LaneBits &laneDone);

class PeriodicReady
{
  public:
    PeriodicReady(uint64_t period = 1, uint64_t highCycles = 1);
    bool at(uint64_t cycle) const;

  private:
    uint64_t period;
    uint64_t highCycles;
};

struct Im2ColModelStats
{
    uint64_t pushCount = 0;
    uint64_t popCount = 0;
    uint64_t handshakeCount = 0;
    uint64_t presentedLanes = 0;
    uint64_t sramReadLanes = 0;
    uint64_t paddingZeroLanes = 0;
    uint64_t invalidLanes = 0;
    std::array<uint64_t, ScratchpadBanks> bankRequestCycles{};
    uint64_t bankRowConflicts = 0;
    uint64_t extraCollectCycles = 0;
    uint64_t fifoOccupancySamples = 0;
    uint64_t fifoOccupancySum = 0;
    uint64_t fifoPeak = 0;
    uint64_t fifoFullStallCycles = 0;
    uint64_t backpressureCycles = 0;
};

struct Im2ColCycle
{
    uint64_t cycle = 0;
    Im2ColState state = Im2ColState::Idle;
    bool busy = false;
    bool done = false;
    uint8_t fifoCount = 0;
    uint8_t fifoReadPointer = 0;
    uint8_t fifoWritePointer = 0;
    SramRequest request;
    SramResponse response;
    bool feedValid = false;
    bool feedReady = false;
    FeedVector feed;
    bool fifoPush = false;
    bool fifoPop = false;
    bool drained = false;
};

class Im2ColModel
{
  public:
    explicit Im2ColModel(const ResolvedConfig &config);
    Im2ColModel(
        const ResolvedConfig &config,
        const BankedScratchpad &preloadedScratchpad);

    Im2ColCycle tick(bool feedReady);

    const ResolvedConfig &config() const { return resolved; }
    const DerivedConfig &derived() const { return dimensions; }
    const Im2ColModelStats &stats() const { return counters; }
    uint64_t nextCycle() const { return cycleNumber; }
    bool hasDrained() const { return drainedAt.has_value(); }
    std::optional<uint64_t> rtlDoneCycle() const { return rtlDoneAt; }
    std::optional<uint64_t> drainedCycle() const { return drainedAt; }
    std::optional<uint64_t> postDoneDrainCycles() const;

  private:
    struct Iterators
    {
        uint64_t n = 0;
        uint64_t c = 0;
        uint64_t oh = 0;
        uint64_t owBase = 0;
        uint64_t kh = 0;
        uint64_t kw = 0;
    };

    struct Registers
    {
        Im2ColState state = Im2ColState::Issue;
        bool done = false;
        Iterators iterators;
        LaneRequests laneRequests{};
        LaneBits laneDone{};
        FeedVector intermediate;
        std::array<FeedVector, Im2ColFifoDepth> fifo{};
        uint8_t fifoCount = 0;
        uint8_t fifoReadPointer = 0;
        uint8_t fifoWritePointer = 0;
    };

    struct IssuedVector
    {
        LaneRequests requests{};
        FeedVector intermediate;
    };

    IssuedVector buildIssuedVector(const Iterators &iterators) const;
    void collectResponses(
        Registers &next, const Registers &old,
        const SramRequest &request, const SramResponse &response) const;
    bool allLanesDone(const Registers &registers) const;
    bool advanceIterators(Iterators &iterators) const;
    void updatePushLaneStats(const Registers &old);
    void updateConflictStats(const Registers &old);
    void checkInvariants(const Registers &registers) const;

    ResolvedConfig resolved;
    DerivedConfig dimensions;
    BankedScratchpad scratchpad;
    Registers registers;
    Im2ColModelStats counters;
    uint64_t cycleNumber = 0;
    std::optional<uint64_t> rtlDoneAt;
    std::optional<uint64_t> drainedAt;
};

} // namespace gem5::sau_n

#endif // __SAU_N_IM2COL_MODEL_HH__
