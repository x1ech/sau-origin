#ifndef __SAU_FUNCTIONAL_MEMORY_HH__
#define __SAU_FUNCTIONAL_MEMORY_HH__

#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "sau/data_beat.hh"
#include "sau/types.hh"

namespace gem5::sau
{

/**
 * PLAN3 Step 1 single functional data authority for strict fixed-SRAM
 * runs.
 *
 * The memory is byte addressable over one bounded address range and uses
 * sparse page-granular backing: declaring a 1 GiB range performs no eager
 * allocation, pages materialize only on first write or image load, and
 * reading an unallocated hole returns the contract fill value without
 * allocating.
 *
 * Out-of-range accesses and invalid image content throw
 * std::invalid_argument; a content difference found by compareRange() is
 * reported as data, not as an error.
 */
class FunctionalMemory
{
  public:
    static constexpr unsigned PageBytes = 4096;

    struct CompareMismatch
    {
        Addr address = 0;
        uint8_t expectedByte = 0;
        uint8_t actualByte = 0;
        /// 256-bit beat index counted from the compare-range start.
        uint64_t beatIndex = 0;
        /// Byte lane within that beat; lane k is address offset k.
        unsigned laneIndex = 0;
    };

    struct CompareResult
    {
        bool match = true;
        CompareMismatch first;
    };

    FunctionalMemory(Addr base, uint64_t size, uint8_t fill = 0);

    Addr baseAddress() const { return rangeBase; }
    uint64_t sizeBytes() const { return rangeBytes; }
    uint8_t fillValue() const { return fill; }

    /// True when [address, address + size) lies inside the memory range.
    bool contains(Addr address, uint64_t size) const;

    uint8_t readByte(Addr address) const;
    void writeByte(Addr address, uint8_t value);
    void read(Addr address, uint8_t *destination, uint64_t size) const;
    void write(Addr address, const uint8_t *source, uint64_t size);

    MemoryBeat256 readBeat(Addr address) const;
    void writeBeat(Addr address, const MemoryBeat256 &beat);

    /**
     * Load an RTL hex image.  Each non-blank line holds exactly one
     * little-endian word of bytesPerLine bytes: the last two hex digits
     * are the lowest addressed byte.  Line i is placed at
     * base + i * bytesPerLine, counting only non-blank lines.
     *
     * This covers both package formats: initial_memory.hex uses 16-byte
     * words and final_output_memory.hex uses 1-byte words.  $readmemh
     * '@' address directives and comments are rejected explicitly.
     *
     * Returns the number of bytes loaded.  imageName only labels error
     * messages.
     */
    uint64_t loadLittleEndianWordHex(std::istream &image,
                                     std::string_view imageName,
                                     Addr base, unsigned bytesPerLine);
    uint64_t loadLittleEndianWordHexFile(const std::string &path,
                                         Addr base, unsigned bytesPerLine);

    /// Copy of [address, address + size); holes read the fill value.
    std::vector<uint8_t> dumpRange(Addr address, uint64_t size) const;

    /// Byte compare of [address, address + size) against expected data,
    /// reporting the first differing address and its beat/lane.
    CompareResult compareRange(Addr address, const uint8_t *expected,
                               uint64_t size) const;

    uint64_t allocatedPageCount() const { return pages.size(); }

    /**
     * Shared final-memory dump writer for both data authorities: one
     * byte per line as two lowercase hex digits, ascending addresses.
     * This is exactly the golden final_output_memory.hex format, so a
     * dump can be compared against a package file directly.
     */
    static void writeByteHexFile(const std::string &path,
                                 const std::vector<uint8_t> &bytes);

  private:
    void checkAccess(Addr address, uint64_t size,
                     const char *operation) const;
    /// Backing bytes for the page containing address, or nullptr when
    /// the page is an unallocated hole.
    const uint8_t *peekPage(Addr address) const;
    /// Backing bytes for the page containing address, allocating and
    /// fill-initializing it on first use.
    uint8_t *touchPage(Addr address);

    const Addr rangeBase;
    const uint64_t rangeBytes;
    const uint8_t fill;
    std::unordered_map<uint64_t, std::vector<uint8_t>> pages;
};

} // namespace gem5::sau

#endif // __SAU_FUNCTIONAL_MEMORY_HH__
