#include "sau_n/sau_tile_buffer.hh"

#include <limits>
#include <stdexcept>

namespace gem5::sau_n
{
namespace
{

uint64_t
maskPopulation(uint16_t mask)
{
    uint64_t count = 0;
    for (uint64_t lane = 0; lane < SauRows; ++lane) {
        count += (mask >> lane) & uint16_t{1};
    }
    return count;
}

bool
isPrefixMask(uint16_t mask)
{
    const uint64_t count = maskPopulation(mask);
    const uint16_t expected = count == SauRows ?
        std::numeric_limits<uint16_t>::max() :
        static_cast<uint16_t>((uint32_t{1} << count) - 1);
    return mask == expected;
}

} // anonymous namespace

SauTileMetadata
spatialTileMetadata(
    const PipelineResolvedConfig &config,
    const PipelineDerivedConfig &derived,
    uint64_t tileIndex)
{
    if (tileIndex >= derived.expectedTiles) {
        throw std::out_of_range("tile index exceeds expected tile count");
    }
    const uint64_t tilesPerBatch = checkedMultiply(
        derived.im2col.hGroups, derived.im2col.wGroups,
        "tiles per batch");
    const uint64_t n = tileIndex / tilesPerBatch;
    const uint64_t localTile = tileIndex % tilesPerBatch;
    const uint64_t hGroup = localTile / derived.im2col.wGroups;
    const uint64_t wGroup = localTile % derived.im2col.wGroups;

    SauTileMetadata metadata;
    metadata.index = tileIndex;
    for (uint64_t lane = 0; lane < SauRows; ++lane) {
        uint64_t oh = 0;
        uint64_t ow = 0;
        bool valid = false;
        if (config.im2col.w <= SauRows) {
            const uint64_t localH = lane / config.im2col.w;
            oh = checkedAdd(
                checkedMultiply(
                    hGroup, derived.im2col.rowsPerWord,
                    "tile output row"),
                localH, "tile output row");
            ow = lane % config.im2col.w;
            valid = localH < derived.im2col.rowsPerWord &&
                oh < config.im2col.outH && ow < config.im2col.outW;
        } else {
            oh = hGroup;
            ow = checkedAdd(
                checkedMultiply(wGroup, SauRows, "tile output column"),
                lane, "tile output column");
            valid = oh < config.im2col.outH && ow < config.im2col.outW;
        }
        if (valid) {
            metadata.rows[lane] = {true, n, oh, ow};
            metadata.spatialMask |= static_cast<uint16_t>(uint16_t{1} << lane);
            ++metadata.validRows;
        }
    }
    if (metadata.validRows == 0 || !isPrefixMask(metadata.spatialMask)) {
        throw std::invalid_argument(
            "SA spatial tile mask must be a nonempty canonical prefix");
    }
    return metadata;
}

void
validateSpatialTileMapping(
    const PipelineResolvedConfig &config,
    const PipelineDerivedConfig &derived)
{
    uint64_t positions = 0;
    for (uint64_t tile = 0; tile < derived.expectedTiles; ++tile) {
        positions = checkedAdd(
            positions,
            spatialTileMetadata(config, derived, tile).validRows,
            "mapped spatial position count");
    }
    const uint64_t expected = checkedMultiply(
        checkedMultiply(
            config.im2col.n, config.im2col.outH,
            "expected spatial position count"),
        config.im2col.outW, "expected spatial position count");
    if (positions != expected) {
        throw std::invalid_argument(
            "SA spatial tile mapping does not cover every output position");
    }
}

SauTileBuffer::SauTileBuffer(uint64_t capacity) : tileCapacity(capacity)
{
    if (capacity == 0 || capacity > SauMaxChannels * 9) {
        throw std::invalid_argument(
            "tile buffer capacity must be in [1, 567]");
    }
    entries.reserve(static_cast<std::size_t>(capacity));
}

void
SauTileBuffer::begin(const SauTileMetadata &metadata)
{
    if (hasMetadata || !entries.empty()) {
        throw std::logic_error("cannot begin an active tile buffer");
    }
    if (metadata.validRows == 0 ||
        metadata.validRows != maskPopulation(metadata.spatialMask) ||
        !isPrefixMask(metadata.spatialMask)) {
        throw std::invalid_argument("invalid tile metadata spatial mask");
    }
    activeMetadata = metadata;
    hasMetadata = true;
}

void
SauTileBuffer::push(uint64_t canonicalK, const FeedVector &feed)
{
    if (!hasMetadata) {
        throw std::logic_error("tile metadata must be installed before push");
    }
    if (canonicalK != entries.size()) {
        throw std::logic_error(
            "tile entries must arrive in canonical K order");
    }
    if (full()) {
        throw std::overflow_error("tile buffer is full");
    }
    if (feed.mask != activeMetadata.spatialMask) {
        throw std::invalid_argument(
            "feed spatial mask disagrees with active tile metadata");
    }
    entries.push_back(feed);
}

const FeedVector &
SauTileBuffer::at(uint64_t canonicalK) const
{
    if (canonicalK >= entries.size()) {
        throw std::out_of_range("tile buffer K index is not available");
    }
    return entries[static_cast<std::size_t>(canonicalK)];
}

void
SauTileBuffer::clear()
{
    entries.clear();
    activeMetadata = {};
    hasMetadata = false;
}

const SauTileMetadata &
SauTileBuffer::metadata() const
{
    if (!hasMetadata) {
        throw std::logic_error("tile buffer has no active metadata");
    }
    return activeMetadata;
}

} // namespace gem5::sau_n
