#include "sau_n/conv_pipeline_io.hh"

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

void
writeFeedData(std::ostream &stream, const Im2ColCycle &cycle)
{
    stream << "0x" << std::hex << std::nouppercase << std::setfill('0');
    for (std::size_t lane = Im2ColLanes; lane > 0; --lane) {
        const uint8_t value = cycle.feedValid ? cycle.feed.data[lane - 1] : 0;
        stream << std::setw(2) << static_cast<unsigned>(value);
    }
    stream << std::dec;
}

void
writeInt8Lanes(
    std::ostream &stream,
    const SauNumericCore::Int8Lanes &lanes,
    bool valid)
{
    stream << "0x" << std::hex << std::nouppercase << std::setfill('0');
    for (std::size_t lane = SauRows; lane > 0; --lane) {
        const uint8_t value = valid ?
            static_cast<uint8_t>(lanes[lane - 1]) : 0;
        stream << std::setw(2) << static_cast<unsigned>(value);
    }
    stream << std::dec;
}

void
writeBiases(
    std::ostream &stream,
    const SauNumericCore::BiasLanes &biases,
    bool valid)
{
    stream << "0x" << std::hex << std::nouppercase << std::setfill('0');
    for (std::size_t column = SauColumns; column > 0; --column) {
        const uint16_t value = valid ?
            static_cast<uint16_t>(biases[column - 1]) : 0;
        stream << std::setw(4) << value;
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
    std::ostream &stream,
    const SauCycleObservation &cycle,
    bool activation)
{
    stream << "0x" << std::hex << std::nouppercase << std::setfill('0');
    for (std::size_t index = SauRows * SauColumns; index > 0; --index) {
        const std::size_t pe = index - 1;
        const bool valid = maskBit(cycle.peValidMask, pe);
        const int8_t signedValue = activation ?
            cycle.peStates[pe].activation : cycle.peStates[pe].weight;
        const uint8_t value = valid ?
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

void
writeOutputSlots(std::ostream &stream, const SauCycleObservation &cycle)
{
    stream << "0x" << std::hex << std::nouppercase << std::setfill('0');
    for (std::size_t column = SauColumns; column > 0; --column) {
        const uint16_t value = cycle.rowScoreValid ?
            cycle.outputSlots[column - 1] : 0;
        stream << std::setw(4) << value;
    }
    stream << std::dec;
}

uint16_t
prefixMask(uint64_t count)
{
    return count == SauRows ? uint16_t{0xffff} :
        static_cast<uint16_t>((uint32_t{1} << count) - 1);
}

} // anonymous namespace

ConvPipelineTraceWriter::ConvPipelineTraceWriter(
    const std::string &path, const std::string &resolvedConfigSha256)
    : enabled(!path.empty()), configSha256(resolvedConfigSha256)
{
    validateSha256(configSha256);
    if (!enabled) {
        return;
    }
    output.open(path, std::ios::out | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot open pipeline trace file: " + path);
    }
    for (std::size_t index = 0;
         index < CanonicalPipelineTraceFields.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        output << CanonicalPipelineTraceFields[index];
    }
    output << '\n';
}

void
ConvPipelineTraceWriter::emit(const ConvPipelineCycle &cycle)
{
    if (!enabled) {
        return;
    }
    output << 1 << ',' << configSha256 << ',' << cycle.cycle << ','
           << static_cast<unsigned>(cycle.state) << ',' << cycle.tileIndex
           << ',' << cycle.tileBufferCount << ',' << cycle.collectK << ','
           << cycle.streamK << ','
           << static_cast<unsigned>(cycle.im2col.state) << ','
           << cycle.im2col.done << ','
           << static_cast<unsigned>(cycle.im2col.fifoCount) << ','
           << static_cast<unsigned>(cycle.im2col.fifoReadPointer) << ','
           << static_cast<unsigned>(cycle.im2col.fifoWritePointer) << ','
           << cycle.im2col.feedValid << ',' << cycle.im2col.feedReady << ','
           << cycle.im2colFeedHandshake << ',';
    writeFeedData(output, cycle.im2col);
    output << ',';
    writeHex(
        output,
        cycle.im2col.feedValid ? cycle.im2col.feed.mask : uint16_t{0},
        FeedMaskHexDigits);
    output << ',' << cycle.sauInputs.insValid << ','
           << (cycle.sauInputs.insValid ?
               cycle.sauInputs.config.calcCycles : 0) << ','
           << (cycle.sauInputs.insValid ?
               cycle.sauInputs.config.validRows : 0) << ','
           << (cycle.sauInputs.insValid ?
               cycle.sauInputs.config.validColumns : 0) << ','
           << (cycle.sauInputs.insValid ?
               cycle.sauInputs.config.cutbit : 0) << ',';
    writeBiases(
        output, cycle.sauInputs.config.biases, cycle.sauInputs.insValid);
    output << ',' << static_cast<unsigned>(cycle.sau.state) << ','
           << cycle.sau.dataInCount << ',' << cycle.sau.outputCounter << ','
           << cycle.sauInputs.inputValid << ',';
    writeInt8Lanes(
        output, cycle.sauInputs.activations,
        cycle.sauInputs.inputValid);
    output << ',';
    writeInt8Lanes(
        output, cycle.sauInputs.weights, cycle.sauInputs.inputValid);
    output << ',';
    writeHex(
        output,
        cycle.sauInputs.inputValid ?
            prefixMask(cycle.sauInputs.config.validRows) : uint16_t{0},
        4);
    output << ',';
    writeHex(
        output,
        cycle.sauInputs.inputValid ?
            prefixMask(cycle.sauInputs.config.validColumns) : uint16_t{0},
        4);
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
    output << ',';
    writeHex(output, cycle.sau.osValidMask, 4);
    output << ',';
    writeHex(output, cycle.sau.rowReadyMask, 4);
    output << ',' << cycle.sau.peFinish << ','
           << cycle.sau.storageReady << ','
           << cycle.sauInputs.outputRequest << ','
           << cycle.sauInputs.outputGrant << ','
           << cycle.sau.internalOutputValid << ','
           << cycle.sau.engineOutputFire << ','
           << cycle.sau.rowScoreValid << ','
           << (cycle.sau.rowScoreValid ? cycle.sau.rowSequence : 0) << ',';
    writeOutputSlots(output, cycle.sau);
    output << ',' << cycle.sau.calFinish << ',' << cycle.outputCollected
           << ',' << cycle.sauLastResult << ',' << cycle.drained << '\n';
    if (cycle.drained) {
        output.flush();
    }
    if (!output) {
        throw std::runtime_error("failed while writing pipeline trace");
    }
}

void
writeConvPipelineOutput(
    const std::string &path,
    const PipelineResolvedConfig &config,
    const std::vector<int8_t> &values)
{
    if (path.empty()) {
        throw std::invalid_argument(
            "pipeline output file path must not be empty");
    }
    const auto derived = validateAndDerive(config);
    if (values.size() != derived.expectedOutputs) {
        throw std::invalid_argument(
            "pipeline output count disagrees with resolved configuration");
    }
    std::ofstream output(path, std::ios::out | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot open pipeline output file: " + path);
    }
    output << "n,oc,oh,ow,value\n";
    std::size_t index = 0;
    for (uint64_t n = 0; n < config.im2col.n; ++n) {
        for (uint64_t oc = 0; oc < config.outChannels; ++oc) {
            for (uint64_t oh = 0; oh < config.im2col.outH; ++oh) {
                for (uint64_t ow = 0; ow < config.im2col.outW; ++ow) {
                    output << n << ',' << oc << ',' << oh << ',' << ow << ','
                           << static_cast<int>(values[index++]) << '\n';
                }
            }
        }
    }
    if (!output) {
        throw std::runtime_error("failed while writing pipeline output file");
    }
}

} // namespace gem5::sau_n
