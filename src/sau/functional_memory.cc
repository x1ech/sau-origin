#include "sau/functional_memory.hh"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <istream>
#include <sstream>

namespace gem5::sau
{

namespace
{

int
hexDigitValue(char digit)
{
    if (digit >= '0' && digit <= '9') {
        return digit - '0';
    }
    if (digit >= 'a' && digit <= 'f') {
        return digit - 'a' + 10;
    }
    if (digit >= 'A' && digit <= 'F') {
        return digit - 'A' + 10;
    }
    return -1;
}

[[noreturn]] void
imageError(std::string_view imageName, uint64_t lineNumber,
           const std::string &reason)
{
    std::ostringstream message;
    message << "SAU memory image " << imageName << " line " << lineNumber
            << ": " << reason;
    throw std::invalid_argument(message.str());
}

} // anonymous namespace

FunctionalMemory::FunctionalMemory(Addr base, uint64_t size, uint8_t fill)
    : rangeBase(base), rangeBytes(size), fill(fill)
{
    if (size == 0) {
        throw std::invalid_argument(
            "SAU functional memory range must be nonzero");
    }
    if (base + size < base) {
        throw std::invalid_argument(
            "SAU functional memory range wraps the address space");
    }
}

bool
FunctionalMemory::contains(Addr address, uint64_t size) const
{
    if (address < rangeBase || size > rangeBytes) {
        return false;
    }
    return address - rangeBase <= rangeBytes - size;
}

void
FunctionalMemory::checkAccess(Addr address, uint64_t size,
                              const char *operation) const
{
    if (!contains(address, size)) {
        std::ostringstream message;
        message << "SAU functional memory " << operation
                << " outside the declared range: address 0x" << std::hex
                << address << " size 0x" << size << " range [0x"
                << rangeBase << ", 0x" << rangeBase + rangeBytes << ")";
        throw std::invalid_argument(message.str());
    }
}

const uint8_t *
FunctionalMemory::peekPage(Addr address) const
{
    const auto page = pages.find(address / PageBytes);
    if (page == pages.end()) {
        return nullptr;
    }
    return page->second.data();
}

uint8_t *
FunctionalMemory::touchPage(Addr address)
{
    auto &page = pages[address / PageBytes];
    if (page.empty()) {
        page.assign(PageBytes, fill);
    }
    return page.data();
}

uint8_t
FunctionalMemory::readByte(Addr address) const
{
    checkAccess(address, 1, "read");
    const uint8_t *page = peekPage(address);
    if (!page) {
        return fill;
    }
    return page[address % PageBytes];
}

void
FunctionalMemory::writeByte(Addr address, uint8_t value)
{
    checkAccess(address, 1, "write");
    touchPage(address)[address % PageBytes] = value;
}

void
FunctionalMemory::read(Addr address, uint8_t *destination,
                       uint64_t size) const
{
    checkAccess(address, size, "read");
    uint64_t copied = 0;
    while (copied < size) {
        const Addr current = address + copied;
        const uint64_t pageOffset = current % PageBytes;
        const uint64_t chunk =
            std::min<uint64_t>(size - copied, PageBytes - pageOffset);
        const uint8_t *page = peekPage(current);
        if (page) {
            std::copy_n(page + pageOffset, chunk, destination + copied);
        } else {
            std::fill_n(destination + copied, chunk, fill);
        }
        copied += chunk;
    }
}

void
FunctionalMemory::write(Addr address, const uint8_t *source, uint64_t size)
{
    checkAccess(address, size, "write");
    uint64_t copied = 0;
    while (copied < size) {
        const Addr current = address + copied;
        const uint64_t pageOffset = current % PageBytes;
        const uint64_t chunk =
            std::min<uint64_t>(size - copied, PageBytes - pageOffset);
        std::copy_n(source + copied, chunk, touchPage(current) + pageOffset);
        copied += chunk;
    }
}

MemoryBeat256
FunctionalMemory::readBeat(Addr address) const
{
    MemoryBeat256 beat;
    read(address, beat.bytes.data(), beat.bytes.size());
    return beat;
}

void
FunctionalMemory::writeBeat(Addr address, const MemoryBeat256 &beat)
{
    write(address, beat.bytes.data(), beat.bytes.size());
}

uint64_t
FunctionalMemory::loadLittleEndianWordHex(std::istream &image,
                                          std::string_view imageName,
                                          Addr base, unsigned bytesPerLine)
{
    if (bytesPerLine == 0 || bytesPerLine > PageBytes) {
        throw std::invalid_argument(
            "SAU memory image word width must be 1..4096 bytes");
    }

    std::vector<uint8_t> word(bytesPerLine, 0);
    uint64_t lineNumber = 0;
    uint64_t wordIndex = 0;
    std::string line;
    while (std::getline(image, line)) {
        ++lineNumber;
        while (!line.empty() &&
               std::isspace(static_cast<unsigned char>(line.back()))) {
            line.pop_back();
        }
        uint64_t start = 0;
        while (start < line.size() &&
               std::isspace(static_cast<unsigned char>(line[start]))) {
            ++start;
        }
        if (start == line.size()) {
            continue;
        }
        if (line[start] == '@' ||
            line.compare(start, 2, "//") == 0) {
            imageError(imageName, lineNumber,
                       "$readmemh directives and comments are not part of "
                       "the image contract");
        }
        const uint64_t digits = line.size() - start;
        if (digits != 2ull * bytesPerLine) {
            std::ostringstream reason;
            reason << "expected " << 2ull * bytesPerLine
                   << " hex digits, found " << digits;
            imageError(imageName, lineNumber, reason.str());
        }
        // The line text is one little-endian word: its last two hex
        // digits are the lowest addressed byte.
        for (unsigned byte = 0; byte < bytesPerLine; ++byte) {
            const uint64_t position =
                start + 2ull * (bytesPerLine - 1 - byte);
            const int high = hexDigitValue(line[position]);
            const int low = hexDigitValue(line[position + 1]);
            if (high < 0 || low < 0) {
                imageError(imageName, lineNumber, "invalid hex digit");
            }
            word[byte] = static_cast<uint8_t>((high << 4) | low);
        }
        const Addr wordAddress = base + wordIndex * bytesPerLine;
        if (!contains(wordAddress, bytesPerLine)) {
            imageError(imageName, lineNumber,
                       "image word falls outside the declared memory "
                       "range");
        }
        write(wordAddress, word.data(), bytesPerLine);
        ++wordIndex;
    }
    if (image.bad()) {
        imageError(imageName, lineNumber, "stream read failure");
    }
    return wordIndex * bytesPerLine;
}

uint64_t
FunctionalMemory::loadLittleEndianWordHexFile(const std::string &path,
                                              Addr base,
                                              unsigned bytesPerLine)
{
    std::ifstream image(path);
    if (!image) {
        throw std::invalid_argument(
            "SAU memory image is not readable: " + path);
    }
    return loadLittleEndianWordHex(image, path, base, bytesPerLine);
}

std::vector<uint8_t>
FunctionalMemory::dumpRange(Addr address, uint64_t size) const
{
    std::vector<uint8_t> bytes(size, 0);
    read(address, bytes.data(), size);
    return bytes;
}

void
FunctionalMemory::writeByteHexFile(const std::string &path,
                                   const std::vector<uint8_t> &bytes)
{
    std::ofstream dump(path);
    if (!dump) {
        throw std::invalid_argument(
            "SAU memory dump is not writable: " + path);
    }
    static const char digits[] = "0123456789abcdef";
    for (const uint8_t byte : bytes) {
        dump << digits[byte >> 4] << digits[byte & 0xf] << '\n';
    }
    dump.flush();
    if (!dump) {
        throw std::invalid_argument(
            "SAU memory dump write failed: " + path);
    }
}

FunctionalMemory::CompareResult
FunctionalMemory::compareRange(Addr address, const uint8_t *expected,
                               uint64_t size) const
{
    const std::vector<uint8_t> actual = dumpRange(address, size);
    CompareResult result;
    for (uint64_t offset = 0; offset < size; ++offset) {
        if (actual[offset] == expected[offset]) {
            continue;
        }
        result.match = false;
        result.first.address = address + offset;
        result.first.expectedByte = expected[offset];
        result.first.actualByte = actual[offset];
        result.first.beatIndex = offset / BeatBytes;
        result.first.laneIndex = static_cast<unsigned>(offset % BeatBytes);
        break;
    }
    return result;
}

} // namespace gem5::sau
