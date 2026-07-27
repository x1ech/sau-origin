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
    input_buffer_entries = Param.Unsigned(
        8, "Non-strict returned plus in-flight Operand-B token slots"
    )
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
        4, "Non-strict fixed-memory read latency override"
    )
    csr_fixture = Param.String(
        "", "Directory containing an RTL csr_writes.csv fixture"
    )
    strict_timing = Param.Bool(
        False, "Derive timing only from the CSR fixture and named RTL parameters"
    )
    rtl_sa_size = Param.Unsigned(32, "RTL scheduler SA_SIZE")
    rtl_register_depth = Param.Unsigned(256, "RTL register-file depth")
    rtl_sram_delay = Param.Unsigned(3, "RTL SA_CORE SRAM_DELAY")
    rtl_sram_data_width = Param.Unsigned(256, "RTL SRAM_DATA_WIDTH in bits")
    rtl_mem_address_delay = Param.Unsigned(2, "RTL SA_CORE ADDR_DELAY")
    rtl_memctrl_delay = Param.Unsigned(2, "RTL feeder MEMCTRL_DELAY")
    rtl_register_file_address_delay = Param.Unsigned(
        1, "RTL register_file_in ADDR_DELAY"
    )
    rtl_register_delay = Param.Unsigned(2, "RTL feeder REGISTER_DELAY")
    timing_ledger_file = Param.String("", "Per-command timing derivation CSV")
    state_trace_file = Param.String(
        "", "Independent semantic-state debug trace CSV"
    )
    trace_file = Param.String("", "CSV timing trace path")
    exit_on_done = Param.Bool(
        True, "Exit simulation when the synthetic command completes"
    )

    memory_image_file = Param.String(
        "", "RTL hex image loaded into the functional data authority"
    )
    memory_image_base = Param.Addr(0, "Address of memory image line 0")
    memory_image_word_bytes = Param.Unsigned(
        16, "Little-endian word bytes per memory image line"
    )
    functional_memory_base = Param.Addr(
        0, "Base of the declared functional memory range"
    )
    functional_memory_size = Param.UInt64(
        0, "Functional memory range bytes; 0 disables the data contract"
    )
    functional_memory_fill = Param.Unsigned(
        0, "Fill value returned by unwritten functional memory holes"
    )
    final_memory_dump_file = Param.String(
        "", "Byte-per-line hex dump of the final memory range"
    )
    final_memory_dump_base = Param.Addr(0, "Final dump range base")
    final_memory_dump_size = Param.UInt64(0, "Final dump range bytes")
    boundary_trace_file = Param.String(
        "",
        "Model boundary trace of the first strict command's payload "
        "edges for compare_boundary.py; empty disables it",
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
