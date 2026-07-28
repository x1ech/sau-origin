#include "sau/a_register_file.hh"

#include <stdexcept>

#include "sau/address_program.hh"
#include "sau/resource_config.hh"

namespace gem5::sau
{
namespace
{

bool
matchesResidentAddress(const SauCommand &command, const Beat &beat)
{
    const auto &raw = command.control.registerInput;
    if (command.instructionLoops != 1 || raw.xBurst == 0 ||
        raw.yCycle == 0 || raw.cCycle == 0) {
        return false;
    }

    RtlResidentAddressProgram program(
        deriveResourceConfigs(command.control).residentAddress);
    for (uint32_t index = 0; index < beat.index; ++index) {
        if (program.done()) {
            return false;
        }
        program.advance();
    }
    return !program.done() && beat.address == program.address();
}

} // anonymous namespace

ARegisterFileIn::ARegisterFileIn(
    const SauCommand &command, uint32_t totalArrayInputs)
    : command(command),
      arrayInputs(totalArrayInputs == 0 ? command.workItems :
                                            totalArrayInputs),
      loaded(static_cast<size_t>(command.operandA.beats) *
             command.instructionLoops, false)
{
}

void
ARegisterFileIn::load(const Beat &beat)
{
    if (beat.stream != StreamKind::OperandA) {
        throw std::invalid_argument(
            "A register_file_in can only load operand A beats");
    }
    if (beat.index >= command.operandA.beats) {
        throw std::invalid_argument("operand A load beat index out of range");
    }

    if (matchesResidentAddress(command, beat)) {
        loaded[slot(0, beat.index)] = true;
        return;
    }

    // AddressGenerator emits A preload once per instruction. Infer the
    // instruction by matching the external A preload address. Synthetic
    // direct commands use this linear fallback; CSR commands are matched
    // against register_addr.sv's nested program above.
    for (uint32_t instruction = 0; instruction < command.instructionLoops;
         ++instruction) {
        const Addr expected =
            command.operandA.base +
            static_cast<Addr>(instruction) *
                command.operandA.instructionStrideBytes +
            static_cast<Addr>(beat.index) * command.operandA.strideBytes;
        if (beat.address == expected) {
            loaded[slot(instruction, beat.index)] = true;
            return;
        }
    }

    throw std::invalid_argument(
        "operand A load address is not in this command");
}

bool
ARegisterFileIn::instructionReady(uint32_t instruction) const
{
    checkInstruction(instruction);
    for (uint32_t beat = 0; beat < command.operandA.beats; ++beat) {
        if (!loaded[slot(instruction, beat)]) {
            return false;
        }
    }
    return true;
}

uint32_t
ARegisterFileIn::loadedBeats(uint32_t instruction) const
{
    checkInstruction(instruction);
    uint32_t count = 0;
    for (uint32_t beat = 0; beat < command.operandA.beats; ++beat) {
        if (loaded[slot(instruction, beat)]) {
            ++count;
        }
    }
    return count;
}

uint64_t
ARegisterFileIn::totalExternalLoadBeats() const
{
    return static_cast<uint64_t>(command.operandA.beats) *
        command.instructionLoops;
}

uint64_t
ARegisterFileIn::totalArrayInputBeats() const
{
    return arrayInputs;
}

Beat
ARegisterFileIn::arrayInputBeat(
    uint32_t instruction, uint32_t flow, uint32_t beat,
    uint32_t arrayIndex) const
{
    checkInstruction(instruction);
    if (flow >= command.flowLoops) {
        throw std::invalid_argument("operand A array flow out of range");
    }
    if (beat >= command.operandA.beats) {
        throw std::invalid_argument("operand A array beat out of range");
    }
    if (!instructionReady(instruction)) {
        throw std::logic_error(
            "operand A array input requested before preload completion");
    }

    if (arrayIndex >= arrayInputs) {
        throw std::invalid_argument("operand A array index out of range");
    }
    const bool last = arrayIndex + 1 == arrayInputs;

    return {StreamKind::OperandA, 0, arrayIndex, last};
}

size_t
ARegisterFileIn::slot(uint32_t instruction, uint32_t beat) const
{
    return static_cast<size_t>(instruction) * command.operandA.beats + beat;
}

void
ARegisterFileIn::checkInstruction(uint32_t instruction) const
{
    if (instruction >= command.instructionLoops) {
        throw std::invalid_argument("operand A instruction out of range");
    }
}

} // namespace gem5::sau
