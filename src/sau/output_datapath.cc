#include "sau/output_datapath.hh"

#include <stdexcept>

namespace gem5::sau
{

ResultSerializer::ResultSerializer(const SauOutputResourceConfig &config)
    : transposedOrder(config.transposedOrder)
{}

void
ResultSerializer::accept(const OperandVector32x8 &row)
{
    if (!canAccept()) {
        throw std::logic_error(
            "result serializer cannot accept while its bank is full");
    }
    rows[inputCount] = row;
    ++inputCount;
    if (inputCount == BeatLanes) {
        ready = true;
    }
}

OutputVector32x16
ResultSerializer::output(unsigned index) const
{
    OutputVector32x16 result;
    for (unsigned lane = 0; lane < BeatLanes; ++lane) {
        const int8_t value = transposedOrder
                                 ? rows[BeatLanes - 1 - lane].lanes[index]
                                 : rows[index].lanes[lane];
        result.lanes[lane] = value;
    }
    return result;
}

SerializedResult
ResultSerializer::peek() const
{
    if (!ready) {
        throw std::logic_error(
            "result serializer output requested before 32 rows");
    }
    return SerializedResult{output(outputCount), outputCount == BeatLanes - 1};
}

SerializedResult
ResultSerializer::take()
{
    const SerializedResult result = peek();
    if (result.last) {
        inputCount = 0;
        outputCount = 0;
        ready = false;
    } else {
        ++outputCount;
    }
    return result;
}

void
ResultSerializer::reset()
{
    rows = {};
    inputCount = 0;
    outputCount = 0;
    ready = false;
}

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
    if (unloadActive || unloadDoneFlag) {
        throw std::logic_error("output-register update conflicts with unload");
    }
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

SauResidentAddressResourceConfig
OutputRegisterFile::unloadAddressConfig(Addr baseAddress) const
{
    SauResidentAddressResourceConfig result;
    result.baseAddress = baseAddress;
    result.xBurst = config.registerXBurst;
    result.yStep = config.registerYStep;
    result.yCycle = config.registerYCycle;
    result.cStep = config.registerCStep;
    result.cCycle = config.registerCCycle;
    return result;
}

void
OutputRegisterFile::startUnload(Addr baseAddress)
{
    if (!resultDone) {
        throw std::logic_error(
            "output-register unload started before accumulation completed");
    }
    if (unloadActive || unloadDoneFlag) {
        throw std::logic_error("output-register unload already launched");
    }
    unloadAddress.emplace(unloadAddressConfig(baseAddress));
    unloadAddressReg.reset();
    unloadDelay1.reset();
    unloadDelay2.reset();
    // register_addr first enters RUNNING, then registers its first valid
    // address. The enclosing d1/d2 pipeline adds two more edges.
    unloadLaunchDelay = 1;
    unloadActive = true;
}

std::optional<OutputRegisterUnload>
OutputRegisterFile::tickUnload()
{
    if (!unloadActive) {
        return std::nullopt;
    }

    // Every stage samples the pre-edge register snapshot.
    const auto nextDelay2 = unloadDelay1;
    const auto nextDelay1 = unloadAddressReg;
    std::optional<OutputRegisterUnload> nextAddressReg;

    if (unloadLaunchDelay != 0) {
        --unloadLaunchDelay;
    } else if (unloadAddress && !unloadAddress->done()) {
        const uint8_t logicalAddress =
            static_cast<uint8_t>(unloadAddress->writePointer());
        nextAddressReg =
            OutputRegisterUnload{unloadAddress->address(), logicalAddress,
                                 read(logicalAddress), unloadAddress->last()};
        unloadAddress->advance();
    }

    unloadAddressReg = nextAddressReg;
    unloadDelay1 = nextDelay1;
    unloadDelay2 = nextDelay2;

    if (unloadDelay2 && unloadDelay2->last) {
        unloadActive = false;
        unloadDoneFlag = true;
    }
    return unloadDelay2;
}

void
OutputRegisterFile::clearUnloadControl()
{
    unloadAddress.reset();
    unloadAddressReg.reset();
    unloadDelay1.reset();
    unloadDelay2.reset();
    unloadLaunchDelay = 0;
    unloadActive = false;
    unloadDoneFlag = false;
}

void
OutputRegisterFile::clearCompletion()
{
    if (unloadActive) {
        throw std::logic_error(
            "output-register completion cleared during unload");
    }
    resultDone = false;
    clearUnloadControl();
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
    clearUnloadControl();
}

} // namespace gem5::sau
