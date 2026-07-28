import argparse
import json
import os
import sys

import m5
from m5.objects import *


def nonnegative_int(value):
    parsed = int(value, 0)
    if parsed < 0:
        raise argparse.ArgumentTypeError("value must be nonnegative")
    return parsed


def positive_int(value):
    parsed = int(value, 0)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("value must be positive")
    return parsed


def trace_path(path):
    if path:
        return path
    return os.path.join(m5.options.outdir, "sau.csv")


parser = argparse.ArgumentParser(
    description="Run the standalone SAU timing model"
)

parser.add_argument("--sau-clock", default="1GHz")
parser.add_argument("--memory-size", default="16MiB")
parser.add_argument("--memory-latency", default="3ns")
parser.add_argument("--memory-latency-var", default="0ns")
parser.add_argument("--memory-bandwidth", default="256GiB/s")
parser.add_argument("--max-tick", type=positive_int, default=20000000000)
parser.add_argument("--trace", default="")
parser.add_argument(
    "--rtl-profile",
    metavar="CSR_FIXTURE",
    default="",
    help="Run one RTL CSR fixture in strict timing mode",
)
parser.add_argument(
    "--timing-memory",
    action="store_true",
    help=(
        "Replay --rtl-profile commands through SystemXBar and SimpleMemory "
        "for causal/backpressure validation"
    ),
)
parser.add_argument(
    "--fixture-start-policy",
    choices=("raw", "sequential"),
    default="raw",
    help=(
        "Use raw CSR start cycles, or treat fixture commands as templates "
        "submitted only after the prior command is complete and write-visible"
    ),
)

parser.add_argument("--beat-bytes", type=positive_int, default=32)
parser.add_argument("--a-beats", type=positive_int, default=256)
parser.add_argument("--b-beats", type=positive_int, default=256)
parser.add_argument("--output-beats", type=positive_int, default=256)
parser.add_argument("--flow-loops", type=positive_int, default=8)
parser.add_argument("--instruction-loops", type=positive_int, default=1)
parser.add_argument("--command-count", type=positive_int, default=1)
parser.add_argument("--inter-command-gap-cycles", type=nonnegative_int,
                    default=0)

parser.add_argument("--a-base", type=nonnegative_int, default=0x1000)
parser.add_argument("--b-base", type=nonnegative_int, default=0x100000)
parser.add_argument("--output-base", type=nonnegative_int, default=0x200000)
parser.add_argument("--a-command-stride", type=nonnegative_int, default=0)
parser.add_argument("--b-command-stride", type=nonnegative_int, default=0)
parser.add_argument("--output-command-stride", type=nonnegative_int,
                    default=0)
parser.add_argument("--a-flow-stride", type=nonnegative_int, default=0)
parser.add_argument("--b-flow-stride", type=nonnegative_int, default=0x10000)
parser.add_argument("--b-stride-bytes", type=positive_int, default=32)
parser.add_argument(
    "--output-instruction-stride", type=nonnegative_int, default=0x10000
)

parser.add_argument("--read-issue-width", type=positive_int, default=1)
parser.add_argument("--write-issue-width", type=positive_int, default=1)
parser.add_argument("--max-outstanding-reads", type=positive_int, default=4)
parser.add_argument("--max-outstanding-writes", type=positive_int, default=4)
parser.add_argument("--input-buffer-entries", type=positive_int, default=8)
parser.add_argument("--output-buffer-entries", type=positive_int, default=256)

parser.add_argument("--array-capacity", type=positive_int, default=4096)
parser.add_argument("--array-fill-cycles", type=positive_int, default=343)
parser.add_argument("--array-ii-cycles", type=positive_int, default=1)
parser.add_argument(
    "--array-input-start-delay-cycles", type=nonnegative_int, default=269
)
parser.add_argument("--array-input-skew-cycles", type=nonnegative_int,
                    default=32)
parser.add_argument("--b-read-start-ahead-beats", type=nonnegative_int,
                    default=0)
parser.add_argument("--array-input-burst-beats", type=positive_int,
                    default=32)
