#ifndef __SAU_N_BANKED_SCRATCHPAD_HH__
#define __SAU_N_BANKED_SCRATCHPAD_HH__

#include <array>
#include <cstddef>
#include <cstdint>

#include "sau_n/im2col_types.hh"

namespace gem5::sau_n
{

inline constexpr std::size_t ScratchpadBanks =
    static_cast<std::size_t>(SpBanks);
inline constexpr std::size_t ScratchpadRows =
    static_cast<std::size_t>(SpBankEntries);

struct SramRequest
{
    std::array<bool, ScratchpadBanks> valid{};
    std::array<uint16_t, ScratchpadBanks> address{};
};

struct SramResponse
{
    std::array<bool, ScratchpadBanks> valid{};
    std::array<uint8_t, ScratchpadBanks> data{};
};

class BankedScratchpad
{
  public:
    void clear();
    void write(uint64_t bank, uint64_t row, uint8_t value);
    uint8_t read(uint64_t bank, uint64_t row) const;

    void preload(const ResolvedConfig &config);
    SramResponse combinationalResponse(const SramRequest &request) const;

  private:
    using Bank = std::array<uint8_t, ScratchpadRows>;
    std::array<Bank, ScratchpadBanks> storage{};
};

} // namespace gem5::sau_n

#endif // __SAU_N_BANKED_SCRATCHPAD_HH__
