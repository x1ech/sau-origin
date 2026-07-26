#include "sau/address_program.hh"

#include <cassert>

namespace gem5::sau
{

RtlStreamAddressProgram::RtlStreamAddressProgram(
    const SauStreamAddressResourceConfig &config)
{
    const auto &counters = config.counters;
    // mem_addr.sv IDLE: a zero x/y/flow/instruction count raises
    // last_load_done immediately and produces no address.
    if (counters.xBurst == 0 || counters.yCycle == 0 ||
        counters.flowCycle == 0 || counters.instructionCycle == 0) {
        exhausted = true;
        return;
    }
    xLimit = counters.xBurst - 1;
    yLimit = counters.yCycle - 1;
    flowLimit = counters.flowCycle - 1;
    instructionLimit = counters.instructionCycle - 1;

    // The two-stage start pipeline computes the step registers.  The
    // flow*y product is a 16-bit register in the source; conv_kernal=0
    // selects the fixed one-beat instruction step.
    const uint16_t flowTimesY = static_cast<uint16_t>(
        static_cast<uint32_t>(counters.flowStep) * counters.yStep);
    stepXBytes = static_cast<uint32_t>(counters.xStep) << 5;
    stepYBytes = static_cast<uint32_t>(counters.yStep) << 5;
    stepFlowBytes = static_cast<uint32_t>(flowTimesY) << 5;
    stepInstructionBytes = config.convKernal == 0 ?
        uint32_t(1) << 5 :
        (static_cast<uint32_t>(counters.instructionStep) * flowTimesY) << 5;

    currentAddr = static_cast<uint32_t>(config.baseAddress);
    rowStartAddr = currentAddr;
    flowStartAddr = currentAddr;
    instructionStartAddr = currentAddr;
}

bool
RtlStreamAddressProgram::done() const
{
    return exhausted;
}

Addr
RtlStreamAddressProgram::address() const
{
    assert(!exhausted);
    return currentAddr;
}

bool
RtlStreamAddressProgram::lastOfFlow() const
{
    assert(!exhausted);
    // vertical_cnt_last is raised only when a multi-row flow enters its
    // final row; a single-row program never raises it in the source.
    return yLimit > 0 && cntY == yLimit;
}

bool
RtlStreamAddressProgram::last() const
{
    assert(!exhausted);
    if (yLimit == 0) {
        // Single-row programs take the RUNNING else-branch straight to
        // DONE after the x walk; flow/instruction never advance.
        return cntX == xLimit;
    }
    return cntY == yLimit && cntFlow == flowLimit &&
        cntInstruction == instructionLimit;
}

uint32_t
RtlStreamAddressProgram::x() const
{
    return cntX;
}

uint32_t
RtlStreamAddressProgram::y() const
{
    return cntY;
}

uint32_t
RtlStreamAddressProgram::flow() const
{
    return cntFlow;
}

uint32_t
RtlStreamAddressProgram::instruction() const
{
    return cntInstruction;
}

void
RtlStreamAddressProgram::advance()
{
    assert(!exhausted);
    if (last()) {
        exhausted = true;
        return;
    }
    if (yLimit == 0) {
        ++cntX;
        currentAddr += stepXBytes;
        return;
    }
    if (cntY == yLimit) {
        // The source leaves RUNNING when it enters the final row, so a
        // flow emits exactly one beat of that row before the WAIT_TRIG
        // retrigger; cnt_x/cnt_y restart there.
        cntX = 0;
        cntY = 0;
        if (cntFlow == flowLimit) {
            cntFlow = 0;
            ++cntInstruction;
            instructionStartAddr += stepInstructionBytes;
            flowStartAddr = instructionStartAddr;
            rowStartAddr = instructionStartAddr;
            currentAddr = instructionStartAddr;
        } else {
            ++cntFlow;
            flowStartAddr += stepFlowBytes;
            rowStartAddr = flowStartAddr;
            currentAddr = flowStartAddr;
        }
        return;
    }
    if (cntX < xLimit) {
        ++cntX;
        currentAddr += stepXBytes;
        return;
    }
    cntX = 0;
    ++cntY;
    rowStartAddr += stepYBytes;
    currentAddr = rowStartAddr;
}

RtlResidentAddressProgram::RtlResidentAddressProgram(
    const SauResidentAddressResourceConfig &config)
{
    // register_addr.sv IDLE: a zero x/y/channel count raises load_done
    // immediately and produces no address.
    if (config.xBurst == 0 || config.yCycle == 0 || config.cCycle == 0) {
        exhausted = true;
        return;
    }
    xLimit = config.xBurst - 1;
    yLimit = config.yCycle - 1;
    channelLimit = config.cCycle - 1;
    stepYBytes = static_cast<uint32_t>(config.yStep) << 5;
    stepChannelBytes =
        (static_cast<uint32_t>(config.cStep) * config.yStep) << 5;
    paddingEnabled = config.padding != 0;
    validYStart = config.validYStart;
    validYEnd = config.validYEnd;
    validXStart = config.validXStart;
    validXEnd = config.validXEnd;

    currentAddr = static_cast<uint32_t>(config.baseAddress);
    rowStartAddr = currentAddr;
    channelStartAddr = currentAddr;
}

bool
RtlResidentAddressProgram::done() const
{
    return exhausted;
}

Addr
RtlResidentAddressProgram::address() const
{
    assert(!exhausted);
    return currentAddr;
}

uint32_t
RtlResidentAddressProgram::writePointer() const
{
    assert(!exhausted);
    return pointer;
}

bool
RtlResidentAddressProgram::paddingBeat() const
{
    assert(!exhausted);
    return rawYPadding() || xPadding();
}

bool
RtlResidentAddressProgram::last() const
{
    assert(!exhausted);
    return cntX == xLimit && cntY == yLimit && cntChannel == channelLimit;
}

uint32_t
RtlResidentAddressProgram::x() const
{
    return cntX;
}

uint32_t
RtlResidentAddressProgram::y() const
{
    return cntY;
}

uint32_t
RtlResidentAddressProgram::channel() const
{
    return cntChannel;
}

bool
RtlResidentAddressProgram::rawYPadding() const
{
    return paddingEnabled && (cntY < validYStart || cntY > validYEnd);
}

bool
RtlResidentAddressProgram::xPadding() const
{
    return paddingEnabled && (cntX < validXStart || cntX > validXEnd);
}

void
RtlResidentAddressProgram::advance()
{
    assert(!exhausted);
    ++pointer;
    if (cntX < xLimit) {
        // A padded position freezes the address; the write pointer above
        // still advances (register_addr.sv cnt).
        if (!(rawYPadding() || xPadding())) {
            currentAddr += 32;
        }
        ++cntX;
        return;
    }
    if (cntY < yLimit) {
        // Leaving a y-padded row keeps row_start and returns the address
        // to the row start; a valid row advances by the y step.
        const bool leavingPaddedRow = rawYPadding();
        cntX = 0;
        ++cntY;
        if (!leavingPaddedRow) {
            rowStartAddr += stepYBytes;
        }
        currentAddr = rowStartAddr;
        return;
    }
    if (cntChannel < channelLimit) {
        cntX = 0;
        cntY = 0;
        ++cntChannel;
        channelStartAddr += stepChannelBytes;
        rowStartAddr = channelStartAddr;
        currentAddr = channelStartAddr;
        return;
    }
    exhausted = true;
}

} // namespace gem5::sau
