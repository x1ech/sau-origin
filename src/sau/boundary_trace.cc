#include "sau/boundary_trace.hh"

#include <array>
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

// Cycle anchors and register depths from the frozen control sources
// (scheduler.sv, feeder.sv, sa_feeder.sv; hashes in
// RTL_TIMING_PROVENANCE.md).  Every constant is structural — none is
// fitted to the golden trace.
//
// First resident SRAM request edge after start (strict driver).
constexpr unsigned ResidentReadStart = 3;
// mem_ctrl read-visible chain: STATE_DELAY = SRAM_DELAY + 1.
constexpr unsigned MemVisibleDelay = 4;
// scheduler.sv transload_state_cnt holds TRANSPOSE_LOAD for SA_SIZE
// edges (the streamed data_last cannot arrive earlier in this shape).
constexpr unsigned TransposeLoadCycles = 32;
// REUSE_LOAD entry to the first streamed SRAM request (Step 5.5-proven
// 290 -> 293 on the baseline).
constexpr unsigned StreamRequestOffset = 3;
// SRAM_DELAY: request to rdata at the shared-SRAM boundary.
constexpr unsigned SramVisibleDelay = 3;
// TRANSPOSE_LOAD entry to the first data_A_o beat: the feeder-visible
// state edge starts the register-file readout (Step 5.5-proven
// 258 -> 269 on the baseline).
constexpr unsigned ReadoutDelay = 11;
// scheduler input_switch to the sa_feeder ports: feeder STATE_DELAY
// (SRAM_DELAY + ADDR_DELAY + MEMCTRL_DELAY = 7) + input_switch_d + two
// out-pipe stages + the input_switch_o output register.
constexpr unsigned SwitchDelay = 11;
// feeder.sv input_switch_case: ~input_switch_d_o[0][0] registered once,
// i.e. the inverted scheduler switch bit 0 delayed STATE_DELAY + 2.
constexpr unsigned SwitchCaseDelay = 9;
// feeder.sv data chain: data_i -> data_i_d -> data_i_d2 ->
// data_i_case0_reg -> data_B_o.
constexpr unsigned FeederOutputDelay = 4;
// feeder.sv STATE_DELAY: scheduler core_state at the feeder taps.
constexpr unsigned FeederStateDelay = 7;

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
    control.input.xStep = 1;
    control.input.xBurst = 1;
    control.input.yStep = 1;
    control.input.yBurst = 32;
    control.input.flowStep = 1;
    control.input.flowBurst = 1;
    control.input.instructionStep = 1;
    control.input.instructionBurst = 1;
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
    writeRow(signal, nextCycle[signal], hexValue(value));
}

void
BoundaryTraceWriter::emit(const std::string &signal, uint64_t cycle,
                          const MemoryBeat256 &value)
{
    writeRow(signal, cycle, hexValue(value));
}

void
BoundaryTraceWriter::emitZero(const std::string &signal)
{
    writeRow(signal, nextCycle[signal], "0x0");
}

void
BoundaryTraceWriter::emitZero(const std::string &signal, uint64_t cycle)
{
    writeRow(signal, cycle, "0x0");
}

bool
BoundaryTraceWriter::good() const
{
    return output.good();
}

