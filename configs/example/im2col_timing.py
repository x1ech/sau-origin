import argparse
from pathlib import Path
import sys

import m5
from m5.objects import Im2ColTiming, Root, SrcClockDomain, VoltageDomain


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
if str(REPOSITORY_ROOT) not in sys.path:
    sys.path.insert(0, str(REPOSITORY_ROOT))

from util.im2col.im2col_fixture import FixtureError, load_fixture


def positive_int(value):
    parsed = int(value, 0)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("value must be positive")
    return parsed


parser = argparse.ArgumentParser(
    description="Run the standalone Im2Col reference timing model"
)
parser.add_argument("--fixture", required=True, type=Path)
parser.add_argument("--clock", default="100MHz")
parser.add_argument(
    "--trace",
    default="",
    help="CSV trace path (default: <outdir>/im2col/trace.csv)",
)
parser.add_argument("--ready-period", type=positive_int, default=1)
parser.add_argument("--ready-high-cycles", type=positive_int, default=1)
args = parser.parse_args()

if args.ready_high_cycles > args.ready_period:
    parser.error("--ready-high-cycles must not exceed --ready-period")

try:
    loaded = load_fixture(args.fixture)
except FixtureError as error:
    parser.error(str(error))

for warning in loaded.warnings:
    print(f"warning: {warning}", file=sys.stderr)
print(f"expected_vectors={loaded.derived.expected_vectors}", file=sys.stderr)
print(f"resolved_config_sha256={loaded.resolved_config_sha256}")

config = loaded.config
trace_path = Path(args.trace) if args.trace else (
    Path(m5.options.outdir) / "im2col" / "trace.csv"
)
trace_path.parent.mkdir(parents=True, exist_ok=True)
clock_domain = SrcClockDomain(
    clock=args.clock,
    voltage_domain=VoltageDomain(),
)

root = Root(full_system=False)
root.im2col = Im2ColTiming(
    clk_domain=clock_domain,
    schema_version=config.schema_version,
    fixture_name=config.name,
    n=config.n,
    c=config.c,
    h=config.h,
    w=config.w,
    out_h=config.out_h,
    out_w=config.out_w,
    kernel_h=config.kernel_h,
    kernel_w=config.kernel_w,
    stride_h=config.stride_h,
    stride_w=config.stride_w,
    dilation_h=config.dilation_h,
    dilation_w=config.dilation_w,
    pad_top=config.pad_top,
    pad_left=config.pad_left,
    spad_base=config.spad_base,
    cfg_dw_mode=config.cfg_dw_mode,
    cfg_kernel_pattern=config.cfg_kernel_pattern,
    input_generator=config.input_generator,
    resolved_config_sha256=loaded.resolved_config_sha256,
    trace_file=str(trace_path),
    ready_period=args.ready_period,
    ready_high_cycles=args.ready_high_cycles,
)

m5.instantiate()
exit_event = m5.simulate()
cause = exit_event.getCause()
m5.stats.dump()

print(f"Im2Col timing simulation exited: {cause}")
if cause != "im2col model drained":
    print(f"unexpected exit cause: {cause}", file=sys.stderr)
    sys.exit(1)
if not trace_path.is_file() or trace_path.stat().st_size == 0:
    print(f"Im2Col trace was not created: {trace_path}", file=sys.stderr)
    sys.exit(1)
print(f"Im2Col trace written: {trace_path}")
