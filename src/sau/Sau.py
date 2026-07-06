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
    output_buffer_entries = Param.Unsigned(8, "Result token slots")
    array_fill_cycles = Param.Cycles(1, "Calibrated fill latency")
    array_ii_cycles = Param.Cycles(1, "Array initiation interval")
    array_capacity = Param.Unsigned(16, "Maximum in-flight work tokens")
    command_start_cycles = Param.Cycles(
        1, "Command acceptance to first issue"
    )
    trace_file = Param.String("", "CSV timing trace path")
    exit_on_done = Param.Bool(
        True, "Exit simulation when the synthetic command completes"
    )

    command_id = Param.UInt64(1, "Synthetic command ID")
    a_base = Param.Addr(0x1000, "Operand A base")
    b_base = Param.Addr(0x2000, "Operand B base")
    output_base = Param.Addr(0x3000, "Output base")
    a_beats = Param.Unsigned(4, "Operand A beats per flow")
    b_beats = Param.Unsigned(4, "Operand B beats per flow")
    output_beats = Param.Unsigned(4, "Output beats per instruction")
    flow_loops = Param.Unsigned(1, "Flow repetitions")
    instruction_loops = Param.Unsigned(1, "Instruction repetitions")
    a_flow_stride = Param.Unsigned(0x100, "A bytes between flows")
    b_flow_stride = Param.Unsigned(0x100, "B bytes between flows")
    output_instruction_stride = Param.Unsigned(
        0x1000, "Output bytes between instructions"
    )
