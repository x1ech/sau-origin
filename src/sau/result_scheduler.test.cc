#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <utility>
#include <vector>

#include "sau/result_scheduler.hh"

namespace gem5::sau
{
namespace
{

std::vector<std::pair<uint64_t, uint32_t>>
collect(ResultScheduler &scheduler, uint64_t lastCycle)
{
    std::vector<std::pair<uint64_t, uint32_t>> events;
    for (uint64_t cycle = 0; cycle <= lastCycle; ++cycle) {
        if (scheduler.canProduce(Cycles(cycle))) {
            events.emplace_back(cycle, scheduler.produce(Cycles(cycle)));
        }
    }
    return events;
}

TEST(ResultScheduler, ProducesResultBurstsFromFirstArrayInput)
{
    ResultScheduler scheduler(64, 32, Cycles(343), Cycles(234));
    scheduler.start(Cycles(269));

    const auto events = collect(scheduler, 910);

    ASSERT_EQ(events.size(), 64);
    const std::vector<std::pair<uint64_t, uint32_t>> firstFour(
        events.begin(), events.begin() + 4);
    const std::vector<std::pair<uint64_t, uint32_t>> expectedFirstFour = {
        {612, 0},
        {613, 1},
        {614, 2},
        {615, 3},
    };
    EXPECT_EQ(firstFour, expectedFirstFour);
    EXPECT_EQ(events[31], (std::pair<uint64_t, uint32_t>{643, 31}));
    EXPECT_EQ(events[32], (std::pair<uint64_t, uint32_t>{878, 32}));
    EXPECT_EQ(events[63], (std::pair<uint64_t, uint32_t>{909, 63}));
    EXPECT_TRUE(scheduler.complete());
}

TEST(ResultScheduler, DefersAFlowWithoutCollapsingLaterSpacing)
{
    ResultScheduler scheduler(64, 32, Cycles(343), Cycles(234));
    scheduler.start(Cycles(269));

    scheduler.deferUntil(Cycles(1000));

    EXPECT_TRUE(scheduler.canProduce(Cycles(1000)));
    EXPECT_EQ(scheduler.produce(Cycles(1000)), 0U);
    EXPECT_FALSE(scheduler.canProduce(Cycles(1000)));
    EXPECT_TRUE(scheduler.canProduce(Cycles(1001)));

    for (uint32_t index = 1; index < 32; ++index) {
        EXPECT_EQ(scheduler.produce(Cycles(1000 + index)), index);
    }
    EXPECT_FALSE(scheduler.canProduce(Cycles(1265)));
    EXPECT_TRUE(scheduler.canProduce(Cycles(1266)));
}

TEST(ResultScheduler, AllowsAnExternalPerTickProducerToReleaseResults)
{
    ResultScheduler scheduler(2, 1, Cycles(343), Cycles(0));

    EXPECT_EQ(scheduler.produce(), 0U);
    EXPECT_EQ(scheduler.produce(), 1U);
    EXPECT_TRUE(scheduler.complete());
}

} // anonymous namespace
} // namespace gem5::sau
