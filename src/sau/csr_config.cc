#include "sau/csr_config.hh"

#include <limits>
#include <stdexcept>
#include <string>

#include "sau/command.hh"

namespace gem5::sau
{
namespace
{

constexpr unsigned BeatBytes = 32;

uint32_t
bits(uint64_t data, unsigned low, unsigned width)
{
    return static_cast<uint32_t>((data >> low) & ((uint64_t(1) << width) - 1));
}

uint32_t
checkedProduct(uint32_t lhs, uint32_t rhs, const char *description)
{
    const uint64_t product = static_cast<uint64_t>(lhs) * rhs;
    if (product > std::numeric_limits<uint32_t>::max()) {
        throw std::invalid_argument(description);
    }
    return static_cast<uint32_t>(product);
}

uint32_t
product3(uint32_t first, uint32_t second, uint32_t third,
         const char *description)
{
    return checkedProduct(checkedProduct(first, second, description), third,
                          description);
}

} // anonymous namespace

bool
SauCsrConfig::apply(const SauCsrWrite &write)
{
    if (!write.accepted) {
        return false;
    }
    if (write.operation > 0x3) {
        throw std::invalid_argument("SAU CSR operation must be two bits");
    }
    if ((write.address >> 4) != DeviceId) {
        throw std::invalid_argument(
            "accepted CSR write targets another device");
    }
    if (hasAppliedWrite && write.cycle < lastAppliedCycle) {
        throw std::invalid_argument("SAU CSR writes must be ordered by cycle");
    }
    if (startActive && write.cycle > startCycle) {
        // csr.sv clears start_reg on the next clock after a start pulse.
        startActive = false;
    }
    hasAppliedWrite = true;
    lastAppliedCycle = write.cycle;

    const uint8_t index = (write.address >> 1) & 0x7;
    const uint64_t data = write.data;
    switch (index) {
      case 0:
        horizontalAddress = bits(data, 0, 32);
        verticalAddress = bits(data, 32, 32);
        break;
      case 1:
        registerInput.validYStart = bits(data, 0, 6);
        registerInput.validYEnd = bits(data, 6, 6);
        registerInput.validXStart = bits(data, 12, 6);
        registerInput.validXEnd = bits(data, 18, 6);
        registerInput.cCycle = bits(data, 24, 8);
        registerInput.xBurst = bits(data, 32, 6);
        registerInput.yStep = bits(data, 38, 8);
        registerInput.yCycle = bits(data, 46, 6);
        registerInput.cStep = bits(data, 52, 8);
        registerInput.padding = bits(data, 60, 4);
        break;
      case 2:
        input.xStep = bits(data, 0, 8);
        input.xBurst = bits(data, 8, 6);
        input.yStep = bits(data, 14, 8);
        input.yBurst = bits(data, 22, 6);
        input.flowStep = bits(data, 32, 8);
        input.flowBurst = bits(data, 40, 6);
        input.instructionStep = bits(data, 46, 8);
        input.instructionBurst = bits(data, 54, 6);
        break;
      case 3:
        transMode = bits(data, 0, 2);
        registerMode = bits(data, 2, 2);
        reuseMode = bits(data, 4, 2);
        peWorkMode = bits(data, 6, 2);
        saFlowMode = bits(data, 8, 2);
        convKernal = bits(data, 10, 3);
        strideFlag = bits(data, 13, 1);
        shiftFlag = bits(data, 14, 1);
        cutbit = bits(data, 15, 5);
        flowLoopTimes = bits(data, 20, 6);
        biasAddress = bits(data, 32, 32);
        break;
      case 4:
        vertical.xStep = bits(data, 0, 8);
        vertical.xBurst = bits(data, 8, 6);
        vertical.yStep = bits(data, 14, 8);
        vertical.yCycle = bits(data, 22, 6);
        vertical.flowStep = bits(data, 32, 8);
        vertical.flowCycle = bits(data, 40, 8);
        vertical.instructionStep = bits(data, 48, 8);
        vertical.instructionCycle = bits(data, 56, 6);
        break;
      case 5:
        output.xStep = bits(data, 0, 8);
        output.yStep = bits(data, 8, 8);
        output.flowStep = bits(data, 16, 8);
        output.instructionStep = bits(data, 24, 8);
        output.xBurst = bits(data, 32, 6);
        output.yBurst = bits(data, 38, 6);
        output.flowBurst = bits(data, 44, 6);
        output.instructionBurst = bits(data, 50, 6);
        output.registerCCycle = bits(data, 56, 8);
        break;
      case 6: {
        output.registerXBurst = bits(data, 0, 6);
        output.registerYStep = bits(data, 6, 8);
        output.registerYCycle = bits(data, 14, 6);
        output.registerCStep = bits(data, 20, 8);
        outputAddress = bits(data, 32, 32);

        const bool nextStart =
            ((write.operation & 0x1) && (startActive || bits(data, 31, 1))) ||
            ((write.operation & 0x2) && startActive && bits(data, 31, 1));
        const bool commandStart = !startActive && nextStart;
        startActive = nextStart;
        if (commandStart) {
            startCycle = write.cycle;
        }
        return commandStart;
      }
      case 7:
        // csr.sv reports csr_ready low for index 7 and leaves state unchanged.
        return false;
    }
    return false;
}

DecodedSauCommand
SauCsrConfig::decode(uint64_t commandId) const
{
    if (transMode != 0x1 || reuseMode != 0x1) {
        throw std::invalid_argument(
            "unsupported SAU CSR mode: trans_mode=" +
            std::to_string(transMode) + ", reuse_mode=" +
            std::to_string(reuseMode) +
            "; supported mode is trans_mode=1, reuse_mode=1");
    }

    const uint32_t residentLoadBeats = checkedProduct(
        registerInput.xBurst, registerInput.yCycle,
        "SAU CSR resident-load beat count overflows");
    const uint32_t inputBeatsPerFlow = product3(
        input.xBurst, input.yBurst, input.instructionBurst,
        "SAU CSR input beat count overflows");
    const uint32_t outputBeats = product3(
        output.xBurst, output.yBurst, output.instructionBurst,
        "SAU CSR output beat count overflows");
    const uint32_t workItems = checkedProduct(
        inputBeatsPerFlow, flowLoopTimes,
        "SAU CSR work item count overflows");

    SauCommand command;
    command.id = commandId;
    command.operation = Operation::Gemm;
    command.precision = Precision::Int8;
    command.operandA = {
        horizontalAddress, residentLoadBeats, BeatBytes, 0, 0};
    command.operandB = {
        verticalAddress, inputBeatsPerFlow, BeatBytes, 0, 0};
    command.output = {outputAddress, outputBeats, BeatBytes, 0, 0};
    command.flowLoops = flowLoopTimes;
    command.instructionLoops = 1;
    command.workItems = workItems;
    command.scheduleInstructions = vertical.instructionCycle;
    command.operandBAddress = {
        true,
        vertical.xBurst,
        vertical.yCycle,
        vertical.flowCycle,
        vertical.instructionCycle,
        static_cast<uint32_t>(vertical.xStep) * BeatBytes,
        static_cast<uint32_t>(vertical.yStep) * BeatBytes,
        static_cast<uint32_t>(vertical.flowStep) * vertical.yStep * BeatBytes,
        convKernal == 0 ? BeatBytes :
            static_cast<uint32_t>(vertical.instructionStep) *
                vertical.flowStep * vertical.yStep * BeatBytes,
    };
    validateCommand(command, BeatBytes);

    return {command, TimingPolicy::derive(
                         command, transMode, reuseMode,
                         RtlTimingParameters{})};
}

std::vector<ReplayedSauCommand>
replayCsrWrites(const std::vector<SauCsrWrite> &writes)
{
    SauCsrConfig config;
    std::vector<ReplayedSauCommand> commands;
    for (const auto &write : writes) {
        if (!config.apply(write)) {
            continue;
        }
        const uint64_t id = commands.size() + 1;
        commands.push_back({write.cycle, config.decode(id)});
    }
    return commands;
}

} // namespace gem5::sau
