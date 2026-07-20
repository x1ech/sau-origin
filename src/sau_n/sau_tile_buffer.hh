#ifndef __SAU_N_SAU_TILE_BUFFER_HH__
#define __SAU_N_SAU_TILE_BUFFER_HH__

#include <array>
#include <cstdint>
#include <vector>

#include "sau_n/im2col_model.hh"
#include "sau_n/sau_types.hh"

namespace gem5::sau_n
{

struct SpatialCoordinate
{
    bool valid = false;
    uint64_t n = 0;
    uint64_t oh = 0;
    uint64_t ow = 0;
};

struct SauTileMetadata
{
    uint64_t index = 0;
    uint16_t spatialMask = 0;
    uint64_t validRows = 0;
    std::array<SpatialCoordinate, SauRows> rows{};
};

SauTileMetadata spatialTileMetadata(
    const PipelineResolvedConfig &config,
    const PipelineDerivedConfig &derived,
    uint64_t tileIndex);
void validateSpatialTileMapping(
    const PipelineResolvedConfig &config,
    const PipelineDerivedConfig &derived);

class SauTileBuffer
{
  public:
    explicit SauTileBuffer(uint64_t capacity);

    void begin(const SauTileMetadata &metadata);
    void push(uint64_t canonicalK, const FeedVector &feed);
    const FeedVector &at(uint64_t canonicalK) const;
    void clear();

    uint64_t capacity() const { return tileCapacity; }
    uint64_t count() const { return entries.size(); }
    bool empty() const { return entries.empty(); }
    bool full() const { return entries.size() == tileCapacity; }
    bool active() const { return hasMetadata; }
    const SauTileMetadata &metadata() const;

  private:
    uint64_t tileCapacity = 0;
    bool hasMetadata = false;
    SauTileMetadata activeMetadata{};
    std::vector<FeedVector> entries;
};

} // namespace gem5::sau_n

#endif // __SAU_N_SAU_TILE_BUFFER_HH__