parser.add_argument("--array-input-burst-gap-cycles", type=nonnegative_int,
                    default=1)
parser.add_argument("--array-input-flow-gap-cycles", type=nonnegative_int,
                    default=3)
parser.add_argument("--result-flow-gap-cycles", type=nonnegative_int,
                    default=234)
parser.add_argument("--writeback-start-delay-cycles", type=nonnegative_int,
                    default=8)
parser.add_argument("--completion-delay-cycles", type=nonnegative_int,
                    default=4)
parser.add_argument(
    "--command-start-cycles",
    type=positive_int,
    default=1,
    help="Command acceptance to first feeder issue",
)
parser.add_argument(
    "--calibration-memory",
    action="store_true",
    help="Use a local fixed-cadence memory model for RTL calibration",
)
parser.add_argument(
    "--calibration-read-latency-cycles",
    type=positive_int,
    default=4,
    help=(
        "Non-strict read accepted-to-visible override for "
        "--calibration-memory; strict fixtures derive SRAM_DELAY + 1"
    ),
)
parser.add_argument(
    "--timing-ledger", default="",
    help="CSV path for strict per-command timing derivations",
)
parser.add_argument(
    "--state-trace", default="",
    help="CSV path for the independent semantic-state debug trace",
)
parser.add_argument(
    "--memory-image", default="",
    help="RTL hex image loaded into the run's functional data authority",
)
parser.add_argument(
    "--memory-image-base", type=nonnegative_int, default=0,
    help="Address of memory image line 0",
)
parser.add_argument(
    "--memory-image-word-bytes", type=positive_int, default=16,
    help="Little-endian word bytes per memory image line",
)
parser.add_argument(
    "--functional-memory-base", type=nonnegative_int, default=0,
    help="Base of the declared functional memory range",
)
parser.add_argument(
    "--functional-memory-size", type=nonnegative_int, default=0,
    help="Functional memory range bytes; 0 disables the data contract",
)
parser.add_argument(
    "--functional-memory-fill", type=nonnegative_int, default=0,
    help="Fill value returned by unwritten functional memory holes",
)
parser.add_argument(
    "--final-memory-dump", default="",
    help="Byte-per-line hex dump of the final memory range",
)
parser.add_argument(
    "--final-memory-dump-base", type=nonnegative_int, default=0,
)
parser.add_argument(
    "--final-memory-dump-size", type=nonnegative_int, default=0,
)
parser.add_argument(
    "--boundary-trace", default="",
    help="Model boundary trace of the first strict command's payload "
         "edges for util/sau/compare_boundary.py",
)

args = parser.parse_args()

if (args.memory_image or args.final_memory_dump) and \
        not args.functional_memory_size:
    parser.error(
        "--memory-image and --final-memory-dump require "
        "--functional-memory-size"
    )
if args.final_memory_dump and not args.final_memory_dump_size:
    parser.error("--final-memory-dump requires --final-memory-dump-size")
if args.boundary_trace and not (args.memory_image and args.rtl_profile):
    parser.error(
        "--boundary-trace requires --rtl-profile and --memory-image"
    )
if args.functional_memory_fill > 0xFF:
    parser.error("--functional-memory-fill must be one byte")

if args.timing_memory and not args.rtl_profile:
    parser.error("--timing-memory requires --rtl-profile")
if args.timing_memory and args.calibration_memory:
    parser.error("--timing-memory cannot be combined with --calibration-memory")
if args.fixture_start_policy == "sequential" and not args.timing_memory:
    parser.error(
        "--fixture-start-policy=sequential requires --timing-memory"
    )

strict_timing_options = {
    "--beat-bytes",
    "--read-issue-width",
    "--write-issue-width",
    "--input-buffer-entries",
    "--array-fill-cycles",
    "--array-ii-cycles",
    "--array-input-start-delay-cycles",
    "--array-input-skew-cycles",
    "--b-read-start-ahead-beats",
    "--array-input-burst-beats",
    "--array-input-burst-gap-cycles",
    "--array-input-flow-gap-cycles",
    "--result-flow-gap-cycles",
    "--writeback-start-delay-cycles",
    "--completion-delay-cycles",
    "--command-start-cycles",
    "--calibration-read-latency-cycles",
}

