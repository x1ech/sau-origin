#include "sau/output_datapath.hh"

#include <stdexcept>

namespace gem5::sau
{

OutputRegisterFile::OutputRegisterFile(
    const SauOutputResourceConfig &config)
    : config(config)
{
    if (config.internalXBurst == 0 || config.internalXBurst > 64 ||
        config.internalYBurst == 0 || config.internalYBurst > 64 ||
        config.internalFlowBurst == 0 ||
        config.internalFlowBurst > 64 ||
        config.internalInstructionBurst == 0 ||
        config.internalInstructionBurst > 64) {
        throw std::invalid_argument(
            "invalid output-register internal counter dimensions");
    }
}

bool
OutputRegisterFile::endX() const
{
    return countX == config.internalXBurst - 1;
}

bool
OutputRegisterFile::endY() const
{
    return countY == config.internalYBurst - 1;
}

bool
OutputRegisterFile::endFlow() const
{
    return countFlow == config.internalFlowBurst - 1;
}

bool
OutputRegisterFile::endInstruction() const
{
    return countInstruction == config.internalInstructionBurst - 1;
}

WriteBeat256 &
OutputRegisterFile::entry(uint8_t logicalAddress)
{
    return logicalAddress & 0x80 ?
        half1[logicalAddress & 0x7f] : half0[logicalAddress & 0x7f];
}

const WriteBeat256 &
OutputRegisterFile::read(uint8_t logicalAddress) const
{
    return logicalAddress & 0x80 ?
        half1[logicalAddress & 0x7f] : half0[logicalAddress & 0x7f];
}

WriteBeat256
OutputRegisterFile::compute(const OutputVector32x16 &input) const
{
    const WriteBeat256 &prior = read(address);
    WriteBeat256 result;
    for (unsigned lane = 0; lane < BeatLanes; ++lane) {
        int16_t value = input.lanes[lane];
        if (config.accumulateExisting) {
            const int16_t oldValue =
                static_cast<int8_t>(prior.bytes[lane]);
            value = wrapAddInt16(value, oldValue);
        }
        result.bytes[lane] = static_cast<uint8_t>(satSigned8(value));
    }
    return result;
}

OutputRegisterUpdate
OutputRegisterFile::accept(const OutputVector32x16 &input)
{
    const bool last = endX() && endY() && endFlow() && endInstruction();
    const uint8_t acceptedAddress = address;
    WriteBeat256 data = compute(input);
    entry(acceptedAddress) = data;
    ++acceptedCount;
    if (last) {
        resultDone = true;
    }
    advancePointer();
    return OutputRegisterUpdate{acceptedAddress, data, last};
}

void
OutputRegisterFile::advancePointer()
{
    if (!endX()) {
        ++countX;
        address = static_cast<uint8_t>(
            address + config.internalXStep);
        return;
    }
    if (!endY()) {
        countX = 0;
        ++countY;
        yBase = static_cast<uint8_t>(
            yBase + config.internalYStep);
        address = yBase;
        return;
    }

    countX = 0;
    countY = 0;
    if (!endFlow()) {
        ++countFlow;
        flowBase = static_cast<uint8_t>(
            flowBase + config.internalFlowStep);
        yBase = flowBase;
        address = flowBase;
    } else if (!endInstruction()) {
        countFlow = 0;
        ++countInstruction;
        instructionBase = static_cast<uint8_t>(
            instructionBase + config.internalInstructionStep);
        flowBase = instructionBase;
        yBase = instructionBase;
        address = instructionBase;
    } else {
        countFlow = 0;
        countInstruction = 0;
        instructionBase = 0;
        flowBase = 0;
        yBase = 0;
        address = 0;
    }
}

void
OutputRegisterFile::reset()
{
    half0 = {};
    half1 = {};
    address = 0;
    yBase = 0;
    flowBase = 0;
    instructionBase = 0;
    countX = 0;
    countY = 0;
    countFlow = 0;
    countInstruction = 0;
    acceptedCount = 0;
    resultDone = false;
}

} // namespace gem5::sau
