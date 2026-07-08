import argparse
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

parser.add_argument("--beat-bytes", type=positive_int, default=32)
parser.add_argument("--a-beats", type=positive_int, default=256)
parser.add_argument("--b-beats", type=positive_int, default=256)
parser.add_argument("--output-beats", type=positive_int, default=256)
parser.add_argument("--flow-loops", type=positive_int, default=8)
parser.add_argument("--instruction-loops", type=positive_int, default=1)

parser.add_argument("--a-base", type=nonnegative_int, default=0x1000)
parser.add_argument("--b-base", type=nonnegative_int, default=0x100000)
parser.add_argument("--output-base", type=nonnegative_int, default=0x200000)
parser.add_argument("--a-flow-stride", type=nonnegative_int, default=0)
parser.add_argument("--b-flow-stride", type=nonnegative_int, default=0x10000)
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
parser.add_argument("--command-start-cycles", type=positive_int, default=1)

args = parser.parse_args()

trace = trace_path(args.trace)
trace_dir = os.path.dirname(trace)
if trace_dir:
    os.makedirs(trace_dir, exist_ok=True)

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
    array_input_burst_beats=args.array_input_burst_beats,
    array_input_burst_gap_cycles=args.array_input_burst_gap_cycles,
    array_input_flow_gap_cycles=args.array_input_flow_gap_cycles,
    result_flow_gap_cycles=args.result_flow_gap_cycles,
    writeback_start_delay_cycles=args.writeback_start_delay_cycles,
    completion_delay_cycles=args.completion_delay_cycles,
    command_start_cycles=args.command_start_cycles,
    trace_file=trace,
    a_base=args.a_base,
    b_base=args.b_base,
    output_base=args.output_base,
    a_beats=args.a_beats,
    b_beats=args.b_beats,
    output_beats=args.output_beats,
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