if args.rtl_profile:
    supplied = {
        token.split("=", 1)[0] for token in sys.argv[1:]
        if token.split("=", 1)[0] in strict_timing_options
    }
    timing_memory_options = {
        "--read-issue-width",
        "--write-issue-width",
        "--input-buffer-entries",
    }
    rejected = supplied
    if args.timing_memory:
        rejected -= timing_memory_options
    if rejected:
        parser.error(
            "--rtl-profile derives internal timing from CSR/RTL and rejects "
            "overrides: " + ", ".join(sorted(rejected))
        )

    fixture = os.path.abspath(args.rtl_profile)
    manifest_path = os.path.join(fixture, "manifest.json")
    try:
        with open(manifest_path, encoding="utf-8") as manifest_file:
            manifest = json.load(manifest_file)
    except (OSError, json.JSONDecodeError) as error:
        parser.error(f"cannot load RTL fixture manifest {manifest_path}: {error}")

    elaboration = manifest.get("elaboration_params", {})
    required = (
        "SA_SIZE", "REGDEPTH", "SRAM_DELAY", "ADDR_DELAY",
        "SRAM_DATA_WIDTH",
    )
    missing = [name for name in required if name not in elaboration]
    if missing:
        parser.error("RTL fixture manifest lacks: " + ", ".join(missing))
    if manifest.get("beat_bytes") != 32 or elaboration["SRAM_DATA_WIDTH"] != 256:
        parser.error("strict SAU fixture must use the supported 256-bit beat")

    args.rtl_profile = fixture
    args.memory_size = "1GiB"
    args.command_count = manifest["command_count"]
    args.calibration_memory = not args.timing_memory
    args.strict_timing = not args.timing_memory
    args.rtl_sa_size = elaboration["SA_SIZE"]
    args.rtl_register_depth = elaboration["REGDEPTH"]
    args.rtl_sram_delay = elaboration["SRAM_DELAY"]
    args.rtl_sram_data_width = elaboration["SRAM_DATA_WIDTH"]
    args.rtl_mem_address_delay = elaboration["ADDR_DELAY"]
    # Keep the generated SimObject configuration descriptive. C++ strict
    # timing independently derives this value from rtl_sram_delay.
    args.calibration_read_latency_cycles = args.rtl_sram_delay + 1
else:
    args.strict_timing = False
    args.rtl_sa_size = 32
    args.rtl_register_depth = 256
    args.rtl_sram_delay = 3
    args.rtl_sram_data_width = 256
    args.rtl_mem_address_delay = 2

trace = trace_path(args.trace)
trace_dir = os.path.dirname(trace)
if trace_dir:
    os.makedirs(trace_dir, exist_ok=True)
if args.rtl_profile and not args.timing_ledger:
    args.timing_ledger = os.path.join(trace_dir or ".", "sau_timing_ledger.csv")
if args.rtl_profile and not args.state_trace:
    args.state_trace = os.path.join(trace_dir or ".", "sau_state.csv")

print(
    "SAU timing mode: " +
    (
        "CSR fixture timing-memory"
        if args.rtl_profile and args.timing_memory
        else "strict CSR fixture"
        if args.rtl_profile
        else "non-strict direct-command/DSE"
    )
)

system = System(
    clk_domain=SrcClockDomain(
        clock=args.sau_clock, voltage_domain=VoltageDomain()
    ),
    mem_mode="timing",
    mem_ranges=[AddrRange(args.memory_size)],
)

system.membus = SystemXBar(width=32)
system.system_port = system.membus.cpu_side_ports

system.physmem = SimpleMemory(
    range=system.mem_ranges[0],
    latency=args.memory_latency,
    latency_var=args.memory_latency_var,
    bandwidth=args.memory_bandwidth,
)
system.physmem.port = system.membus.mem_side_ports

