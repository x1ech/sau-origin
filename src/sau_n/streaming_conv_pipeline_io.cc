#include "sau_n/streaming_conv_pipeline_io.hh"

#include <iomanip>
#include <stdexcept>

namespace gem5::sau_n
{
namespace
{

void
validateSha256(const std::string &value)
{
    if (value.size() != 64) {
        throw std::invalid_argument(
            "resolved configuration SHA256 must contain 64 characters");
    }
    for (const char character : value) {
        const bool decimal = character >= '0' && character <= '9';
        const bool lowerHex = character >= 'a' && character <= 'f';
        if (!decimal && !lowerHex) {
            throw std::invalid_argument(
                "resolved configuration SHA256 must be lowercase hex");
        }
    }
}

template <typename T>
void
writeHex(std::ostream &stream, T value, unsigned width)
{
    stream << "0x" << std::hex << std::nouppercase << std::setfill('0')
           << std::setw(width) << static_cast<uint64_t>(value) << std::dec;
}

uint16_t
laneDoneMask(const StreamingS1Payload &payload)
{
    uint16_t mask = 0;
    for (uint64_t lane = 0; lane < SauRows; ++lane) {
        if (payload.laneDone[lane]) {
            mask |= uint16_t{1} << lane;
        }
    }
    return mask;
}

uint16_t
requestValidMask(const SramRequest &request)
{
    uint16_t mask = 0;
    for (uint64_t bank = 0; bank < SpBanks; ++bank) {
        if (request.valid[bank]) {
            mask |= uint16_t{1} << bank;
        }
    }
    return mask;
}

uint16_t
responseValidMask(const SramResponse &response)
{
    uint16_t mask = 0;
    for (uint64_t bank = 0; bank < SpBanks; ++bank) {
        if (response.valid[bank]) {
            mask |= uint16_t{1} << bank;
        }
    }
    return mask;
}

void
writeRequestRows(std::ostream &stream, const SramRequest &request)
{
    stream << "0x" << std::hex << std::nouppercase << std::setfill('0');
    for (std::size_t bank = SpBanks; bank > 0; --bank) {
        const std::size_t index = bank - 1;
        const uint16_t row = request.valid[index] ?
            request.address[index] : 0;
        stream << std::setw(3) << row;
    }
    stream << std::dec;
}

void
writeSourceLanes(
    std::ostream &stream, const StreamingS2Payload &payload, bool valid)
{
    stream << "0x" << std::hex << std::nouppercase << std::setfill('0');
    for (std::size_t row = SauRows; row > 0; --row) {
        const std::size_t index = row - 1;
        const uint8_t source = valid &&
                payload.compacted.coordinates[index].valid ?
            payload.compacted.sourceLanes[index] : 0;
        stream << std::setw(2) << static_cast<unsigned>(source);
    }
    stream << std::dec;
}

void
writePeMask(std::ostream &stream, const SauPeMask &mask)
{
    stream << "0x" << std::hex << std::nouppercase << std::setfill('0');
    for (std::size_t word = mask.size(); word > 0; --word) {
        stream << std::setw(16) << mask[word - 1];
    }
    stream << std::dec;
}

bool
maskBit(const SauPeMask &mask, std::size_t index)
{
    return (mask[index / 64] & (uint64_t{1} << (index % 64))) != 0;
}

void
writePeInt8(
    std::ostream &stream, const SauCycleObservation &cycle,
    bool activation)
{
    stream << "0x" << std::hex << std::nouppercase << std::setfill('0');
    for (std::size_t index = SauRows * SauColumns; index > 0; --index) {
        const std::size_t pe = index - 1;
        const int8_t signedValue = activation ?
            cycle.peStates[pe].activation : cycle.peStates[pe].weight;
        const uint8_t value = maskBit(cycle.peValidMask, pe) ?
            static_cast<uint8_t>(signedValue) : 0;
        stream << std::setw(2) << static_cast<unsigned>(value);
    }
    stream << std::dec;
}

void
writePeAccumulators(
    std::ostream &stream, const SauCycleObservation &cycle)
{
    stream << "0x" << std::hex << std::nouppercase << std::setfill('0');
    for (std::size_t index = SauRows * SauColumns; index > 0; --index) {
        const std::size_t pe = index - 1;
        const bool valid = maskBit(cycle.peValidMask, pe) ||
            maskBit(cycle.addCommitMask, pe);
        const uint32_t value = valid ?
            static_cast<uint32_t>(cycle.peStates[pe].accumulator) &
                0x00ffffffU : 0;
        stream << std::setw(6) << value;
    }
    stream << std::dec;
}

} // anonymous namespace

StreamingConvPipelineTraceWriter::StreamingConvPipelineTraceWriter(
    const std::string &path, const std::string &resolvedConfigSha256,
    bool detailedPeTrace)
    : enabled(!path.empty()), detailed(detailedPeTrace),
      configSha256(resolvedConfigSha256)
{
    validateSha256(configSha256);
    if (!enabled) {
        return;
    }
    output.open(path, std::ios::out | std::ios::trunc);
    if (!output) {
        throw std::runtime_error(
            "cannot open streaming pipeline trace file: " + path);
    }
    bool first = true;
    auto writeField = [&](std::string_view field) {
        if (!first) {
            output << ',';
        }
        first = false;
        output << field;
    };
    for (const auto field : StreamingPipelineTraceFields) {
        writeField(field);
    }
    if (detailed) {
        for (const auto field : StreamingPipelineDetailedTraceFields) {
            writeField(field);
        }
    }
    output << '\n';
}

void
StreamingConvPipelineTraceWriter::emit(
    const StreamingConvPipelineCycle &cycle)
{
    if (!enabled) {
        return;
    }
    const auto &producer = cycle.producer;
    const bool aRequestValid = requestValidMask(producer.request) != 0;
    const bool aResponseValid =
        responseValidMask(producer.response) != 0;
    const auto &aRequestTag =
        producer.s0Fire ? producer.s0.tag : producer.s1.tag;
    output << 2 << ',' << configSha256 << ',' << cycle.cycle << ','
           << producer.s0Valid << ',' << producer.s0Ready << ','
           << producer.s0Fire << ','
           << (producer.s0Valid ? producer.s0.tag.tileIndex : 0) << ','
           << (producer.s0Valid ? producer.s0.tag.kIndex : 0) << ','
           << producer.s1Valid << ',' << producer.s1CanRetire << ','
           << producer.s1Ready << ',' << producer.s1Fire << ','
           << (producer.s1Valid ? producer.s1.tag.tileIndex : 0) << ','
           << (producer.s1Valid ? producer.s1.tag.kIndex : 0) << ',';
    writeHex(output, producer.s1Valid ? laneDoneMask(producer.s1) : 0, 4);
    output << ',';
    writeHex(output, requestValidMask(producer.request), 4);
    output << ',';
    writeRequestRows(output, producer.request);
    output << ','
           << (producer.s1Valid ? producer.s1.completedReadRounds : 0)
           << ',';
    writeHex(
        output, producer.s1Valid ? producer.s1.rawSpatialMask : 0, 4);
    output << ',' << producer.s2Valid << ',' << producer.s2Ready << ','
           << producer.s2Fire << ','
           << (producer.s2Valid ? producer.s2.tag.tileIndex : 0) << ','
           << (producer.s2Valid ? producer.s2.tag.kIndex : 0) << ',';
    writeHex(
        output,
        producer.s2Valid ? producer.s2.compacted.spatialMask : 0, 4);
    output << ',';
    writeSourceLanes(output, producer.s2, producer.s2Valid);
    output << ',' << producer.producerFire << ','
           << producer.producerExhausted << ',' << cycle.fifoCount << ','
           << cycle.fifoReadPointer << ',' << cycle.fifoWritePointer << ','
           << cycle.fifoPushReady << ',' << cycle.fifoPush << ','
           << cycle.fifoPop << ',' << cycle.fifoHeadValid << ','
           << (cycle.fifoHeadValid ? cycle.fifoHead.tag.tileIndex : 0)
           << ','
           << (cycle.fifoHeadValid ? cycle.fifoHead.tag.kIndex : 0) << ','
           << static_cast<unsigned>(cycle.consumerState) << ','
           << cycle.activeTile << ',' << cycle.acceptedK << ','
           << cycle.consumer.peReady << ',' << cycle.consumer.beginLaunch
           << ',' << cycle.consumer.launch << ','
           << cycle.consumer.inputValid << ',' << cycle.consumer.inputFire
           << ',' << static_cast<unsigned>(cycle.sau.state) << ','
           << cycle.sauInputs.inputValid << ','
           << cycle.sauInputs.outputGrant << ',' << cycle.sau.storageReady
           << ',' << cycle.sau.rowScoreValid << ','
           << (cycle.sau.rowScoreValid ? cycle.sau.rowSequence : 0) << ','
           << cycle.sau.calFinish << ',' << cycle.outputCollected << ','
           << cycle.drained << ',';
    writeHex(output, requestValidMask(producer.request), 4);
    output << ',';
    writeHex(output, requestValidMask(producer.grant), 4);
    output << ',';
    writeHex(output, responseValidMask(producer.response), 4);
    output << ','
           << (aRequestValid ? aRequestTag.tileIndex : 0) << ','
           << (aRequestValid ? aRequestTag.kIndex : 0) << ','
           << (aResponseValid ? producer.s1.tag.tileIndex : 0) << ','
           << (aResponseValid ? producer.s1.tag.kIndex : 0) << ',';
    writeHex(output, requestValidMask(cycle.bRequest), 4);
    output << ',';
    writeHex(output, requestValidMask(cycle.bGrant), 4);
    output << ',';
    writeHex(output, responseValidMask(cycle.bResponse), 4);
    output << ',' << cycle.bRequestBuffer << ',' << cycle.bRequestSlot
           << ',' << cycle.bRequestK << ',' << cycle.bResponseBuffer
           << ',' << cycle.bResponseSlot << ',' << cycle.bResponseK
           << ',';
    writeHex(output, requestValidMask(cycle.cRequest), 4);
    output << ',';
    writeHex(output, requestValidMask(cycle.cGrant), 4);
    output << ',';
    writeHex(output, responseValidMask(cycle.cResponse), 4);
    output << ',' << cycle.cRequestByte << ',' << cycle.cResponseByte
           << ',' << cycle.dQueueOccupancy << ',';
    writeHex(output, cycle.dHeadPendingMask, 4);
    output << ',';
    writeHex(output, requestValidMask(cycle.dRequest), 4);
    output << ',';
    writeHex(output, requestValidMask(cycle.dGrant), 4);
    output << ',' << cycle.dHeadWillRetire << ',' << cycle.dEnqueue
           << ',' << cycle.dDequeue << ',' << cycle.bEntryHit << ','
           << cycle.bReuseHit << ',' << cycle.activeBBuffer << ','
           << cycle.nextExpectedK << ',' << cycle.bReadyEntries;
    if (detailed) {
        output << ',';
        writePeMask(output, cycle.sau.peValidMask);
        output << ',';
        writePeMask(output, cycle.sau.macCommitMask);
        output << ',';
        writePeMask(output, cycle.sau.addCommitMask);
        output << ',';
        writePeInt8(output, cycle.sau, true);
        output << ',';
        writePeInt8(output, cycle.sau, false);
        output << ',';
        writePeAccumulators(output, cycle.sau);
    }
    output << '\n';
    if (cycle.drained) {
        output.flush();
    }
    if (!output) {
        throw std::runtime_error(
            "failed while writing streaming pipeline trace");
    }
}

} // namespace gem5::sau_n
