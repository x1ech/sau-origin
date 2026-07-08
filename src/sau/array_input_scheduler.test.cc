#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <utility>
#include <vector>

#include "sau/array_input_scheduler.hh"

namespace gem5::sau
{
namespace
{

using testing::ElementsAre;

std::vector<std::pair<char, uint32_t>>
collect(ArrayInputScheduler &scheduler)
{
    std::vector<std::pair<char, uint32_t>> events;
    while (!scheduler.complete()) {
        if (scheduler.canIssueB()) {
            events.emplace_back('B', scheduler.issueB());
        }
        if (scheduler.canIssueA()) {
            events.emplace_back('A', scheduler.issueA());
        }
    }
    return events;
}

std::vector<std::tuple<uint64_t, char, uint32_t>>
collectCycles(ArrayInputScheduler &scheduler)
{
    std::vector<std::tuple<uint64_t, char, uint32_t>> events;
    for (uint64_t cycle = 0; !scheduler.complete(); ++cycle) {
        if (scheduler.canIssueB()) {
            events.emplace_back(cycle, 'B', scheduler.issueB());
        }
        if (scheduler.canIssueA()) {
            events.emplace_back(cycle, 'A', scheduler.issueA());
        }
        scheduler.advanceCycle();
    }
    return events;
}

TEST(ArrayInputScheduler, SkewsBBehindA)
{
    ArrayInputScheduler scheduler(8, 3);

    EXPECT_THAT(collect(scheduler), ElementsAre(
        std::pair<char, uint32_t>{'A', 0},
        std::pair<char, uint32_t>{'A', 1},
        std::pair<char, uint32_t>{'A', 2},
        std::pair<char, uint32_t>{'B', 0},
        std::pair<char, uint32_t>{'A', 3},
        std::pair<char, uint32_t>{'B', 1},
        std::pair<char, uint32_t>{'A', 4},
        std::pair<char, uint32_t>{'B', 2},
        std::pair<char, uint32_t>{'A', 5},
        std::pair<char, uint32_t>{'B', 3},
        std::pair<char, uint32_t>{'A', 6},
        std::pair<char, uint32_t>{'B', 4},
        std::pair<char, uint32_t>{'A', 7},
        std::pair<char, uint32_t>{'B', 5},
        std::pair<char, uint32_t>{'B', 6},
        std::pair<char, uint32_t>{'B', 7}));
}

TEST(ArrayInputScheduler, PreservesLegacySameCycleOrderWithZeroSkew)
{
    ArrayInputScheduler scheduler(3, 0);

    EXPECT_THAT(collect(scheduler), ElementsAre(
        std::pair<char, uint32_t>{'B', 0},
        std::pair<char, uint32_t>{'A', 0},
        std::pair<char, uint32_t>{'B', 1},
        std::pair<char, uint32_t>{'A', 1},
        std::pair<char, uint32_t>{'B', 2},
        std::pair<char, uint32_t>{'A', 2}));
}

TEST(ArrayInputScheduler, InsertsTileAndFlowBoundaryGaps)
{
    ArrayInputScheduler scheduler(
        320, 32, 32, 256, Cycles(1), Cycles(3));

    const auto events = collectCycles(scheduler);

    EXPECT_THAT(events, testing::Contains(
        std::tuple<uint64_t, char, uint32_t>{0, 'A', 0}));
    EXPECT_THAT(events, testing::Contains(
        std::tuple<uint64_t, char, uint32_t>{31, 'A', 31}));
    EXPECT_THAT(events, testing::Contains(
        std::tuple<uint64_t, char, uint32_t>{32, 'B', 0}));
    EXPECT_THAT(events, testing::Contains(
        std::tuple<uint64_t, char, uint32_t>{32, 'A', 32}));
    EXPECT_THAT(events, testing::Not(testing::Contains(
        std::tuple<uint64_t, char, uint32_t>{64, 'B', 32})));
    EXPECT_THAT(events, testing::Contains(
        std::tuple<uint64_t, char, uint32_t>{65, 'B', 32}));
    EXPECT_THAT(events, testing::Contains(
        std::tuple<uint64_t, char, uint32_t>{65, 'A', 64}));
    EXPECT_THAT(events, testing::Not(testing::Contains(
        std::tuple<uint64_t, char, uint32_t>{295, 'B', 256})));
    EXPECT_THAT(events, testing::Not(testing::Contains(
        std::tuple<uint64_t, char, uint32_t>{296, 'B', 256})));
    EXPECT_THAT(events, testing::Not(testing::Contains(
        std::tuple<uint64_t, char, uint32_t>{297, 'B', 256})));
    EXPECT_THAT(events, testing::Contains(
        std::tuple<uint64_t, char, uint32_t>{298, 'B', 256}));
    EXPECT_THAT(events, testing::Contains(
        std::tuple<uint64_t, char, uint32_t>{298, 'A', 288}));
}

} // anonymous namespace
} // namespace gem5::sau
