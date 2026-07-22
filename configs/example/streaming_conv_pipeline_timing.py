import argparse
from pathlib import Path
import sys

import m5
from m5.objects import (
    Root,
    SrcClockDomain,
    StreamingConvPipelineTiming,
    VoltageDomain,
)


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
if str(REPOSITORY_ROOT) not in sys.path:
    sys.path.insert(0, str(REPOSITORY_ROOT))

from util.conv_pipeline.pipeline_contract import PipelineConfigError
from util.conv_pipeline.streaming_fixture import (
    StreamingFixtureError,
    load_streaming_fixture,
)
from util.conv_pipeline.streaming_gem5_config import (
    streaming_simobject_parameters,
)


def positive_int(value):
    parsed = int(value, 0)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("value must be positive")
    return parsed


parser = argparse.ArgumentParser(
    description="Run the streaming Im2Col-to-SA exploration model"
)
parser.add_argument("--fixture", required=True, type=Path)
parser.add_argument(
    "--trace",
    default="",
    help=(
        "Cycle CSV path "
        "(default: <outdir>/streaming_conv_pipeline/trace.csv)"
    ),
)
parser.add_argument(
    "--output",
    default="",
    help=(
        "NCHW output CSV path "
        "(default: <outdir>/streaming_conv_pipeline/output.csv)"
    ),
)
parser.add_argument(
    "--detailed-pe-trace",
    action="store_true",
    help="Append detailed 256-PE snapshots to the control trace",
)
parser.add_argument("--output-ready-period", type=positive_int, default=1)
parser.add_argument(
    "--output-ready-high-cycles", type=positive_int, default=1
)
args = parser.parse_args()

try:
    loaded = load_streaming_fixture(args.fixture)
except StreamingFixtureError as error:
    parser.error(str(error))

for warning in loaded.warnings:
    print(f"warning: {warning}", file=sys.stderr)
print(f"expected_tiles={loaded.derived.expected_tiles}", file=sys.stderr)
print(f"expected_outputs={loaded.derived.expected_outputs}", file=sys.stderr)
print(f"expected_macs={loaded.derived.expected_macs}", file=sys.stderr)
print(f"resolved_config_sha256={loaded.resolved_config_sha256}")

output_directory = Path(m5.options.outdir) / "streaming_conv_pipeline"
trace_path = Path(args.trace) if args.trace else output_directory / "trace.csv"
output_path = (
    Path(args.output) if args.output else output_directory / "output.csv"
)
trace_path.parent.mkdir(parents=True, exist_ok=True)
output_path.parent.mkdir(parents=True, exist_ok=True)

try:
    timing_params = streaming_simobject_parameters(
        loaded,
        trace_path,
        output_path,
        args.output_ready_period,
        args.output_ready_high_cycles,
        args.detailed_pe_trace,
    )
except (PipelineConfigError, ValueError) as error:
    parser.error(str(error))

clock_domain = SrcClockDomain(
    clock="100MHz",
    voltage_domain=VoltageDomain(),
)
root = Root(full_system=False)
root.streaming_conv_pipeline = StreamingConvPipelineTiming(
    clk_domain=clock_domain,
    **timing_params,
)

m5.instantiate()
exit_event = m5.simulate()
cause = exit_event.getCause()
m5.stats.dump()

print(f"Streaming conv pipeline simulation exited: {cause}")
if cause != "streaming conv pipeline drained":
    print(f"unexpected exit cause: {cause}", file=sys.stderr)
    sys.exit(1)
for label, path in (("trace", trace_path), ("output", output_path)):
    if not path.is_file() or path.stat().st_size == 0:
        print(
            f"Streaming conv pipeline {label} was not created: {path}",
            file=sys.stderr,
        )
        sys.exit(1)
    print(f"Streaming conv pipeline {label} written: {path}")
