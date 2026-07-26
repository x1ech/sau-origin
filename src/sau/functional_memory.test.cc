#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "sau/functional_memory.hh"

namespace gem5::sau
{
namespace
{

constexpr Addr TestBase = 0x29120000;
constexpr uint64_t OneGiB = 1ull << 30;

TEST(FunctionalMemory, HolesReadFillValueWithoutAllocatingPages)
{
    FunctionalMemory memory(TestBase, OneGiB, 0xa5);

    EXPECT_EQ(memory.readByte(TestBase), 0xa5);
    EXPECT_EQ(memory.readByte(TestBase + OneGiB - 1), 0xa5);
    const MemoryBeat256 beat = memory.readBeat(TestBase + 0x1000);
    for (const uint8_t byte : beat.bytes) {
        EXPECT_EQ(byte, 0xa5);
    }
    // Declaring 1 GiB and reading holes must not allocate backing store.
    EXPECT_EQ(memory.allocatedPageCount(), 0u);
}

TEST(FunctionalMemory, WritesAllocateOnlyTheTouchedPages)
{
    FunctionalMemory memory(TestBase, OneGiB);

    memory.writeByte(TestBase + 5, 0x12);
    memory.writeByte(TestBase + 0x20000000, 0x34);
    EXPECT_EQ(memory.readByte(TestBase + 5), 0x12);
    EXPECT_EQ(memory.readByte(TestBase + 0x20000000), 0x34);
    // An untouched byte on an allocated page reads the fill value.
    EXPECT_EQ(memory.readByte(TestBase + 6), 0x00);
    EXPECT_EQ(memory.allocatedPageCount(), 2u);
}

TEST(FunctionalMemory, BeatRoundTripCrossesPageBoundaries)
{
    FunctionalMemory memory(TestBase, OneGiB);

    // 16 bytes on one page, 16 bytes on the next.
    const Addr crossing = TestBase + FunctionalMemory::PageBytes - 16;
    MemoryBeat256 beat;
    for (unsigned byte = 0; byte < BeatBytes; ++byte) {
        beat.bytes[byte] = static_cast<uint8_t>(byte + 1);
    }
    memory.writeBeat(crossing, beat);

    EXPECT_EQ(memory.readBeat(crossing), beat);
    EXPECT_EQ(memory.readByte(crossing + 15), 16);
    EXPECT_EQ(memory.readByte(crossing + 16), 17);
    EXPECT_EQ(memory.allocatedPageCount(), 2u);
}

TEST(FunctionalMemory, RejectsAccessesOutsideTheDeclaredRange)
{
    FunctionalMemory memory(TestBase, 0x100);

    EXPECT_THROW(memory.readByte(TestBase - 1), std::invalid_argument);
    EXPECT_THROW(memory.readByte(TestBase + 0x100), std::invalid_argument);
    EXPECT_THROW(memory.writeByte(TestBase + 0x100, 0),
                 std::invalid_argument);
    // A beat that starts inside but ends outside must be rejected.
    EXPECT_THROW(memory.readBeat(TestBase + 0x100 - 16),
                 std::invalid_argument);
    EXPECT_NO_THROW(memory.readBeat(TestBase + 0x100 - 32));
    EXPECT_THROW(FunctionalMemory(TestBase, 0), std::invalid_argument);
}

TEST(FunctionalMemory, LoadsSixteenByteLittleEndianWordLines)
{
    FunctionalMemory memory(TestBase, OneGiB);

    // The first two lines of the atbd_cutbit8 package image.  The last
    // two hex digits of a line are its lowest addressed byte.
    std::istringstream image(
        "f9cef7f21bed11e8e60ce000e9e829fa\n"
        "0032340c02280acdd222bafacffcf6fc\n");
    const uint64_t loaded =
        memory.loadLittleEndianWordHex(image, "test-image", TestBase, 16);

    EXPECT_EQ(loaded, 32u);
    EXPECT_EQ(memory.readByte(TestBase + 0), 0xfa);
    EXPECT_EQ(memory.readByte(TestBase + 1), 0x29);
    EXPECT_EQ(memory.readByte(TestBase + 15), 0xf9);
    EXPECT_EQ(memory.readByte(TestBase + 16), 0xfc);
    EXPECT_EQ(memory.readByte(TestBase + 31), 0x00);

    // The two 128-bit words form one 256-bit beat in ascending order,
    // matching the captured first sau_sram_rdata payload.
    const MemoryBeat256 beat = memory.readBeat(TestBase);
    EXPECT_EQ(beat.bytes[0], 0xfa);
    EXPECT_EQ(beat.bytes[16], 0xfc);
    EXPECT_EQ(beat.bytes[31], 0x00);
}

TEST(FunctionalMemory, LoadsOneByteOutputLines)
{
    FunctionalMemory memory(TestBase, 0x1000);

    // final_output_memory.hex format: one byte per line, ascending
    // addresses.  Blank lines are skipped without advancing the address.
    std::istringstream image("0e\nff\n\nf4\n");
    const uint64_t loaded = memory.loadLittleEndianWordHex(
        image, "output-image", TestBase + 0xc00, 1);

    EXPECT_EQ(loaded, 3u);
    EXPECT_EQ(memory.readByte(TestBase + 0xc00), 0x0e);
    EXPECT_EQ(memory.readByte(TestBase + 0xc01), 0xff);
    EXPECT_EQ(memory.readByte(TestBase + 0xc02), 0xf4);
}

TEST(FunctionalMemory, RejectsMalformedImages)
{
    FunctionalMemory memory(TestBase, 0x1000);

    std::istringstream oddDigits("abc\n");
    EXPECT_THROW(
        memory.loadLittleEndianWordHex(oddDigits, "image", TestBase, 2),
        std::invalid_argument);

    std::istringstream badDigit("zz\n");
    EXPECT_THROW(
        memory.loadLittleEndianWordHex(badDigit, "image", TestBase, 1),
        std::invalid_argument);

    std::istringstream directive("@1000\nff\n");
    EXPECT_THROW(
        memory.loadLittleEndianWordHex(directive, "image", TestBase, 1),
        std::invalid_argument);

    std::istringstream comment("// header\nff\n");
    EXPECT_THROW(
        memory.loadLittleEndianWordHex(comment, "image", TestBase, 1),
        std::invalid_argument);

    // 0x1000 one-byte words fit; one more falls outside the range.
    std::ostringstream oversized;
    for (unsigned line = 0; line < 0x1001; ++line) {
        oversized << "ab\n";
    }
    std::istringstream oversizedImage(oversized.str());
    EXPECT_THROW(
        memory.loadLittleEndianWordHex(oversizedImage, "image", TestBase, 1),
        std::invalid_argument);

    EXPECT_THROW(
        memory.loadLittleEndianWordHexFile("/nonexistent/image.hex",
                                           TestBase, 1),
        std::invalid_argument);
}

TEST(FunctionalMemory, DumpAndCompareReportBeatAndLane)
{
    FunctionalMemory memory(TestBase, 0x1000, 0x00);

    std::vector<uint8_t> expected(96, 0x00);
    expected[7] = 0x11;
    expected[69] = 0x22;
    memory.writeByte(TestBase + 7, 0x11);
    memory.writeByte(TestBase + 69, 0x22);

    EXPECT_EQ(memory.dumpRange(TestBase, 96), expected);
    EXPECT_TRUE(memory.compareRange(TestBase, expected.data(), 96).match);

    // Only the first mismatch is reported: address, both bytes, and the
    // 256-bit beat/lane counted from the range start.
    memory.writeByte(TestBase + 69, 0x33);
    memory.writeByte(TestBase + 70, 0x44);
    const auto result = memory.compareRange(TestBase, expected.data(), 96);
    ASSERT_FALSE(result.match);
    EXPECT_EQ(result.first.address, TestBase + 69);
    EXPECT_EQ(result.first.expectedByte, 0x22);
    EXPECT_EQ(result.first.actualByte, 0x33);
    EXPECT_EQ(result.first.beatIndex, 2u);
    EXPECT_EQ(result.first.laneIndex, 5u);
}

TEST(FunctionalMemory, ByteHexDumpRoundTripsThroughTheLoader)
{
    const std::string path = testing::TempDir() + "sau_dump_test.hex";
    const std::vector<uint8_t> bytes{0x0e, 0xff, 0x00, 0xa5};
    FunctionalMemory::writeByteHexFile(path, bytes);

    std::ifstream dump(path);
    ASSERT_TRUE(dump.good());
    std::stringstream contents;
    contents << dump.rdbuf();
    EXPECT_EQ(contents.str(), "0e\nff\n00\na5\n");

    // The dump format is the loader's one-byte-word format, so a dump
    // reloads byte-exactly at any base.
    FunctionalMemory memory(TestBase, 0x100);
    EXPECT_EQ(memory.loadLittleEndianWordHexFile(path, TestBase, 1), 4u);
    EXPECT_EQ(memory.dumpRange(TestBase, 4), bytes);

    EXPECT_THROW(
        FunctionalMemory::writeByteHexFile(
            testing::TempDir() + "missing_dir/sau_dump.hex", bytes),
        std::invalid_argument);
}

TEST(FunctionalMemory, CompareRangeSeesHolesAsFillValue)
{
    FunctionalMemory memory(TestBase, 0x1000, 0xa5);

    std::vector<uint8_t> expected(8, 0xa5);
    EXPECT_TRUE(memory.compareRange(TestBase, expected.data(), 8).match);
    EXPECT_EQ(memory.allocatedPageCount(), 0u);

    expected[3] = 0x00;
    const auto result = memory.compareRange(TestBase, expected.data(), 8);
    ASSERT_FALSE(result.match);
    EXPECT_EQ(result.first.address, TestBase + 3);
    EXPECT_EQ(result.first.expectedByte, 0x00);
    EXPECT_EQ(result.first.actualByte, 0xa5);
}

} // anonymous namespace
} // namespace gem5::sau
