#include "sau/csr_config.hh"

#include <limits>
#include <stdexcept>
#include <string>

#include "sau/command.hh"
#include "sau/resource_config.hh"

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

std::string
rawControlSummary(const SauCsrConfig &config)
{
    return "trans_mode=" + std::to_string(config.transMode) +
           ", register_mode=" + std::to_string(config.registerMode) +
           ", reuse_mode=" + std::to_string(config.reuseMode) +
           ", pe_work_mode=" + std::to_string(config.peWorkMode) +
           ", sa_flow_mode=" + std::to_string(config.saFlowMode) +
           ", conv_kernal=" + std::to_string(config.convKernal) +
           ", stride_flag=" + std::to_string(config.strideFlag ? 1 : 0) +
           ", shift_flag=" + std::to_string(config.shiftFlag ? 1 : 0) +
           ", cutbit=" + std::to_string(config.cutbit) +
           ", flow_loop_times=" + std::to_string(config.flowLoopTimes);
}

/**
 * Reject only combinations the Step 0 support-domain table classifies as
 * switches to operators outside the int8 GEMM stage.  Every other raw
 * value is RTL-executable and must decode losslessly.
 */
void
rejectOutOfStageOperatorSwitches(const SauCsrConfig &config)
{
    std::string switches;
    const auto add = [&switches](const char *reason) {
        if (!switches.empty()) {
            switches += "; ";
        }
        switches += reason;
    };
    if (config.peWorkMode != 0) {
        add("pe_work_mode selects a non-MATMUL operator "
            "(01 CONV, 10 TRANSPOSER, 11 ADD)");
    }
    if (config.shiftFlag) {
        add("shift_flag=1 selects the packed/shift 16-bit datapath");
    }
    if (config.convKernal != 0) {
        add("conv_kernal!=0 selects convolution/packing behavior");
    }
    if (config.strideFlag) {
        add("stride_flag=1 selects the stride path");
    }
    if (config.registerMode == 0x2) {
        add("register_mode=10 selects the depthwise/single-column path");
    }
    if (!switches.empty()) {
        throw std::invalid_argument(
            "SAU CSR configuration switches to an operator outside the "
            "int8 GEMM stage: " + switches + "; raw CSR control: " +
            rawControlSummary(config));
    }
}

/**
 * The strict per-tick timing chain is validated for T-ATBD/R-A with normal,
 * transpose, and retain flow, plus the frozen single-command T-ABTD/R-A
 * normal-flow boundary. Other ABTD flow modes remain unimplemented.
 * register_mode 00/01/11 share the RTL non-depthwise guard, so they select
 * the same structural path and are not a maturity boundary.
 */
bool
onTimingValidatedPath(const SauCsrConfig &config, std::string &reason)
{
    const auto add = [&reason](const std::string &part) {
        if (!reason.empty()) {
            reason += "; ";
        }
        reason += part;
    };
    const bool validatedTransposeFlow =
        (config.transMode == 0x1 && config.saFlowMode <= 0x2) ||
        (config.transMode == 0x2 && config.saFlowMode == 0x0);
    if (!validatedTransposeFlow) {
        add("trans_mode=" + std::to_string(config.transMode) +
            ", sa_flow_mode=" + std::to_string(config.saFlowMode) +
            " selects a transpose/flow path without a validated per-tick "
            "timing and payload chain");
    }
    if (config.reuseMode != 0x1) {
        add("reuse_mode=" + std::to_string(config.reuseMode) +
            " selects a reuse path without a validated per-tick "
            "timing chain");
    }
    return reason.empty();
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
    rejectOutOfStageOperatorSwitches(*this);

    const uint32_t residentRows = checkedProduct(
        registerInput.xBurst, registerInput.yCycle,
        "SAU CSR resident-load beat count overflows");
    const uint32_t residentLoadBeats = checkedProduct(
        residentRows, registerInput.cCycle,
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
    command.control.transMode = transMode;
    command.control.reuseMode = reuseMode;
    command.control.saFlowMode = saFlowMode;
    command.control.registerMode = registerMode;
    command.control.peWorkMode = peWorkMode;
    command.control.convKernal = convKernal;
    command.control.strideFlag = strideFlag;
    command.control.shiftFlag = shiftFlag;
    command.control.cutbit = cutbit;
    command.control.flowLoopTimes = flowLoopTimes;
    command.control.verticalAddress = verticalAddress;
    command.control.horizontalAddress = horizontalAddress;
    command.control.outputAddress = outputAddress;
    command.control.biasAddress = biasAddress;
    command.control.input = input;
    command.control.vertical = vertical;
    command.control.registerInput = registerInput;
    command.control.output = output;

    DecodedSauCommand decoded;
    decoded.command = command;

    const auto paths = selectRtlPaths(command.control);
    const std::string pathSummary = std::string("selected RTL path: ") +
        paths.transPath + "/" + paths.reusePath + "/" + paths.flowPath;

    std::string unimplemented;
    if (!onTimingValidatedPath(*this, unimplemented)) {
        decoded.maturity = ValidationMaturity::RtlLegalUnimplemented;
        decoded.maturityReason = unimplemented + "; " + pathSummary +
            "; raw CSR control: " + rawControlSummary(*this);
        return decoded;
    }
    try {
        validateCommand(command, BeatBytes);
    } catch (const std::invalid_argument &error) {
        // RTL-executable raw counter values (for example zero wrap
        // boundaries) outside the validated execution domain are legal
        // but unimplemented, never mislabeled illegal.
        decoded.maturity = ValidationMaturity::RtlLegalUnimplemented;
        decoded.maturityReason =
            std::string("raw counter shape is outside the validated "
                        "execution domain: ") + error.what() + "; " +
            pathSummary + "; raw CSR control: " + rawControlSummary(*this);
        return decoded;
    }
    decoded.maturity = ValidationMaturity::ResourceTimed;
    decoded.timingPolicy = TimingPolicy::derive(
        command, transMode, reuseMode, RtlTimingParameters{});
    return decoded;
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
