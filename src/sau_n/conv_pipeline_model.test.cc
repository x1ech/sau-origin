#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "sau_n/conv_pipeline_model.hh"

namespace gem5::sau_n
{
namespace
{

PipelineResolvedConfig
pipelineConfig()
{
    PipelineResolvedConfig config;
    config.name = "step5_n1_c2_h4_w5_oc3";
    config.im2col.name = "step5_n1_c2_h4_w5_oc3_im2col";
    config.im2col.n = 1;
    config.im2col.c = 2;
    config.im2col.h = 4;
    config.im2col.w = 5;
    config.im2col.outH = 4;
    config.im2col.outW = 5;
    config.im2col.kernelH = 3;
    config.im2col.kernelW = 3;
    config.im2col.strideH = 1;
    config.im2col.strideW = 1;
    config.im2col.dilationH = 1;
    config.im2col.dilationW = 1;
    config.im2col.padTop = 1;
    config.im2col.padLeft = 1;
    config.outChannels = 3;
    config.cutbit = 8;
    return config;
}

int32_t
sat24(int32_t accumulator, int32_t addend)
{
    const int64_t sum = static_cast<int64_t>(accumulator) + addend;
    return static_cast<int32_t>(std::clamp<int64_t>(
        sum, -(int64_t{1} << 23), (int64_t{1} << 23) - 1));
}

int8_t
directActivation(uint64_t n, uint64_t c, uint64_t h, uint64_t w)
{
    const uint64_t raw = (n * 97 + c * 31 + h * 7 + w + 1) % 256;
    return static_cast<int8_t>(
        raw < 128 ? static_cast<int16_t>(raw) :
        static_cast<int16_t>(raw) - 256);
}

int8_t
directWeight(uint64_t oc, uint64_t c, uint64_t kh, uint64_t kw)
{
    const uint64_t raw =
        (oc * 29 + c * 17 + kh * 5 + kw * 3 + 11) % 255;
    return static_cast<int8_t>(static_cast<int16_t>(raw) - 127);
}

std::vector<int8_t>
directOracle(const PipelineResolvedConfig &config)
{
    std::vector<int8_t> outputs;
    for (uint64_t n = 0; n < config.im2col.n; ++n) {
        for (uint64_t oc = 0; oc < config.outChannels; ++oc) {
            for (uint64_t oh = 0; oh < config.im2col.outH; ++oh) {
                for (uint64_t ow = 0; ow < config.im2col.outW; ++ow) {
                    int32_t accumulator = 0;
                    for (uint64_t c = 0; c < config.im2col.c; ++c) {
                        for (uint64_t kh = 0; kh < 3; ++kh) {
                            for (uint64_t kw = 0; kw < 3; ++kw) {
                                const int64_t ih = static_cast<int64_t>(
                                    oh * config.im2col.strideH +
                                    kh * config.im2col.dilationH) -
                                    static_cast<int64_t>(config.im2col.padTop);
                                const int64_t iw = static_cast<int64_t>(
                                    ow * config.im2col.strideW +
                                    kw * config.im2col.dilationW) -
                                    static_cast<int64_t>(
                                        config.im2col.padLeft);
                                int8_t activation = 0;
                                if (ih >= 0 && iw >= 0 &&
                                    ih < static_cast<int64_t>(
                                        config.im2col.h) &&
                                    iw < static_cast<int64_t>(
                                        config.im2col.w)) {
                                    activation = directActivation(
                                        n, c, static_cast<uint64_t>(ih),
                                        static_cast<uint64_t>(iw));
                                }
                                accumulator = sat24(
                                    accumulator,
                                    static_cast<int32_t>(activation) *
                                        directWeight(oc, c, kh, kw));
                            }
                        }
                    }
                    const int32_t bias = static_cast<int32_t>(
                        (oc * 37 + 13) % 257) - 128;
                    accumulator = sat24(accumulator, bias);
                    const int64_t divisor = int64_t{1} << config.cutbit;
                    const int64_t shifted = accumulator >= 0 ?
                        accumulator / divisor :
                        -((-static_cast<int64_t>(accumulator) +
                           divisor - 1) / divisor);
                    outputs.push_back(static_cast<int8_t>(
                        std::clamp<int64_t>(shifted, -128, 127)));
                }
            }
        }
    }
    return outputs;
}

const std::vector<int8_t> &
pythonOracleAnchor()
{
    static const std::vector<int8_t> values = {
        -58, -90, -94, -99, -69, -104, -128, -128, -128, -121,
        -128, -128, -128, -128, -128, -105, -128, -128, -128, -118,
        -39, -61, -64, -67, -47, -71, -111, -116, -121, -85,
        -94, -128, -128, -128, -108, -73, -114, -117, -121, -84,
        -20, -32, -34, -36, -26, -38, -61, -64, -67, -48,
        -51, -82, -84, -87, -62, -42, -66, -68, -70, -50,
    };
    return values;
}

uint64_t
runToDrained(
    ConvPipelineModel &model, bool *sawIm2ColBackpressure = nullptr,
    uint64_t *observedCollectTileCycles = nullptr)
{
    for (uint64_t attempts = 0; attempts < 200000; ++attempts) {
        const auto cycle = model.tick();
        if (observedCollectTileCycles &&
            cycle.state == PipelineState::CollectTile) {
            ++*observedCollectTileCycles;
        }
        if (sawIm2ColBackpressure && cycle.im2col.feedValid &&
            !cycle.im2col.feedReady) {
            *sawIm2ColBackpressure = true;
        }
        if (cycle.state == PipelineState::StreamK) {
            EXPECT_TRUE(cycle.saInputValid);
        }
        if (cycle.drained) {
            return cycle.cycle;
        }
    }
    throw std::runtime_error("pipeline did not drain");
}

TEST(ConvPipelineModel, OneTileHandComputedOutputAndConservation)
{
    auto config = pipelineConfig();
    config.name = "step5_hand";
    config.im2col.name = "step5_hand_im2col";
    config.im2col.c = 1;
    config.im2col.h = 3;
    config.im2col.w = 1;
    config.im2col.outH = 3;
    config.im2col.outW = 1;
    config.outChannels = 1;
    config.cutbit = 0;
    config.weightGenerator = "ones";
    config.biasGenerator = "zero";

    ConvPipelineModel model(config);
    uint64_t observedCollectTileCycles = 0;
    const uint64_t drained =
        runToDrained(model, nullptr, &observedCollectTileCycles);

    EXPECT_EQ(model.outputs(), (std::vector<int8_t>{9, 24, 23}));
    EXPECT_EQ(model.stats().tilesCollected, uint64_t{1});
    EXPECT_EQ(
        model.stats().collectTileCycles, observedCollectTileCycles);
    EXPECT_LT(model.stats().collectTileCycles, drained + 1);
    EXPECT_EQ(model.stats().tilesLaunched, uint64_t{1});
    EXPECT_EQ(model.stats().tilesCompleted, uint64_t{1});
    EXPECT_EQ(model.stats().activationHandshakes, uint64_t{9});
    EXPECT_EQ(model.stats().engineInputCycles, uint64_t{9});
    EXPECT_EQ(model.stats().usefulMacs, uint64_t{27});
    EXPECT_GT(model.stats().arrayActiveCycles, uint64_t{0});
    EXPECT_EQ(model.stats().tileBufferOccupancySamples, drained + 1);
    EXPECT_EQ(model.stats().tileBufferPeakOccupancy, uint64_t{9});
    EXPECT_EQ(model.stats().outputRows, uint64_t{3});
    EXPECT_EQ(model.stats().outputElements, uint64_t{3});
    ASSERT_TRUE(model.im2colDoneCycle());
    ASSERT_TRUE(model.sauLastResultCycle());
    ASSERT_TRUE(model.drainedCycle());
    EXPECT_EQ(*model.drainedCycle(), drained);
    EXPECT_LE(*model.im2colDoneCycle(), *model.drainedCycle());
    EXPECT_LE(*model.sauLastResultCycle(), *model.drainedCycle());
    EXPECT_THROW(model.tick(), std::logic_error);
}

TEST(ConvPipelineModel, TwoTilesMatchDirectOracleAndBackpressure)
{
    const auto config = pipelineConfig();
    const auto expected = directOracle(config);
    EXPECT_EQ(expected, pythonOracleAnchor());

    ConvPipelineModel readyModel(config);
    bool sawIm2ColBackpressure = false;
    const uint64_t readyDrained =
        runToDrained(readyModel, &sawIm2ColBackpressure);
    EXPECT_TRUE(sawIm2ColBackpressure);
    EXPECT_EQ(readyModel.outputs(), expected);
    EXPECT_EQ(readyModel.stats().positiveSaturations, uint64_t{0});
    EXPECT_EQ(readyModel.stats().negativeSaturations, uint64_t{14});

    ConvPipelineModel blockedModel(config, {11, 1});
    const uint64_t blockedDrained = runToDrained(blockedModel);
    EXPECT_EQ(blockedModel.outputs(), expected);
    EXPECT_GT(blockedDrained, readyDrained);
    EXPECT_GT(blockedModel.stats().outputBackpressureCycles, uint64_t{0});
    EXPECT_GT(blockedModel.stats().im2colBackpressureCycles, uint64_t{0});
    EXPECT_EQ(blockedModel.stats().tilesCompleted, uint64_t{2});
    EXPECT_EQ(blockedModel.stats().activationHandshakes, uint64_t{36});
    EXPECT_EQ(blockedModel.stats().engineInputCycles, uint64_t{36});
    EXPECT_EQ(blockedModel.stats().outputElements, uint64_t{60});
    EXPECT_EQ(blockedModel.stats().usefulMacs, uint64_t{1080});
}

TEST(ConvPipelineModel, CoversFrozenPackingSplittingBatchAndMaxKProfiles)
{
    std::vector<PipelineResolvedConfig> configs;

    auto full = pipelineConfig();
    full.name = "step5_full";
    full.im2col.name = "step5_full_im2col";
    full.im2col.c = 3;
    full.im2col.h = 3;
    full.im2col.w = 16;
    full.im2col.outH = 3;
    full.im2col.outW = 16;
    full.outChannels = 16;
    configs.push_back(full);

    auto split = pipelineConfig();
    split.name = "step5_split";
    split.im2col.name = "step5_split_im2col";
    split.im2col.c = 3;
    split.im2col.h = 5;
    split.im2col.w = 17;
    split.im2col.outH = 5;
    split.im2col.outW = 17;
    split.im2col.dilationH = 2;
    split.im2col.dilationW = 2;
    split.im2col.padTop = 2;
    split.im2col.padLeft = 2;
    split.outChannels = 7;
    split.cutbit = 4;
    configs.push_back(split);

    auto batch = pipelineConfig();
    batch.name = "step5_batch";
    batch.im2col.name = "step5_batch_im2col";
    batch.im2col.n = 2;
    batch.im2col.c = 4;
    batch.im2col.h = 5;
    batch.im2col.w = 20;
    batch.im2col.outH = 3;
    batch.im2col.outW = 10;
    batch.im2col.strideH = 2;
    batch.im2col.strideW = 2;
    batch.outChannels = 15;
    configs.push_back(batch);

    auto maximumK = pipelineConfig();
    maximumK.name = "step5_maxk";
    maximumK.im2col.name = "step5_maxk_im2col";
    maximumK.im2col.c = 63;
    maximumK.im2col.h = 3;
    maximumK.im2col.w = 1;
    maximumK.im2col.outH = 3;
    maximumK.im2col.outW = 1;
    maximumK.outChannels = 16;
    configs.push_back(maximumK);

    for (const auto &config : configs) {
        SCOPED_TRACE(config.name);
        ConvPipelineModel model(config);
        runToDrained(model);
        EXPECT_EQ(model.outputs(), directOracle(config));
        EXPECT_EQ(
            model.stats().tilesCompleted,
            model.derived().expectedTiles);
        EXPECT_EQ(
            model.stats().activationHandshakes,
            model.derived().expectedTiles * model.derived().k);
        EXPECT_EQ(
            model.stats().outputElements,
            model.derived().expectedOutputs);
        EXPECT_EQ(model.stats().usefulMacs, model.derived().expectedMacs);
    }
}

TEST(ConvPipelineModel, RejectsInvalidReadyAndScatteredTileShape)
{
    EXPECT_THROW(ConvPipelineModel(pipelineConfig(), {0, 1}),
                 std::invalid_argument);
    auto scattered = pipelineConfig();
    scattered.im2col.padTop = 0;
    scattered.im2col.padLeft = 0;
    scattered.im2col.outH = 2;
    scattered.im2col.outW = 3;
    EXPECT_THROW((void)ConvPipelineModel{scattered}, std::invalid_argument);
}

} // anonymous namespace
} // namespace gem5::sau_n
