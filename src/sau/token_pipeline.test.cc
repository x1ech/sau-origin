#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "sau/token_pipeline.hh"

namespace gem5::sau
{
namespace
{

TEST(TokenBuffer, RefusesPushAtCapacity)
{
    TokenBuffer buffer(1);

    EXPECT_TRUE(buffer.canPush());
    buffer.push({1, 7, Cycles(3), false});

    EXPECT_FALSE(buffer.canPush());
    EXPECT_EQ(buffer.size(), 1);
    EXPECT_EQ(buffer.front().index, 7);

    buffer.pop();
    EXPECT_TRUE(buffer.canPush());
    EXPECT_EQ(buffer.size(), 0);
}

TEST(ArrayPipeline, ProducesAfterFillLatency)
{
    ArrayPipeline pipeline(Cycles(3), Cycles(1), 4);

    pipeline.accept(1, 0, false, Cycles(0));
    pipeline.accept(1, 1, true, Cycles(1));

    EXPECT_FALSE(pipeline.hasReady(Cycles(2)));
    ASSERT_TRUE(pipeline.hasReady(Cycles(3)));
    EXPECT_EQ(pipeline.takeReady(Cycles(3)).index, 0);
    EXPECT_FALSE(pipeline.hasReady(Cycles(3)));
    ASSERT_TRUE(pipeline.hasReady(Cycles(4)));
    EXPECT_EQ(pipeline.takeReady(Cycles(4)).index, 1);
}

TEST(ArrayPipeline, EnforcesInitiationInterval)
{
    ArrayPipeline pipeline(Cycles(3), Cycles(2), 4);

    ASSERT_TRUE(pipeline.canAccept(Cycles(0)));
    pipeline.accept(1, 0, false, Cycles(0));

    EXPECT_FALSE(pipeline.canAccept(Cycles(0)));
    EXPECT_FALSE(pipeline.canAccept(Cycles(1)));
    EXPECT_TRUE(pipeline.canAccept(Cycles(2)));
}

TEST(ArrayPipeline, AdditionalAcceptDoesNotConsumeInitiationInterval)
{
    ArrayPipeline pipeline(Cycles(3), Cycles(2), 4);

    pipeline.accept(1, 0, false, Cycles(0));
    ASSERT_TRUE(pipeline.canAcceptAdditional());
    pipeline.acceptAdditional(1, 1, false, Cycles(0));

    EXPECT_FALSE(pipeline.canAccept(Cycles(1)));
    EXPECT_TRUE(pipeline.canAccept(Cycles(2)));
    ASSERT_TRUE(pipeline.hasReady(Cycles(3)));
    EXPECT_EQ(pipeline.takeReady(Cycles(3)).index, 0);
    ASSERT_TRUE(pipeline.hasReady(Cycles(3)));
    EXPECT_EQ(pipeline.takeReady(Cycles(3)).index, 1);
}

TEST(ArrayPipeline, ResetStartsANewCommandLocalTimingEpoch)
{
    ArrayPipeline pipeline(Cycles(3), Cycles(2), 4);
    pipeline.accept(1, 0, false, Cycles(10));
    ASSERT_TRUE(pipeline.hasReady(Cycles(13)));
    pipeline.takeReady(Cycles(13));

    pipeline.reset();

    EXPECT_TRUE(pipeline.canAccept(Cycles(0)));
    pipeline.accept(2, 0, false, Cycles(0));
    EXPECT_TRUE(pipeline.hasReady(Cycles(3)));
}

TEST(ArrayPipeline, CommandResetClearsOnlyCommandLocalShadowTokens)
{
    ArrayPipeline pipeline(Cycles(10), Cycles(2), 4);
    pipeline.accept(1, 0, false, Cycles(0));
    EXPECT_EQ(pipeline.inFlight(), 1U);

    pipeline.resetForCommand();

    EXPECT_EQ(pipeline.inFlight(), 0U);
    EXPECT_TRUE(pipeline.canAccept(Cycles(0)));
    pipeline.accept(2, 0, true, Cycles(0));
    EXPECT_TRUE(pipeline.hasReady(Cycles(10)));
}

TEST(ArrayPipeline, StopsAtMaximumInFlight)
{
    ArrayPipeline pipeline(Cycles(0), Cycles(0), 1);

    pipeline.accept(1, 0, true, Cycles(0));

    EXPECT_FALSE(pipeline.canAccept(Cycles(0)));
    ASSERT_TRUE(pipeline.hasReady(Cycles(0)));
    pipeline.takeReady(Cycles(0));
    EXPECT_TRUE(pipeline.canAccept(Cycles(0)));
}

TEST(ArrayPipeline, ConservesAcceptedTokens)
{
    ArrayPipeline pipeline(Cycles(2), Cycles(1), 3);

    pipeline.accept(9, 10, false, Cycles(0));
    pipeline.accept(9, 11, false, Cycles(1));
    pipeline.accept(9, 12, true, Cycles(2));

    std::vector<PipelineToken> produced;
    for (uint64_t cycle = 2; cycle <= 4; ++cycle) {
        ASSERT_TRUE(pipeline.hasReady(Cycles(cycle)));
        produced.push_back(pipeline.takeReady(Cycles(cycle)));
    }

    ASSERT_EQ(produced.size(), 3);
    EXPECT_EQ(produced[0].commandId, 9);
    EXPECT_EQ(produced[0].index, 10);
    EXPECT_EQ(produced[1].index, 11);
    EXPECT_EQ(produced[2].index, 12);
    EXPECT_TRUE(produced[2].last);
    EXPECT_EQ(pipeline.inFlight(), 0);
}

} // anonymous namespace
} // namespace gem5::sau