system.sau = SauModel(
    clk_domain=SrcClockDomain(
        clock=args.sau_clock, voltage_domain=system.clk_domain.voltage_domain
    ),
    beat_bytes=args.beat_bytes,
    read_issue_width=args.read_issue_width,
    write_issue_width=args.write_issue_width,
    max_outstanding_reads=args.max_outstanding_reads,
    max_outstanding_writes=args.max_outstanding_writes,
    input_buffer_entries=args.input_buffer_entries,
    output_buffer_entries=args.output_buffer_entries,
    array_capacity=args.array_capacity,
    array_fill_cycles=args.array_fill_cycles,
    array_ii_cycles=args.array_ii_cycles,
    array_input_start_delay_cycles=args.array_input_start_delay_cycles,
    array_input_skew_cycles=args.array_input_skew_cycles,
    b_read_start_ahead_beats=args.b_read_start_ahead_beats,
    array_input_burst_beats=args.array_input_burst_beats,
    array_input_burst_gap_cycles=args.array_input_burst_gap_cycles,
    array_input_flow_gap_cycles=args.array_input_flow_gap_cycles,
    result_flow_gap_cycles=args.result_flow_gap_cycles,
    writeback_start_delay_cycles=args.writeback_start_delay_cycles,
    completion_delay_cycles=args.completion_delay_cycles,
    command_start_cycles=args.command_start_cycles,
    calibration_memory=args.calibration_memory,
    calibration_read_latency_cycles=args.calibration_read_latency_cycles,
    csr_fixture=args.rtl_profile,
    fixture_start_policy=args.fixture_start_policy,
    strict_timing=args.strict_timing,
    rtl_sa_size=args.rtl_sa_size,
    rtl_register_depth=args.rtl_register_depth,
    rtl_sram_delay=args.rtl_sram_delay,
    rtl_sram_data_width=args.rtl_sram_data_width,
    rtl_mem_address_delay=args.rtl_mem_address_delay,
    timing_ledger_file=args.timing_ledger,
    state_trace_file=args.state_trace,
    trace_file=trace,
    memory_image_file=args.memory_image,
    memory_image_base=args.memory_image_base,
    memory_image_word_bytes=args.memory_image_word_bytes,
    functional_memory_base=args.functional_memory_base,
    functional_memory_size=args.functional_memory_size,
    functional_memory_fill=args.functional_memory_fill,
    final_memory_dump_file=args.final_memory_dump,
    final_memory_dump_base=args.final_memory_dump_base,
    final_memory_dump_size=args.final_memory_dump_size,
    boundary_trace_file=args.boundary_trace,
    command_count=args.command_count,
    inter_command_gap_cycles=args.inter_command_gap_cycles,
    a_base=args.a_base,
    b_base=args.b_base,
    output_base=args.output_base,
    a_command_stride=args.a_command_stride,
    b_command_stride=args.b_command_stride,
    output_command_stride=args.output_command_stride,
    a_beats=args.a_beats,
    b_beats=args.b_beats,
    output_beats=args.output_beats,
    b_stride_bytes=args.b_stride_bytes,
    flow_loops=args.flow_loops,
    instruction_loops=args.instruction_loops,
    a_flow_stride=args.a_flow_stride,
    b_flow_stride=args.b_flow_stride,
    output_instruction_stride=args.output_instruction_stride,
)
system.sau.memory = system.membus.cpu_side_ports

root = Root(full_system=False, system=system)

m5.instantiate()
exit_event = m5.simulate(args.max_tick)
cause = exit_event.getCause()
print(f"SAU timing simulation exited: {cause}")

if cause != "SAU command complete":
    print(f"unexpected exit cause: {cause}", file=sys.stderr)
    sys.exit(1)

if not os.path.isfile(trace) or os.path.getsize(trace) == 0:
    print(f"SAU trace was not created: {trace}", file=sys.stderr)
    sys.exit(1)

with open(trace, encoding="utf-8") as trace_file:
    if "command_complete" not in trace_file.read():
        print(f"SAU trace is missing command_complete: {trace}",
              file=sys.stderr)
        sys.exit(1)

print(f"SAU trace written: {trace}")
