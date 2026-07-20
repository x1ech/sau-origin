#include <gtest/gtest.h>

#include <stdexcept>

#include "sau_n/sau_tile_buffer.hh"

namespace gem5::sau_n
{
namespace
{

PipelineResolvedConfig
tileConfig(uint64_t h, uint64_t w, uint64_t outH, uint64_t outW)
{
    PipelineResolvedConfig config;
    config.name = "tile_mapping";
    config.im2col.name = "tile_mapping_im2col";
    config.im2col.n = 1;
    config.im2col.c = 1;
    config.im2col.h = h;
    config.im2col.w = w;
    config.im2col.outH = outH;
    config.im2col.outW = outW;
    config.im2col.kernelH = 3;
    config.im2col.kernelW = 3;
    config.im2col.strideH = 1;
    config.im2col.strideW = 1;
    config.im2col.dilationH = 1;
    config.im2col.dilationW = 1;
    config.outChannels = 1;
    return config;
}

TEST(SauSpatialTile, MapsPackedW5AndSplitW17)
{
    auto packed = tileConfig(4, 5, 4, 5);
    const auto packedDerived = validateAndDerive(packed);
    validateSpatialTileMapping(packed, packedDerived);
    ASSERT_EQ(packedDerived.expectedTiles, uint64_t{2});

    const auto first = spatialTileMetadata(packed, packedDerived, 0);
    EXPECT_EQ(first.spatialMask, uint16_t{0x7fff});
    EXPECT_EQ(first.validRows, uint64_t{15});
    EXPECT_EQ(first.rows[0].oh, uint64_t{0});
    EXPECT_EQ(first.rows[0].ow, uint64_t{0});
    EXPECT_EQ(first.rows[14].oh, uint64_t{2});
    EXPECT_EQ(first.rows[14].ow, uint64_t{4});

    const auto second = spatialTileMetadata(packed, packedDerived, 1);
    EXPECT_EQ(second.spatialMask, uint16_t{0x001f});
    EXPECT_EQ(second.validRows, uint64_t{5});
    EXPECT_EQ(second.rows[0].oh, uint64_t{3});
    EXPECT_FALSE(second.rows[5].valid);

    auto split = tileConfig(1, 17, 1, 17);
    const auto splitDerived = validateAndDerive(split);
    validateSpatialTileMapping(split, splitDerived);
    EXPECT_EQ(
        spatialTileMetadata(split, splitDerived, 0).spatialMask,
        uint16_t{0xffff});
    const auto splitTail = spatialTileMetadata(split, splitDerived, 1);
    EXPECT_EQ(splitTail.spatialMask, uint16_t{0x0001});
    EXPECT_EQ(splitTail.rows[0].ow, uint64_t{16});
}

TEST(SauSpatialTile, RejectsScatteredOutputShapeAndBadIndex)
{
    auto scattered = tileConfig(4, 5, 2, 3);
    const auto derived = validateAndDerive(scattered);
    EXPECT_THROW(
        validateSpatialTileMapping(scattered, derived),
        std::invalid_argument);
    EXPECT_THROW(
        spatialTileMetadata(scattered, derived, derived.expectedTiles),
        std::out_of_range);
}

TEST(SauTileBuffer, EnforcesMetadataMaskKOrderAndCapacity)
{
    auto config = tileConfig(3, 1, 3, 1);
    const auto derived = validateAndDerive(config);
    const auto metadata = spatialTileMetadata(config, derived, 0);
    SauTileBuffer buffer(derived.k);
    buffer.begin(metadata);

    FeedVector feed;
    feed.mask = metadata.spatialMask;
    for (uint64_t k = 0; k < derived.k; ++k) {
        buffer.push(k, feed);
    }
    EXPECT_TRUE(buffer.full());
    EXPECT_EQ(buffer.at(0), feed);
    EXPECT_THROW(buffer.push(derived.k, feed), std::overflow_error);

    buffer.clear();
    EXPECT_TRUE(buffer.empty());
    EXPECT_FALSE(buffer.active());
    EXPECT_THROW(buffer.at(0), std::out_of_range);

    buffer.begin(metadata);
    feed.mask = 1;
    EXPECT_THROW(buffer.push(0, feed), std::invalid_argument);
    feed.mask = metadata.spatialMask;
    EXPECT_THROW(buffer.push(1, feed), std::logic_error);
}

} // anonymous namespace
} // namespace gem5::sau_n