void
BoundaryTraceWriter::writeRow(const std::string &signal, uint64_t cycle,
                              const std::string &value)
{
    output << signal << ',' << cycle << ',' << value << '\n';
    nextCycle[signal] = cycle + 1;
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
    // Their rdata cycles are emitted inside the per-cycle loop below.
    std::vector<MemoryBeat256> residentBeats;
    RtlResidentAddressProgram resident(configs.residentAddress);
    while (!resident.done()) {
        residentBeats.push_back(readBeat(memory, resident.address()));
        resident.advance();
    }
    std::vector<MemoryBeat256> streamBeats;
    RtlStreamAddressProgram stream(configs.streamAddress);
    while (!stream.done()) {
        streamBeats.push_back(readBeat(memory, stream.address()));
        stream.advance();
    }

    // Operand-A readout payloads: the resident beats enter the input
    // register file through the write path and come back in
    // read-pointer order.  Under reuse-A the readout replays back to
    // back (shift_almost_last retriggers rden without a gap), so the
    // data_A stream is the readout sequence twice.
    InputRegisterFile file;
    InputWritePath writePath(configs.input.padding);
    for (unsigned index = 0; index < residentBeats.size(); ++index) {
        writePath.write(file, index, residentBeats[index], false,
                        index == 0);
    }
    std::vector<MemoryBeat256> readout;
    InputReadPointerProgram pointerProgram(configs.input.streamedCounters);
    while (!pointerProgram.done()) {
        readout.push_back(file.read(pointerProgram.pointer()));
        pointerProgram.advance();
    }

    // Per-cycle ABTD control chain.  Under raw ABTD+reuse-A the
    // scheduler preloads input_switch=11 in IDLE and REUSE_LOAD forces
    // 01 (conv_reuse_flag is constant 1), so the feeder B gate
    // (input_switch_case) is open the whole time: the resident tail
    // beat leaks through the data chain as one early accepted data_B
    // pulse, and once the delayed 01 reaches the sa_feeder data mux
    // (data_i = input_switch[1] ? data_B_i : data_A_i), the bank load
    // enables still follow B-valid while the loaded payload follows
    // the replayed data_A stream.
    const unsigned residentCount = residentBeats.size();
    const unsigned streamCount = streamBeats.size();
    const unsigned residentEnStart = ResidentReadStart + MemVisibleDelay;
    const unsigned residentEnLast = residentEnStart + residentCount - 1;
    const unsigned transposeLoadEntry = ResidentReadStart + residentCount - 1;
    const unsigned reuseLoadEntry = transposeLoadEntry + TransposeLoadCycles;
    const unsigned streamRequestStart = reuseLoadEntry + StreamRequestOffset;
    const unsigned streamEnStart = streamRequestStart + MemVisibleDelay;
    const unsigned residentDataStart = ResidentReadStart + SramVisibleDelay;
    const unsigned streamDataStart = streamRequestStart + SramVisibleDelay;
    const unsigned readoutStart = transposeLoadEntry + ReadoutDelay;
    const unsigned readoutBeats = 2 * readout.size();
    const unsigned endCycle =
        streamEnStart + streamCount + FeederOutputDelay + 2;

    // feeder.sv output-number state machine (conv_reuse_flag==1 arcs).
    enum class OutState { NoInput, OneInput, TwoInput };
    OutState outState = OutState::NoInput;
    bool noInputDelay = true;   // cur_out_state_o[1] == NO_INPUT
    bool noInputState = true;   // registered NO_INPUT_state
    std::array<bool, 3> enDelay{};        // EN_i_d .. EN_i_d3
    std::array<bool, SwitchCaseDelay> caseDelay;
    caseDelay.fill(true);                 // reset switch 00 -> case 1
    std::array<uint8_t, SwitchDelay> switchDelay{};
    FeederBPipeline chain;
    bool dataBValidOut = false;           // data_B_valid_o
    MemoryBeat256 dataBOut;               // data_B_o
    // sa_feeder.sv: data_i_d loads on any operand valid;
    // trans_load_valid registers data_B_valid for ABTD.
    MemoryBeat256 dataInputReg;
    bool transLoadValid = false;

    TransposerArbiter arbiter(configs.transposeReuse);
    arbiter.clearOnStart();
    bool outputPhaseStarted = false;
    uint64_t outputColumnCycle = 0;

    trace.emitZero("sau_sram_rdata", 0);
    for (unsigned cycle = 0; cycle <= endCycle; ++cycle) {
        // Wires of this cycle.
        const bool memEnResident =
            cycle >= residentEnStart && cycle <= residentEnLast;
        const bool memEnStream = cycle >= streamEnStart &&
            cycle < streamEnStart + streamCount;
        const bool memEn = memEnResident || memEnStream;
        MemoryBeat256 memData;
        if (memEnResident) {
            memData = residentBeats[cycle - residentEnStart];
        } else if (memEnStream) {
            memData = streamBeats[cycle - streamEnStart];
        }
        const bool memLast = cycle == residentEnLast ||
            (streamCount > 0 && cycle == streamEnStart + streamCount - 1);

        const bool readoutValid = cycle >= readoutStart &&
            cycle < readoutStart + readoutBeats;
        MemoryBeat256 operandA;
        if (readoutValid) {
            operandA = readout[(cycle - readoutStart) % readout.size()];
        }

        // scheduler.sv input_switch: IDLE preloads 2'b11 for ABTD, and
        // REUSE_LOAD forces 2'b01 one edge after entry because
        // conv_reuse_flag_reg is constant 1 (the raw-config quirk that
        // overrides the commented ABT-reuse-A switch table).
        const uint8_t schedSwitch =
            cycle == 0 ? 0 : (cycle >= reuseLoadEntry + 1 ? 0b01 : 0b11);

        // Registered values visible this cycle.
        const uint8_t saSwitch = switchDelay[0];
        const bool inputSwitchCase = caseDelay[0];

        // The rdata boundary: request cycle plus SRAM_DELAY.
        if (cycle >= residentDataStart &&
            cycle < residentDataStart + residentCount) {
            trace.emit("sau_sram_rdata", cycle,
                       residentBeats[cycle - residentDataStart]);
        } else if (cycle >= streamDataStart &&
                   cycle < streamDataStart + streamCount) {
            trace.emit("sau_sram_rdata", cycle,
                       streamBeats[cycle - streamDataStart]);
        }

        // sa_feeder acceptances (inputs before outputs per the
        // arbiter call-order contract).
        if (transLoadValid) {
            const unsigned bankIndex = arbiter.acceptRow(dataInputReg);
            if (bankIndex == 0) {
                trace.emit("u_trans2sa_top.trans0_inRow", cycle,
                           dataInputReg);
            }
        }
        if (dataBValidOut) {
            trace.emit("data_B", cycle, dataBOut);
        }
        if (readoutValid) {
            trace.emit("data_A", cycle, operandA);
        }
        // The output bank latches from the registered pre-edge loaded
        // bank when T0 fills, before the overflow row reaches T1; the
        // first prefetched column surfaces one cycle later.
        if (!outputPhaseStarted && arbiter.bank(0).outputReady()) {
            arbiter.startOutputPhase();
            outputPhaseStarted = true;
            outputColumnCycle = cycle + 1;
        }

        // feeder.sv next state.
        OutState nextOutState = outState;
        switch (outState) {
          case OutState::NoInput:
            // trans_flag & conv_reuse_flag_reg & data_last_i.
            if (memLast) {
                nextOutState = OutState::OneInput;
            }
            break;
          case OutState::OneInput:
            // The feeder-visible TRANSPOSE_LOAD -> REUSE_LOAD edge.
            if (cycle == reuseLoadEntry + FeederStateDelay) {
                nextOutState = OutState::TwoInput;
            }
            break;
          case OutState::TwoInput:
            // Returns to NO_INPUT only past this capture window.
            break;
        }

        // data_B gating: data_C_state stays 0 for single-flow int8
        // GEMM (the C pre-chain needs EN during last_flow_time).
        const bool dataBEnable = !inputSwitchCase && !noInputState;
        const bool dataBValid = dataBEnable && enDelay[2];
        chain.step(memData, dataBEnable);

        // Commit the registers for the next cycle.  Order matters:
        // consumers of this cycle's values run before overwrites.
        const bool operandEn = readoutValid || dataBValidOut;
        if (operandEn) {
            dataInputReg = (saSwitch & 0b10) ? dataBOut : operandA;
        }
        transLoadValid = dataBValidOut;
        dataBOut = chain.operandB();
        dataBValidOut = dataBValid;
        noInputState = noInputDelay;
        noInputDelay = outState == OutState::NoInput;
        outState = nextOutState;
        enDelay[2] = enDelay[1];
        enDelay[1] = enDelay[0];
        enDelay[0] = memEn;
        for (unsigned tap = 0; tap + 1 < SwitchDelay; ++tap) {
            switchDelay[tap] = switchDelay[tap + 1];
        }
        switchDelay[SwitchDelay - 1] = schedSwitch;
        for (unsigned tap = 0; tap + 1 < SwitchCaseDelay; ++tap) {
            caseDelay[tap] = caseDelay[tap + 1];
        }
        caseDelay[SwitchCaseDelay - 1] = !(schedSwitch & 0b01);
    }

    trace.emitZero("u_trans2sa_top.trans0_outCol", 0);
    while (arbiter.columnReady()) {
        const auto column = arbiter.readColumn();
        trace.emit("u_trans2sa_top.trans0_outCol", outputColumnCycle++,
                   column.data);
        if (column.last) {
            break;
        }
    }
    return trace.good();
}

} // namespace gem5::sau
