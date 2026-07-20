#include "sau/csr_fixture.hh"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace gem5::sau
{
namespace
{

uint64_t
parseInteger(const std::string &field, const char *name)
{
    size_t parsed = 0;
    const auto value = std::stoull(field, &parsed, 0);
    if (parsed != field.size()) {
        throw std::invalid_argument(std::string("invalid ") + name +
                                    " in csr_writes.csv");
    }
    return value;
}

std::vector<std::string>
splitCsv(const std::string &line)
{
    std::vector<std::string> fields;
    std::stringstream stream(line);
    std::string field;
    while (std::getline(stream, field, ',')) {
        fields.push_back(field);
    }
    return fields;
}

void
stripCarriageReturn(std::string &value)
{
    if (!value.empty() && value.back() == '\r') {
        value.pop_back();
    }
}

} // anonymous namespace

std::vector<ReplayedSauCommand>
loadCsrFixture(const std::string &fixtureDirectory,
               const RtlTimingParameters &rtl)
{
    const std::string path = fixtureDirectory + "/csr_writes.csv";
    std::ifstream input(path);
    if (!input.is_open()) {
        throw std::runtime_error("failed to open SAU CSR fixture: " + path);
    }

    std::string header;
    if (!std::getline(input, header)) {
        throw std::invalid_argument("unexpected SAU csr_writes.csv header");
    }
    stripCarriageReturn(header);
    if (header != "cycle,csr_addr,csr_operation,csr_wdata,accepted") {
        throw std::invalid_argument("unexpected SAU csr_writes.csv header");
    }

    std::vector<SauCsrWrite> writes;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        auto fields = splitCsv(line);
        if (fields.size() != 5) {
            throw std::invalid_argument("malformed SAU CSR fixture row");
        }
        for (auto &field : fields) {
            stripCarriageReturn(field);
        }
        writes.push_back({
            parseInteger(fields[0], "cycle"),
            static_cast<uint16_t>(parseInteger(fields[1], "csr_addr")),
            static_cast<uint8_t>(parseInteger(fields[2], "csr_operation")),
            parseInteger(fields[3], "csr_wdata"),
            parseInteger(fields[4], "accepted") != 0,
        });
    }

    auto replayed = replayCsrWrites(writes);
    std::vector<ReplayedSauCommand> commands;
    commands.reserve(replayed.size());
    for (auto &entry : replayed) {
        entry.decoded.timingPolicy = TimingPolicy::derive(
            entry.decoded.command, entry.decoded.timingPolicy.transMode,
            entry.decoded.timingPolicy.reuseMode, rtl);
        commands.push_back(std::move(entry));
    }
    if (commands.empty()) {
        throw std::invalid_argument("SAU CSR fixture contains no accepted start");
    }
    return commands;
}

} // namespace gem5::sau
