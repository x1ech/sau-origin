#include "sau/timing_policy.hh"

#include <limits>
#include <stdexcept>

namespace gem5::sau
{
namespace
{

Cycles
cycles(uint64_t value)
{
    return Cycles(value);
}

std::string
source(const char *text)
{
    return text;
}

} // anonymous namespace

RtlStorageTiming
RtlStorageTiming::derive(const RtlTimingParameters &rtl)
{
    if (rtl.sramDataWidthBits != 256 || rtl.sramDataWidthBits % 8 != 0) {
        throw std::invalid_argument(
            "supported SAU RTL storage contract requires a 256-bit SRAM "
            "interface");
    }

    RtlStorageTiming storage;
    storage.beatBytes = rtl.sramDataWidthBits / 8;
    storage.issueWidth = 1;
    storage.sharedReadWritePort = true;
    storage.readPriority = true;
    storage.inOrderResponses = true;
    // mem_ctrl.sv shifts the request valid through STATE_DELAY, where
    // STATE_DELAY = SA_CORE.SRAM_DELAY + 1.
    storage.readVisibleLatencyCycles =
        cycles(static_cast<uint64_t>(rtl.sramDelay) + 1);
    return storage;
}

TimingPolicy
TimingPolicy::derive(const SauCommand &command, uint8_t transMode,
                     uint8_t reuseMode, const RtlTimingParameters &rtl)
{
    if (rtl.saSize == 0 || rtl.registerDepth == 0 ||
        rtl.saSize > rtl.registerDepth || command.operandA.beats == 0 ||
        command.operandB.beats == 0 || command.output.beats == 0 ||
        command.flowLoops == 0 || command.instructionLoops == 0 ||
        command.workItems == 0) {
        throw std::invalid_argument("invalid RTL timing-policy dimensions");
    }

    TimingPolicy policy;
    policy.transMode = transMode;
    policy.reuseMode = reuseMode;
    policy.residentLoadBeats = command.operandA.beats;
    const uint64_t defaultScheduleInstructions =
        static_cast<uint64_t>(command.flowLoops) * command.instructionLoops;
    const uint64_t scheduleInstructions =
        command.scheduleInstructions != 0 ?
            command.scheduleInstructions : defaultScheduleInstructions;
    if (scheduleInstructions == 0 ||
        scheduleInstructions > std::numeric_limits<uint32_t>::max() ||
        command.workItems % scheduleInstructions != 0) {
        throw std::invalid_argument(
            "invalid RTL schedule-instruction extent");
    }
    policy.scheduleInstructions = scheduleInstructions;
    policy.inputBeatsPerInstruction =
        command.workItems / policy.scheduleInstructions;
    if (static_cast<uint64_t>(policy.inputBeatsPerInstruction) +
            command.flowLoops < rtl.saSize) {
        throw std::invalid_argument(
            "RTL flow execution extent is shorter than one SA row");
    }
    // scheduler.sv advances flow_times_cnt on each data_last.  Across one
    // instruction, the first SA_SIZE row overlaps the feeder fill while the
    // flow_times_i row tails remain visible in REUSE_LOAD.
    policy.flowExecuteCycles = cycles(
        static_cast<uint64_t>(policy.inputBeatsPerInstruction) - rtl.saSize +
        command.flowLoops);
    policy.outputBeats = command.output.beats * command.instructionLoops;
    policy.arrayInputBBeats = command.workItems;
    // register_file_in performs one initial TRANSPOSE_LOAD row and feeder.sv
    // keeps shift_data_cnt valid for SA_SIZE.  It becomes an additional A row
    // only when the resident extent itself is exactly one SA row.
    policy.arrayInputABeats = command.workItems +
        (policy.residentLoadBeats == rtl.saSize ? rtl.saSize : 0);
    // SA_CORE.sv connects scheduler.flow_times_i to flow_loop_times.  Keep
    // this guard independent of scheduleInstructions, which represents the
    // separately connected scheduler.ins_times_i input.
    policy.shortDirectDOutPath = command.flowLoops == 1;
    // scheduler.sv's FIRST_INS/INS_LOOP guards have an asymmetric boundary
    // only when exactly one of flow_times_i and ins_times_i is one. In that
    // case REGISTER_UNLOAD can become visible before the final result stream;
    // otherwise result_last/update_finished closes the final D_OUT.
    policy.earlyFinalUnload =
        (command.flowLoops == 1) != (policy.scheduleInstructions == 1);
    policy.storage = RtlStorageTiming::derive(rtl);

    // mem_addr.sv pipelines command activation by ADDR_DELAY.  The command
    // register itself supplies the following clock edge.
    policy.commandStartCycles = cycles(rtl.memAddressDelay + 1);

    // register_file_in.sv: PAD_DELAY = MEMCTRL_DELAY + SRAM_DELAY +
    // ADDR_DELAY(=1) + 1.  The final resident beat is accepted inclusively.
    const uint64_t residentPad = rtl.memCtrlDelay + rtl.sramDelay +
        rtl.registerFileAddressDelay + 1;
    policy.arrayInputStartDelayCycles = cycles(
        static_cast<uint64_t>(policy.commandStartCycles) +
        policy.residentLoadBeats - 1 +
        static_cast<uint64_t>(policy.storage.readVisibleLatencyCycles) +
        residentPad);

    policy.arrayInputBurstBeats = rtl.saSize;
    policy.arrayInputBurstGapCycles = cycles(rtl.registerFileAddressDelay);
    policy.arrayInputFlowGapCycles = cycles(rtl.sramDelay);
    policy.firstArrayInputFlowGapCycles = policy.arrayInputFlowGapCycles;
    policy.secondArrayInputFlowGapCycles = policy.arrayInputFlowGapCycles;
    policy.steadyArrayInputFlowGapCycles = policy.arrayInputFlowGapCycles;
    policy.arrayInputSkewCycles = rtl.saSize;

    // scheduler.sv updates its state/control register on one clock edge.
    // feeder.sv then propagates input_switch through STATE_DELAY, the
    // input_switch_d register, REGISTER_DELAY output stages, and its final
    // input_switch_o register.  Keep every sequential stage named so the
    // state trace does not hide a fixture-specific offset.
    policy.inputSwitchVisibleDelayCycles = cycles(
        rtl.schedulerStateRegisterDelay + rtl.sramDelay +
        rtl.memAddressDelay + rtl.memCtrlDelay +
        rtl.feederInputSwitchRegisterDelay + rtl.registerDelay +
        rtl.feederOutputSwitchRegisterDelay);

    // sa_feeder.sv registers result_last and update_finished. scheduler.sv
    // then commits REGISTER_UNLOAD and clears input_switch on the following
    // edge; feeder.sv propagates that cleared switch to input_switch_o.
    policy.inputSwitchResetVisibleDelayCycles = cycles(
        rtl.resultLastRegisterDelay + rtl.schedulerStateRegisterDelay +
        rtl.schedulerRegisterUnloadSwitchDelay + rtl.sramDelay +
        rtl.memAddressDelay + rtl.memCtrlDelay +
        rtl.feederInputSwitchRegisterDelay + rtl.registerDelay +
        rtl.feederOutputSwitchRegisterDelay);

    if (policy.shortDirectDOutPath) {
        // REUSE_LOAD enters D_OUT directly when scheduler.flow_times is one.
        // The first two boundaries include the input-switch and feeder
        // pipeline transients; later boundaries settle to one SA row plus the
        // normal completion path.
        policy.firstArrayInputFlowGapCycles =
            policy.inputSwitchVisibleDelayCycles +
            cycles(rtl.sramDelay + rtl.registerFileAddressDelay +
                   rtl.schedulerStateRegisterDelay);
        policy.secondArrayInputFlowGapCycles =
            cycles(rtl.saSize) + policy.inputSwitchVisibleDelayCycles;
        policy.steadyArrayInputFlowGapCycles =
            cycles(rtl.saSize + rtl.memAddressDelay + rtl.memCtrlDelay);

        policy.firstShortExecuteCycles = cycles(
            rtl.saSize + rtl.schedulerStateRegisterDelay);
        policy.steadyShortExecuteCycles = cycles(
            rtl.saSize + 2 * rtl.schedulerStateRegisterDelay);
        policy.firstShortDrainCycles =
            policy.inputSwitchResetVisibleDelayCycles +
            cycles(rtl.schedulerStateRegisterDelay);
        policy.secondShortDrainCycles = cycles(
            rtl.saSize + rtl.sramDelay + rtl.memAddressDelay +
            rtl.memCtrlDelay + rtl.registerDelay +
            rtl.schedulerStateRegisterDelay);
        policy.steadyShortDrainCycles = cycles(
            rtl.saSize + 2 * rtl.schedulerStateRegisterDelay);
    }

    // The B request may be released after the A lead covered by the feeder
    // control pipeline.  The same named stages explain the writeback guard.
    policy.writebackStartDelayCycles = cycles(
        rtl.memAddressDelay + rtl.memCtrlDelay + rtl.registerDelay +
        rtl.registerFileAddressDelay + 1);
    policy.earlyUnloadWritebackStartDelayCycles = cycles(
        rtl.memAddressDelay + rtl.memCtrlDelay + rtl.registerDelay +
        rtl.registerFileAddressDelay);
    policy.bReadStartAheadBeats = rtl.saSize >
            static_cast<uint64_t>(policy.writebackStartDelayCycles) ?
        rtl.saSize - static_cast<uint64_t>(policy.writebackStartDelayCycles) :
        0;
    // B requests lead their matching array admission by the remainder of the
    // SA row.  This bounds returned feeder tokens; the fixed-latency SRAM
    // pipeline remains a separate in-flight stage in strict mode.
    policy.bStagingBeats = rtl.saSize - policy.bReadStartAheadBeats;

    // The first result traverses the two SA dimensions plus the registered
    // feeder/output paths. sa_feeder.sv computes CALC_CYCLE_i from its
    // flow_loop_times_i input, which SA_CORE connects to flow_loop_times.
    const uint64_t feederPath = rtl.sramDelay + rtl.memAddressDelay +
        rtl.memCtrlDelay + rtl.registerDelay;
    policy.arrayFillCycles = cycles(
        policy.residentLoadBeats + 2 * static_cast<uint64_t>(rtl.saSize) +
        feederPath + command.flowLoops +
        rtl.memAddressDelay + rtl.memCtrlDelay + rtl.registerDelay);

    // register_file_in/feeder retain a full output-row burst between flow
    // releases.  A partial output row is drained as one final result stream.
    const uint64_t outputsPerFlow =
        policy.outputBeats / policy.scheduleInstructions;
    if (policy.scheduleInstructions > 1 && outputsPerFlow >= rtl.saSize) {
        if (policy.shortDirectDOutPath) {
            policy.resultFlowGapCycles = cycles(
                rtl.saSize + rtl.memAddressDelay + rtl.memCtrlDelay);
        } else {
            // One retained output row spans the per-instruction input extent,
            // the RTL flow_times_i iterations, and the two registered
            // scheduler/feeder boundary edges.
            policy.resultFlowGapCycles = cycles(
                policy.inputBeatsPerInstruction - rtl.saSize +
                command.flowLoops +
                rtl.schedulerStateRegisterDelay +
                rtl.feederInputSwitchRegisterDelay);
        }
    }

    policy.completionDelayCycles =
        cycles(rtl.memAddressDelay + rtl.memCtrlDelay);
    policy.finalDrainToInputSwitchResetCycles = cycles(
        rtl.saSize + 2 * rtl.schedulerStateRegisterDelay) +
        policy.inputSwitchVisibleDelayCycles;

    policy.ledger = {
        {"storage_beat_bytes", cycles(policy.storage.beatBytes),
         source("SA_CORE.SRAM_DATA_WIDTH / 8")},
        {"storage_issue_width", cycles(policy.storage.issueWidth),
         source("SA_CORE single shared SRAM request interface")},
        {"storage_read_visible",
         policy.storage.readVisibleLatencyCycles,
         source("mem_ctrl.STATE_DELAY = SA_CORE.SRAM_DELAY + 1")},
        {"b_staging", cycles(policy.bStagingBeats),
         source("scheduler.SA_SIZE - B read-ahead window")},
        {"command_start", policy.commandStartCycles,
         source("mem_addr.ADDR_DELAY + command-register edge")},
        {"resident_load", cycles(policy.residentLoadBeats),
         source("CSR register_input.x_burst * y_cycle")},
        {"resident_to_array", policy.arrayInputStartDelayCycles,
         source("command_start + inclusive resident load + runtime read "
                "visibility + register_file_in.PAD_DELAY")},
        {"array_input_a", cycles(policy.arrayInputABeats),
         source("register_file_in initial TRANSPOSE_LOAD row + "
                "feeder.shift_data_cnt SA_SIZE window")},
        {"array_input_b", cycles(policy.arrayInputBBeats),
         source("CSR streamed work-item extent")},
        {"schedule_instructions", cycles(policy.scheduleInstructions),
         source("SA_CORE.scheduler.ins_times_i = vertical_ins_cycle_i")},
        {"early_final_unload", cycles(policy.earlyFinalUnload ? 1 : 0),
         source("scheduler FIRST_INS/INS_LOOP single-counter crossing")},
        {"transpose_burst", cycles(policy.arrayInputBurstBeats),
         source("scheduler.SA_SIZE")},
        {"input_switch_visible", policy.inputSwitchVisibleDelayCycles,
         source("scheduler state register + feeder.STATE_DELAY + "
                "input_switch_d + feeder.REGISTER_DELAY + "
                "input_switch_o")},
        {"input_switch_reset_visible", policy.inputSwitchResetVisibleDelayCycles,
         source("sa_feeder.result_last/update_finished + scheduler "
                "REGISTER_UNLOAD + feeder input-switch pipeline")},
        {"flow_execute", policy.flowExecuteCycles,
         source("scheduler data_last cadence: per-instruction input extent "
                "- SA_SIZE + flow_times_i")},
        {"transpose_burst_gap", policy.arrayInputBurstGapCycles,
         source("register_file_in.ADDR_DELAY")},
        {"flow_boundary_gap", policy.arrayInputFlowGapCycles,
         source("SA_CORE.SRAM_DELAY")},
        {"short_first_flow_gap", policy.firstArrayInputFlowGapCycles,
         source("feeder input-switch visibility + SRAM_DELAY + "
                "register_file_in.ADDR_DELAY + scheduler edge")},
        {"short_second_flow_gap", policy.secondArrayInputFlowGapCycles,
         source("SA_SIZE + feeder input-switch visibility")},
        {"short_steady_flow_gap", policy.steadyArrayInputFlowGapCycles,
         source("SA_SIZE + mem_addr.ADDR_DELAY + feeder.MEMCTRL_DELAY")},
        {"array_fill", policy.arrayFillCycles,
         source("resident CSR extent + two SA dimensions + feeder/output "
                "registered paths + feeder.flow_loop_times_i")},
        {"result_flow_gap", policy.resultFlowGapCycles,
         source("scheduler/feeder output-row retention guard")},
        {"writeback_start", policy.writebackStartDelayCycles,
         source("mem_addr + memctrl + feeder register stages")},
        {"early_unload_writeback_start",
         policy.earlyUnloadWritebackStartDelayCycles,
         source("writeback pipeline with REGISTER_UNLOAD already visible")},
        {"final_drain_to_input_switch_reset",
         policy.finalDrainToInputSwitchResetCycles,
         source("final SA_SIZE D_OUT row + scheduler edges + feeder "
                "input-switch visibility")},
        {"completion", policy.completionDelayCycles,
         source("mem_addr.ADDR_DELAY + feeder.MEMCTRL_DELAY")},
    };
    return policy;
}

Cycles
TimingPolicy::arrayInputFlowGapAfter(uint32_t completedFlows) const
{
    if (completedFlows <= 1) {
        return firstArrayInputFlowGapCycles;
    }
    if (completedFlows == 2) {
        return secondArrayInputFlowGapCycles;
    }
    return steadyArrayInputFlowGapCycles;
}

Cycles
TimingPolicy::shortExecuteCycles(uint32_t instructionIndex) const
{
    return instructionIndex == 0 ? firstShortExecuteCycles :
                                   steadyShortExecuteCycles;
}

Cycles
TimingPolicy::shortDrainCycles(uint32_t instructionIndex) const
{
    if (instructionIndex == 0) {
        return firstShortDrainCycles;
    }
    if (instructionIndex == 1) {
        return secondShortDrainCycles;
    }
    return steadyShortDrainCycles;
}

} // namespace gem5::sau
