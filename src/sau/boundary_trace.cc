#include "sau/boundary_trace.hh"

#include <vector>

#include "sau/address_program.hh"
#include "sau/functional_memory.hh"
#include "sau/input_datapath.hh"
#include "sau/resource_config.hh"
#include "sau/transposer.hh"

namespace gem5::sau
{
namespace
{

// The 32x32x32 ABTD boundary package contract (manifest.json):
// operands at the ATBD-layout image addresses, one flow, one
// instruction.
constexpr Addr PackageBase = 0x29120000;
constexpr uint64_t PackageSpan = 0x40000;
constexpr Addr OperandABase = 0x29120000;
constexpr Addr OperandBBase = 0x29120400;

std::string
hexValue(const MemoryBeat256 &value)
{
    static const char digits[] = "0123456789abcdef";
    std::string text = "0x";
    for (unsigned lane = BeatBytes; lane-- > 0;) {
        text.push_back(digits[value.bytes[lane] >> 4]);
        text.push_back(digits[value.bytes[lane] & 0xf]);
    }
    return text;
}

SauControlFields
abtdControl()
{
    SauControlFields control;
    control.transMode = 2;
    control.reuseMode = 1;
    control.saFlowMode = 0;
    control.horizontalAddress = OperandABase;
    control.verticalAddress = OperandBBase;
    control.registerInput.xBurst = 1;
    control.registerInput.yStep = 1;
    control.registerInput.yCycle = 32;
    control.registerInput.cCycle = 1;
    control.vertical.xStep = 1;
    control.vertical.xBurst = 1;
    control.vertical.yStep = 1;
    control.vertical.yCycle = 32;
    control.vertical.flowStep = 1;
    control.vertical.flowCycle = 1;
    control.vertical.instructionStep = 1;
    control.vertical.instructionCycle = 1;
    return control;
}

MemoryBeat256
readBeat(const FunctionalMemory &memory, Addr address)
{
    MemoryBeat256 beat;
    memory.read(address, beat.bytes.data(), BeatBytes);
    return beat;
}

} // anonymous namespace

BoundaryTraceWriter::BoundaryTraceWriter(const std::string &path)
    : output(path)
{
    if (output.is_open()) {
        output << "signal,cycle,value\n";
    }
}

void
BoundaryTraceWriter::emit(const std::string &signal,
                          const MemoryBeat256 &value)
{
    writeRow(signal, hexValue(value));
}

void
BoundaryTraceWriter::emitZero(const std::string &signal)
{
    writeRow(signal, "0x0");
}

bool
BoundaryTraceWriter::good() const
{
    return output.good();
}

void
BoundaryTraceWriter::writeRow(const std::string &signal,
                              const std::string &value)
{
    output << signal << ',' << nextCycle[signal]++ << ',' << value << '\n';
}

bool
generateAbtdBoundaryTrace(const std::string &packageDirectory,
                          const std::string &outputPath)
{
    const std::string imagePath = packageDirectory + "/initial_memory.hex";
    {
        std::ifstream probe(imagePath);
        if (!probe.is_open()) {
            return false;
        }
    }

    FunctionalMemory memory(PackageBase, PackageSpan);
    memory.loadLittleEndianWordHexFile(imagePath, PackageBase, 16);

    const auto configs = deriveResourceConfigs(abtdControl());
    BoundaryTraceWriter trace(outputPath);

    // External read payloads: the resident operand loads first, then
    // the streamed operand, both from the raw-counter address programs.
    trace.emitZero("sau_sram_rdata");
    RtlResidentAddressProgram resident(configs.residentAddress);
    while (!resident.done()) {
        trace.emit("sau_sram_rdata", readBeat(memory, resident.address()));
        resident.advance();
    }
    std::vector<MemoryBeat256> streamBeats;
    RtlStreamAddressProgram stream(configs.streamAddress);
    while (!stream.done()) {
        const auto beat = readBeat(memory, stream.address());
        trace.emit("sau_sram_rdata", beat);
        streamBeats.push_back(beat);
        stream.advance();
    }

    // The streamed beats run the feeder B pipeline into the arbiter;
    // outputs surface after the four-register delay.
    TransposerArbiter arbiter(configs.transposeReuse);
    arbiter.clearOnStart();
    FeederBPipeline feeder;
    for (unsigned step = 0; step < streamBeats.size() + 3; ++step) {
        const MemoryBeat256 input = step < streamBeats.size() ?
            streamBeats[step] : MemoryBeat256{};
        feeder.step(input, true);
        if (step >= 3) {
            const auto &operandB = feeder.operandB();
            trace.emit("data_B", operandB);
            arbiter.acceptRow(operandB);
            trace.emit("u_trans2sa_top.trans0_inRow", operandB);
        }
    }

    trace.emitZero("u_trans2sa_top.trans0_outCol");
    arbiter.startOutputPhase();
    while (arbiter.columnReady()) {
        const auto column = arbiter.readColumn();
        trace.emit("u_trans2sa_top.trans0_outCol", column.data);
        if (column.last) {
            break;
        }
    }
    return trace.good();
}

} // namespace gem5::sau
