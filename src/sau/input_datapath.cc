#include "sau/input_datapath.hh"

#include <cassert>

namespace gem5::sau
{

StreamPaddingShifter::StreamPaddingShifter(uint8_t paddingBytes)
    : paddingBytes(paddingBytes & 0xf)
{
}

MemoryBeat256
StreamPaddingShifter::shift(const MemoryBeat256 &data, bool startOfPacket)
{
    // {data, prev} << padding*8, output = high 256 bits: byte j takes
    // data byte j-p, or the previous beat's tail byte 32-p+j below p.
    MemoryBeat256 output;
    const unsigned pad = paddingBytes;
    for (unsigned byte = 0; byte < BeatBytes; ++byte) {
        if (byte >= pad) {
            output.bytes[byte] = data.bytes[byte - pad];
        } else if (!startOfPacket) {
            output.bytes[byte] = previous.bytes[BeatBytes - pad + byte];
        }
    }
    previous = data;
    return output;
}

void
InputRegisterFile::write(uint8_t pointer, const MemoryBeat256 &data)
{
    storage[pointer] = data;
    availLabel.set(pointer);
}

void
InputRegisterFile::clearLabels()
{
    availLabel.reset();
}

bool
InputRegisterFile::available(uint8_t pointer) const
{
    return availLabel.test(pointer);
}

const MemoryBeat256 &
InputRegisterFile::read(uint8_t pointer) const
{
    return storage[pointer];
}

void
FeederBPipeline::step(const MemoryBeat256 &dataIn, bool enableB)
{
    // All four registers sample their pre-edge inputs, so the chain is
    // committed back to front.
    operandBOut = enableB ? case0Reg : MemoryBeat256{};
    case0Reg = dataD2;
    dataD2 = dataD1;
    dataD1 = dataIn;
}

const MemoryBeat256 &
FeederBPipeline::registerFileWriteData() const
{
    return dataD1;
}

const MemoryBeat256 &
FeederBPipeline::operandB() const
{
    return operandBOut;
}

void
FeederAPipeline::step(const MemoryBeat256 &dataIn, bool enable)
{
    // shift_register.sv in bypass mode: the shift stage samples the
    // readout on enabled cycles and zero on idle cycles; data_A_o
    // registers the previous shift-stage value.
    operandAOut = shiftData;
    shiftData = enable ? dataIn : MemoryBeat256{};
}

const MemoryBeat256 &
FeederAPipeline::operandA() const
{
    return operandAOut;
}

InputWritePath::InputWritePath(uint8_t paddingBytes)
    : shifter(paddingBytes)
{
}

void
InputWritePath::write(InputRegisterFile &file, uint8_t pointer,
                      const MemoryBeat256 &data, bool padFlag,
                      bool startOfPacket)
{
    const MemoryBeat256 input = padFlag ? MemoryBeat256{} : data;
    file.write(pointer, shifter.shift(input, startOfPacket));
}

InputReadPointerProgram::InputReadPointerProgram(
    const SauInputCsrConfig &counters)
    : xStep(counters.xStep), yStep(counters.yStep),
      flowStep(counters.flowStep), instructionStep(counters.instructionStep)
{
    // 6-bit "burst - 1" compares: a raw zero burst wraps to 63 and
    // therefore means 64 iterations in this FSM.
    xLimit = (static_cast<uint32_t>(counters.xBurst) - 1) & 0x3f;
    yLimit = (static_cast<uint32_t>(counters.yBurst) - 1) & 0x3f;
    flowLimit = (static_cast<uint32_t>(counters.flowBurst) - 1) & 0x3f;
    instructionLimit =
        (static_cast<uint32_t>(counters.instructionBurst) - 1) & 0x3f;
}

bool
InputReadPointerProgram::done() const
{
    return exhausted;
}

uint8_t
InputReadPointerProgram::pointer() const
{
    assert(!exhausted);
    return addrCurrent;
}

bool
InputReadPointerProgram::lastOfBurst() const
{
    assert(!exhausted);
    return endX() && endY();
}

bool
InputReadPointerProgram::last() const
{
    assert(!exhausted);
    return endX() && endY() && endFlow() && endInstruction();
}

uint32_t
InputReadPointerProgram::x() const
{
    return cntX;
}

uint32_t
InputReadPointerProgram::y() const
{
    return cntY;
}

uint32_t
InputReadPointerProgram::flow() const
{
    return cntFlow;
}

uint32_t
InputReadPointerProgram::instruction() const
{
    return cntInstruction;
}

bool
InputReadPointerProgram::endX() const
{
    return cntX == xLimit;
}

bool
InputReadPointerProgram::endY() const
{
    return cntY == yLimit;
}

bool
InputReadPointerProgram::endFlow() const
{
    return cntFlow == flowLimit;
}

bool
InputReadPointerProgram::endInstruction() const
{
    return cntInstruction == instructionLimit;
}

void
InputReadPointerProgram::advance()
{
    assert(!exhausted);
    if (!endX()) {
        ++cntX;
        addrCurrent = static_cast<uint8_t>(addrCurrent + xStep);
        return;
    }
    if (!endY()) {
        cntX = 0;
        ++cntY;
        addrYBase = static_cast<uint8_t>(addrYBase + yStep);
        addrCurrent = addrYBase;
        return;
    }
    // end_x && end_y: the shared adder resolves the next base from the
    // first still-open outer level.
    cntX = 0;
    cntY = 0;
    if (!endFlow()) {
        ++cntFlow;
        addrFlowBase = static_cast<uint8_t>(addrFlowBase + flowStep);
        addrYBase = addrFlowBase;
        addrCurrent = addrFlowBase;
        return;
    }
    if (!endInstruction()) {
        cntFlow = 0;
        ++cntInstruction;
        addrInstructionBase =
            static_cast<uint8_t>(addrInstructionBase + instructionStep);
        addrFlowBase = addrInstructionBase;
        addrYBase = addrInstructionBase;
        addrCurrent = addrInstructionBase;
        return;
    }
    exhausted = true;
}

} // namespace gem5::sau
