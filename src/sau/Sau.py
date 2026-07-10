from m5.objects.ClockedObject import ClockedObject
from m5.params import *
from m5.proxy import *


class SauModel(ClockedObject):
    type = "SauModel"
    cxx_class = "gem5::sau::SauModel"
    cxx_header = "sau/sau_model.hh"

    system = Param.System(Parent.any, "System used for requestor IDs")
    memory = RequestPort("SAU timing-memory request port")

    beat_bytes = Param.Unsigned(32, "SRAM beat size")
    read_issue_width = Param.Unsigned(1, "Maximum accepted reads per cycle")
    write_issue_width = Param.Unsigned(1, "Maximum accepted writes per cycle")
    max_outstanding_reads = Param.Unsigned(4, "Read response slots")
    max_outstanding_writes = Param.Unsigned(4, "Write response slots")
    input_buffer_entries = Param.Unsigned(8, "Returned operand token slots")
    output_buffer_entries = Param.Unsigned(256, "Result token slots")
    array_fill_cycles = Param.Cycles(343, "Calibrated fill latency")
    array_ii_cycles = Param.Cycles(1, "Array initiation interval")
    array_capacity = Param.Unsigned(4096, "Maximum in-flight work tokens")
    array_input_start_delay_cycles = Param.Cycles(
        269, "Command acceptance to first A array-input eligibility"
    )
    array_input_burst_beats = Param.Unsigned(
        32, "Array-input burst length before transpose gap"
    )
    array_input_burst_gap_cycles = Param.Cycles(
        1, "Idle cycles between array-input bursts inside a flow"
    )
    array_input_flow_gap_cycles = Param.Cycles(
        3, "Idle cycles at array-input flow boundaries"
    )
    array_input_skew_cycles = Param.Unsigned(
        32, "B array-input token skew behind resident A"
    )
    b_read_start_ahead_beats = Param.Unsigned(
        0, "Resident A array-input lead before external B reads may start"
    )
    result_flow_gap_cycles = Param.Cycles(
        234, "Idle cycles between 32-result flow bursts"
    )
    writeback_start_delay_cycles = Param.Cycles(
        8, "Delay from last result to first writeback beat"
    )
    completion_delay_cycles = Param.Cycles(
        4, "Delay from last writeback beat to command complete"
    )
    command_start_cycles = Param.Cycles(
        1, "Command acceptance to first feeder issue"
    )
    calibration_memory = Param.Bool(
        False, "Use local fixed-cadence memory for RTL timing calibration"
    )
    calibration_read_latency_cycles = Param.Cycles(
        4, "Fixed read accepted-to-visible latency in calibration-memory mode"
    )
    trace_file = Param.String("", "CSV timing trace path")
    exit_on_done = Param.Bool(
        True, "Exit simulation when the synthetic command completes"
    )

    command_id = Param.UInt64(1, "Synthetic command ID")
    command_count = Param.Unsigned(1, "Number of synthetic commands")
    inter_command_gap_cycles = Param.Cycles(
        0, "Delay from one command_complete to the next command_accepted"
    )
    a_base = Param.Addr(0x1000, "Operand A base")
    b_base = Param.Addr(0x2000, "Operand B base")
    output_base = Param.Addr(0x3000, "Output base")
    a_command_stride = Param.Unsigned(0, "A base stride between commands")
    b_command_stride = Param.Unsigned(0, "B base stride between commands")
    output_command_stride = Param.Unsigned(
        0, "Output base stride between commands"
    )
    a_beats = Param.Unsigned(4, "Operand A beats per flow")
    b_beats = Param.Unsigned(4, "Operand B beats per flow")
    output_beats = Param.Unsigned(4, "Output beats per instruction")
    b_stride_bytes = Param.Unsigned(32, "Operand B bytes between beat indices")
    flow_loops = Param.Unsigned(1, "Flow repetitions")
    instruction_loops = Param.Unsigned(1, "Instruction repetitions")
    a_flow_stride = Param.Unsigned(0x100, "A bytes between flows")
    b_flow_stride = Param.Unsigned(0x100, "B bytes between flows")
    output_instruction_stride = Param.Unsigned(
        0x1000, "Output bytes between instructions"
    )
