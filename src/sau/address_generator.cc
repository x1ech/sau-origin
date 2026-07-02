#include "sau/address_generator.hh"

#include <cassert>

namespace gem5::sau
{

AddressGenerator::AddressGenerator(const SauCommand &command)
    : command(command)
{
    updateFront();
}

bool
AddressGenerator::empty() const
{
    return exhausted;
}

const Beat &
AddressGenerator::front() const
{
    assert(!empty());
    return current;
}

void
AddressGenerator::pop()
{
    assert(!empty());

    ++beat;
    if (beat < streamDesc().beats) {
        updateFront();
        return;
    }

    beat = 0;
    if (stream == StreamKind::OperandB) {
        stream = StreamKind::OperandA;
        updateFront();
        return;
    }

    stream = StreamKind::OperandB;
    ++flow;
    if (flow < command.flowLoops) {
        updateFront();
        return;
    }

    flow = 0;
    ++instruction;
    if (instruction < command.instructionLoops) {
        updateFront();
        return;
    }

    exhausted = true;
}

uint64_t
AddressGenerator::totalReadBeats() const
{
    const uint64_t beats =
        static_cast<uint64_t>(command.operandA.beats) +
        command.operandB.beats;
    return beats * command.flowLoops * command.instructionLoops;
}

const StreamDesc &
AddressGenerator::streamDesc() const
{
    return stream == StreamKind::OperandB ?
        command.operandB : command.operandA;
}

void
AddressGenerator::updateFront()
{
    const auto &desc = streamDesc();
    const Addr address =
        desc.base +
        static_cast<Addr>(instruction) * desc.instructionStrideBytes +
        static_cast<Addr>(flow) * desc.flowStrideBytes +
        static_cast<Addr>(beat) * desc.strideBytes;
    current = {
        stream,
        address,
        beat,
        beat + 1 == desc.beats,
    };
}

} // namespace gem5::sau
