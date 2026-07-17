#include <gtest/gtest.h>

#include <array>
#include <stdexcept>
#include <vector>

#include "sau_n/im2col_address.hh"
#include "sau_n/im2col_model.hh"

namespace gem5::sau_n
{
namespace
{

ResolvedConfig
baseConfig()
{
    ResolvedConfig config;
    config.name = "model";
    config.n = 1;
    config.c = 1;
    config.h = 1;
    config.w = 1;
    config.outH = 1;
    config.outW = 1;
    config.kernelH = 1;
    config.kernelW = 1;
    config.strideH = 1;
    config.strideW = 1;
    config.dilationH = 1;
    config.dilationW = 1;
    return config;
}

std::vector<FeedVector>
runToDrained(Im2ColModel &model, const PeriodicReady &ready)
{
    std::vector<FeedVector> handshakes;
    for (uint64_t attempts = 0; attempts < 100000; ++attempts) {
        const auto cycle = model.tick(ready.at(model.nextCycle()));
        if (cycle.fifoPop) {
            handshakes.push_back(cycle.feed);
        }
        if (cycle.drained) {
            return handshakes;
        }
    }
    throw std::runtime_error("test model did not drain");
}

FeedVector
w5Vector(uint64_t firstH, uint64_t rows)
{
    FeedVector vector;
    for (uint64_t localH = 0; localH < rows; ++localH) {
        for (uint64_t w = 0; w < 5; ++w) {
            const uint64_t lane = localH * 5 + w;
            vector.data[lane] = tbActValueV1(0, 0, firstH + localH, w);
            vector.mask |= static_cast<uint16_t>(1U << lane);
        }
    }
    return vector;
}

TEST(Im2ColArbitration, SelectsFirstLaneAndCoalescesEqualRows)
{
    LaneRequests requests{};
    LaneBits done{};
    requests[0] = {true, 3, 11, 3, 0};
    requests[1] = {true, 3, 11, 3, 1};
    requests[2] = {true, 3, 12, 3, 2};
    requests[3] = {true, 4, 9, 4, 3};

    auto selected = arbitrateBanks(requests, done);
    EXPECT_TRUE(selected.valid[3]);
    EXPECT_EQ(selected.address[3], 11);
    EXPECT_TRUE(selected.valid[4]);
    EXPECT_EQ(selected.address[4], 9);

    done[0] = true;
    done[1] = true;
    selected = arbitrateBanks(requests, done);
    EXPECT_TRUE(selected.valid[3]);
    EXPECT_EQ(selected.address[3], 12);

    done[2] = true;
    done[3] = true;
    selected = arbitrateBanks(requests, done);
    for (bool valid : selected.valid) {
        EXPECT_FALSE(valid);
    }
}

TEST(Im2ColArbitration, RejectsOutOfRangeValidRequest)
{
    LaneRequests requests{};
    LaneBits done{};
    requests[0] = {true, 16, 0, 0, 0};
    EXPECT_THROW(arbitrateBanks(requests, done), std::out_of_range);
}

TEST(PeriodicReadyTest, ValidatesAndGeneratesFrozenPattern)
{
    EXPECT_THROW(PeriodicReady(0, 1), std::invalid_argument);
    EXPECT_THROW(PeriodicReady(4, 0), std::invalid_argument);
    EXPECT_THROW(PeriodicReady(4, 5), std::invalid_argument);

    const PeriodicReady ready(4, 2);
    const std::array<bool, 8> expected = {
        true, true, false, false, true, true, false, false};
    for (uint64_t cycle = 0; cycle < expected.size(); ++cycle) {
        EXPECT_EQ(ready.at(cycle), expected[cycle]);
    }
}

TEST(Im2ColModelTest, PreservesCycleAnchorsAndNoEmptyFifoBypass)
{
    auto config = baseConfig();
    config.w = 16;
    config.outW = 16;
    Im2ColModel model(config);

    const auto issue = model.tick(true);
    EXPECT_EQ(issue.cycle, uint64_t{0});
    EXPECT_EQ(issue.state, Im2ColState::Issue);
    EXPECT_TRUE(issue.busy);
    EXPECT_FALSE(issue.done);

    const auto collectData = model.tick(true);
    EXPECT_EQ(collectData.state, Im2ColState::Collect);
    for (bool valid : collectData.request.valid) {
        EXPECT_TRUE(valid);
    }

    const auto collectDone = model.tick(true);
    EXPECT_EQ(collectDone.state, Im2ColState::Collect);
    for (bool valid : collectDone.request.valid) {
        EXPECT_FALSE(valid);
    }

    const auto push = model.tick(true);
    EXPECT_EQ(push.state, Im2ColState::Push);
    EXPECT_TRUE(push.fifoPush);
    EXPECT_FALSE(push.feedValid);
    EXPECT_FALSE(push.fifoPop);

    const auto next = model.tick(true);
    EXPECT_EQ(next.state, Im2ColState::Next);
    EXPECT_TRUE(next.feedValid);
    EXPECT_TRUE(next.fifoPop);
    EXPECT_EQ(next.feed.mask, uint16_t{0xffff});

    const auto doneState = model.tick(true);
    EXPECT_EQ(doneState.state, Im2ColState::Done);
    EXPECT_TRUE(doneState.busy);
    EXPECT_FALSE(doneState.done);

    const auto donePulse = model.tick(true);
    EXPECT_EQ(donePulse.state, Im2ColState::Idle);
    EXPECT_FALSE(donePulse.busy);
    EXPECT_TRUE(donePulse.done);
    EXPECT_TRUE(donePulse.drained);
    EXPECT_EQ(model.rtlDoneCycle(), std::optional<uint64_t>{6});
    EXPECT_EQ(model.drainedCycle(), std::optional<uint64_t>{6});
    EXPECT_EQ(model.postDoneDrainCycles(), std::optional<uint64_t>{0});
}

TEST(Im2ColModelTest, ProducesPackedW5FeedSequence)
{
    auto config = baseConfig();
    config.h = 4;
    config.w = 5;
    config.outH = 4;
    config.outW = 5;
    Im2ColModel model(config);

    const auto handshakes = runToDrained(model, PeriodicReady{});

    ASSERT_EQ(handshakes.size(), 2U);
    EXPECT_EQ(handshakes[0], w5Vector(0, 3));
    EXPECT_EQ(handshakes[1], w5Vector(3, 1));
    EXPECT_EQ(model.stats().pushCount, uint64_t{2});
    EXPECT_EQ(model.stats().handshakeCount, uint64_t{2});
    EXPECT_EQ(model.stats().invalidLanes, uint64_t{12});
}

TEST(Im2ColModelTest, AdvancesKwChannelAndWGroupInRtlOrder)
{
    auto config = baseConfig();
    config.c = 2;
    config.w = 20;
    config.outW = 20;
    config.kernelW = 2;
    Im2ColModel model(config);

    const auto handshakes = runToDrained(model, PeriodicReady{});

    ASSERT_EQ(handshakes.size(), 8U);
    EXPECT_EQ(handshakes[0].mask, uint16_t{0xffff});
    EXPECT_EQ(handshakes[0].data[0], tbActValueV1(0, 0, 0, 0));
    EXPECT_EQ(handshakes[0].data[15], tbActValueV1(0, 0, 0, 15));
    EXPECT_EQ(handshakes[1].data[0], tbActValueV1(0, 0, 0, 1));
    EXPECT_EQ(handshakes[2].data[0], tbActValueV1(0, 1, 0, 0));
    EXPECT_EQ(handshakes[3].data[0], tbActValueV1(0, 1, 0, 1));

    EXPECT_EQ(handshakes[4].mask, uint16_t{0x000f});
    EXPECT_EQ(handshakes[4].data[0], tbActValueV1(0, 0, 0, 16));
    EXPECT_EQ(handshakes[4].data[3], tbActValueV1(0, 0, 0, 19));
    EXPECT_EQ(handshakes[5].mask, uint16_t{0x000f});
    EXPECT_EQ(handshakes[5].data[0], tbActValueV1(0, 0, 0, 17));
    EXPECT_EQ(handshakes[5].data[2], tbActValueV1(0, 0, 0, 19));
    EXPECT_EQ(handshakes[5].data[3], uint8_t{0});
    EXPECT_EQ(handshakes[6].data[0], tbActValueV1(0, 1, 0, 16));
    EXPECT_EQ(handshakes[7].data[0], tbActValueV1(0, 1, 0, 17));
    EXPECT_EQ(model.stats().pushCount, model.derived().expectedVectors);
}

TEST(Im2ColModelTest, PaddingIsPresentedZeroWithoutSramRequests)
{
    auto config = baseConfig();
    config.w = 5;
    config.outW = 5;
    config.kernelH = 3;
    config.kernelW = 3;
    config.padTop = 1;
    config.padLeft = 1;
    Im2ColModel model(config);

    model.tick(true);
    const auto firstCollect = model.tick(true);
    for (bool valid : firstCollect.request.valid) {
        EXPECT_FALSE(valid);
    }
    const auto push = model.tick(true);
    EXPECT_EQ(push.state, Im2ColState::Push);
    EXPECT_TRUE(push.fifoPush);

    const auto feed = model.tick(true);
    EXPECT_TRUE(feed.fifoPop);
    EXPECT_EQ(feed.feed.mask, uint16_t{0x001f});
    EXPECT_EQ(feed.feed.data, (std::array<uint8_t, 16>{}));
}

TEST(Im2ColModelTest, SerializesSameBankDifferentRows)
{
    auto config = baseConfig();
    config.w = 20;
    config.outW = 3;
    config.strideW = 8;
    Im2ColModel model(config);

    model.tick(true);
    const auto first = model.tick(true);
    EXPECT_TRUE(first.request.valid[0]);
    EXPECT_EQ(first.request.address[0], 0);
    const auto second = model.tick(true);
    EXPECT_TRUE(second.request.valid[0]);
    EXPECT_EQ(second.request.address[0], 1);
    const auto decision = model.tick(true);
    EXPECT_FALSE(decision.request.valid[0]);

    const auto handshakes = runToDrained(model, PeriodicReady{});
    ASSERT_EQ(handshakes.size(), 1U);
    EXPECT_EQ(handshakes[0].mask, uint16_t{0x0007});
    EXPECT_EQ(handshakes[0].data[0], uint8_t{1});
    EXPECT_EQ(handshakes[0].data[1], uint8_t{9});
    EXPECT_EQ(handshakes[0].data[2], uint8_t{17});
    EXPECT_EQ(model.stats().bankRowConflicts, uint64_t{1});
    EXPECT_EQ(model.stats().extraCollectCycles, uint64_t{1});
}

TEST(Im2ColModelTest, FullFifoPopDoesNotEnableSameCyclePush)
{
    auto config = baseConfig();
    config.h = 65;
    config.w = 1;
    config.outH = 65;
    Im2ColModel model(config);

    bool foundFullPush = false;
    for (uint64_t attempts = 0; attempts < 200; ++attempts) {
        const auto stalled = model.tick(false);
        if (stalled.state == Im2ColState::Push &&
            stalled.fifoCount == static_cast<uint8_t>(FifoDepth)) {
            EXPECT_FALSE(stalled.fifoPush);
            foundFullPush = true;
            break;
        }
    }
    ASSERT_TRUE(foundFullPush);

    const auto popOnly = model.tick(true);
    EXPECT_EQ(popOnly.state, Im2ColState::Push);
    EXPECT_EQ(popOnly.fifoCount, static_cast<uint8_t>(FifoDepth));
    EXPECT_FALSE(popOnly.fifoPush);
    EXPECT_TRUE(popOnly.fifoPop);

    const auto pushAfterSpace = model.tick(false);
    EXPECT_EQ(pushAfterSpace.state, Im2ColState::Push);
    EXPECT_EQ(pushAfterSpace.fifoCount,
              static_cast<uint8_t>(FifoDepth - 1));
    EXPECT_TRUE(pushAfterSpace.fifoPush);
    EXPECT_FALSE(pushAfterSpace.fifoPop);
}

TEST(Im2ColModelTest, PeriodicBackpressureDrainsAfterDone)
{
    auto config = baseConfig();
    config.h = 17;
    config.w = 1;
    config.outH = 17;
    Im2ColModel model(config);
    const PeriodicReady ready(50, 1);
    uint8_t fifoAtDone = 0;

    for (uint64_t attempts = 0; attempts < 10000; ++attempts) {
        const auto cycle = model.tick(ready.at(model.nextCycle()));
        if (cycle.done) {
            fifoAtDone = cycle.fifoCount;
        }
        if (cycle.drained) {
            break;
        }
    }

    ASSERT_TRUE(model.hasDrained());
    EXPECT_GT(fifoAtDone, uint8_t{0});
    ASSERT_TRUE(model.rtlDoneCycle());
    ASSERT_TRUE(model.drainedCycle());
    EXPECT_GT(*model.drainedCycle(), *model.rtlDoneCycle());
    EXPECT_EQ(model.stats().pushCount, model.derived().expectedVectors);
    EXPECT_EQ(model.stats().handshakeCount, model.derived().expectedVectors);
    EXPECT_EQ(model.stats().pushCount, uint64_t{2});
    EXPECT_EQ(model.stats().popCount, uint64_t{2});
    EXPECT_EQ(model.stats().fifoPeak, uint64_t{2});
    EXPECT_GT(model.stats().backpressureCycles, uint64_t{0});
}

} // anonymous namespace
} // namespace gem5::sau_n
