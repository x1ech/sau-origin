#include "sau_n/im2col_trace.hh"

#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace gem5::sau_n
{
namespace
{

bool
validSha256(const std::string &value)
{
    if (value.size() != 64) {
        return false;
    }
    for (const char character : value) {
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

std::string
fixedHex(uint64_t value, unsigned digits)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << std::nouppercase << std::setw(digits)
           << std::setfill('0') << value;
    return stream.str();
}

uint16_t
validBits(const std::array<bool, ScratchpadBanks> &valid)
{
    uint16_t bits = 0;
    for (std::size_t bank = 0; bank < ScratchpadBanks; ++bank) {
        if (valid[bank]) {
            bits |= static_cast<uint16_t>(1U << bank);
        }
    }
    return bits;
}

std::string
feedDataHex(const FeedVector &feed)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << std::nouppercase << std::setfill('0');
    for (std::size_t lane = Im2ColLanes; lane > 0; --lane) {
        stream << std::setw(2) << static_cast<unsigned>(feed.data[lane - 1]);
    }
    return stream.str();
}

} // anonymous namespace

Im2ColTraceWriter::Im2ColTraceWriter(
    const std::string &path, const std::string &resolvedConfigSha256)
    : configSha256(resolvedConfigSha256)
{
    if (!validSha256(configSha256)) {
        throw std::invalid_argument(
            "resolved config SHA256 must contain 64 lowercase hex digits");
    }
    if (path.empty()) {
        return;
    }

    output.open(path, std::ios::out | std::ios::trunc);
    if (!output.is_open()) {
        throw std::runtime_error("failed to open Im2Col trace file: " + path);
    }
    for (std::size_t field = 0; field < TraceFields.size(); ++field) {
        if (field != 0) {
            output << ',';
        }
        output << TraceFields[field];
    }
    output << '\n';
}

bool
Im2ColTraceWriter::enabled() const
{
    return output.is_open();
}

void
Im2ColTraceWriter::emit(const Im2ColCycle &cycle)
{
    if (!enabled()) {
        return;
    }

    const uint16_t requestValid = validBits(cycle.request.valid);
    const uint16_t responseValid = validBits(cycle.response.valid);
    output << SchemaVersion << ',' << configSha256 << ',' << cycle.cycle
           << ',' << static_cast<unsigned>(cycle.state)
           << ',' << cycle.busy << ',' << cycle.done
           << ',' << static_cast<unsigned>(cycle.fifoCount)
           << ',' << static_cast<unsigned>(cycle.fifoReadPointer)
           << ',' << static_cast<unsigned>(cycle.fifoWritePointer)
           << ',' << fixedHex(requestValid, ReqValidHexDigits);

    for (std::size_t bank = 0; bank < ScratchpadBanks; ++bank) {
        const uint64_t address = cycle.request.valid[bank] ?
            cycle.request.address[bank] : 0;
        output << ',' << fixedHex(address, ReqAddrHexDigits);
    }
    output << ',' << fixedHex(responseValid, RespValidHexDigits);
    for (std::size_t bank = 0; bank < ScratchpadBanks; ++bank) {
        const uint64_t data = cycle.response.valid[bank] ?
            cycle.response.data[bank] : 0;
        output << ',' << fixedHex(data, RespDataHexDigits);
    }

    const FeedVector feed = cycle.feedValid ? cycle.feed : FeedVector{};
    output << ',' << cycle.feedValid << ',' << cycle.feedReady
           << ',' << feedDataHex(feed)
           << ',' << fixedHex(feed.mask, FeedMaskHexDigits) << '\n';
    if (!output.good()) {
        throw std::runtime_error("failed while writing Im2Col trace");
    }
    if (cycle.drained) {
        flush();
    }
}

void
Im2ColTraceWriter::flush()
{
    if (!enabled()) {
        return;
    }
    output.flush();
    if (!output.good()) {
        throw std::runtime_error("failed while flushing Im2Col trace");
    }
}

} // namespace gem5::sau_n
