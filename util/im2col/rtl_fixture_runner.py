#!/usr/bin/env python3
"""Run one resolved Im2Col fixture through a prebuilt RTL simulation image."""

import argparse
from dataclasses import asdict
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from util.im2col.compare_traces import load_trace
from util.im2col.im2col_contract import TRACE_FIELDS
from util.im2col.im2col_fixture import FixtureError, load_fixture
from util.im2col.logical_oracle import iter_logical_feed_vectors


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
DUT_PATH = (
    REPOSITORY_ROOT
    / "src/sau_n/rtl/gemmini_im2col_chw_gather_readable.sv"
)
TESTBENCH_PATH = (
    REPOSITORY_ROOT
    / "src/sau_n/rtl/tb_gemmini_im2col_chw_gather_readable.sv"
)
FIXED_HARDWARE = {
    "block_size": 16,
    "elem_bits": 8,
    "fifo_depth": 4,
    "sp_bank_entries": 4096,
    "sp_banks": 16,
    "clock_hz": 100_000_000,
    "sram_response": "combinational",
}


class RtlFixtureError(RuntimeError):
    """Raised when an RTL fixture run or its generated trace is invalid."""


def _sha256(path):
    digest = hashlib.sha256()
    try:
        with Path(path).open("rb") as source:
            for chunk in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as error:
        raise RtlFixtureError(f"cannot hash {path}: {error}") from error
    return digest.hexdigest()


def validate_ready(period, high_cycles):
    if type(period) is not int or period < 1:
        raise RtlFixtureError("ready period must be a positive integer")
    if (type(high_cycles) is not int or high_cycles < 1 or
            high_cycles > period):
        raise RtlFixtureError(
            "ready high cycles must be in [1, ready period]")


def build_plusargs(loaded, trace_path, ready_period=1, ready_high_cycles=1):
    """Translate one validated resolved fixture into explicit SV plusargs."""
    validate_ready(ready_period, ready_high_cycles)
    config = loaded.config
    values = (
        ("FIXTURE_NAME", config.name),
        ("TRACE_FILE", str(Path(trace_path).resolve())),
        ("RESOLVED_CONFIG_SHA256", loaded.resolved_config_sha256),
        ("CFG_N", config.n),
        ("CFG_C", config.c),
        ("CFG_H", config.h),
        ("CFG_W", config.w),
        ("CFG_OUT_H", config.out_h),
        ("CFG_OUT_W", config.out_w),
        ("CFG_KERNEL_H", config.kernel_h),
        ("CFG_KERNEL_W", config.kernel_w),
        ("CFG_STRIDE_H", config.stride_h),
        ("CFG_STRIDE_W", config.stride_w),
        ("CFG_DILATION_H", config.dilation_h),
        ("CFG_DILATION_W", config.dilation_w),
        ("CFG_PAD_TOP", config.pad_top),
        ("CFG_PAD_LEFT", config.pad_left),
        ("CFG_SPAD_BASE", config.spad_base),
        ("CFG_DW_MODE", config.cfg_dw_mode),
        ("CFG_KERNEL_PATTERN", f"{config.cfg_kernel_pattern:04x}"),
        ("READY_PERIOD", ready_period),
        ("READY_HIGH_CYCLES", ready_high_cycles),
    )
    return ("+FIXTURE_MODE", *(f"+{key}={value}" for key, value in values))


def _reported_cycle(output, field):
    matches = re.findall(rf"(?m)^{field}=([0-9]+)$", output)
    if len(matches) != 1:
        raise RtlFixtureError(
            f"RTL output must contain exactly one {field}=<cycle> line")
    return int(matches[0])


def _validate_logical_feeds(loaded, trace):
    indices = {field: index for index, field in enumerate(TRACE_FIELDS)}
    actual = []
    for row in trace.rows:
        if (row[indices["feed_valid"]] == "1" and
                row[indices["feed_ready"]] == "1"):
            actual.append((
                int(row[indices["feed_data"]], 16),
                int(row[indices["feed_mask"]], 16),
            ))
    expected = [
        (vector.feed_data, vector.feed_mask)
        for vector in iter_logical_feed_vectors(loaded.config)
    ]
    if len(actual) != len(expected):
        raise RtlFixtureError(
            f"RTL trace has {len(actual)} handshakes; expected "
            f"{len(expected)}")
    for index, (expected_vector, actual_vector) in enumerate(
            zip(expected, actual)):
        if actual_vector != expected_vector:
            raise RtlFixtureError(
                f"RTL logical feed mismatch at handshake {index}: expected "
                f"data=0x{expected_vector[0]:032x} "
                f"mask=0x{expected_vector[1]:04x}, actual "
                f"data=0x{actual_vector[0]:032x} "
                f"mask=0x{actual_vector[1]:04x}")


def validate_rtl_result(loaded, trace_path, output):
    """Validate PASS output, cycle anchors, trace schema, and logical feeds."""
    pass_line = f"PASS fixture {loaded.config.name} vectors="
    if pass_line not in output:
        raise RtlFixtureError(f"RTL output is missing: {pass_line}")
    trace = load_trace(trace_path)
    indices = {field: index for index, field in enumerate(TRACE_FIELDS)}
    for cycle, row in enumerate(trace.rows):
        if row[indices["resolved_config_sha256"]] != (
                loaded.resolved_config_sha256):
            raise RtlFixtureError(
                f"RTL trace config hash differs at cycle {cycle}")

    done_cycle = _reported_cycle(output, "rtl_done_cycle")
    drained_cycle = _reported_cycle(output, "drained_cycle")
    post_done = _reported_cycle(output, "post_done_drain_cycles")
    if drained_cycle != len(trace.rows) - 1:
        raise RtlFixtureError(
            "reported drained cycle does not match the final trace row")
    if post_done != drained_cycle - done_cycle:
        raise RtlFixtureError(
            "reported post-done drain interval is inconsistent")
    done_row = trace.rows[done_cycle]
    if (done_row[indices["state"]] != "0" or
            done_row[indices["busy"]] != "0" or
            done_row[indices["done"]] != "1"):
        raise RtlFixtureError(
            "reported done cycle is not IDLE/busy=0/done=1")
    if done_cycle == 0:
        raise RtlFixtureError("done cycle cannot be cycle zero")
    before_done = trace.rows[done_cycle - 1]
    if (before_done[indices["state"]] != "5" or
            before_done[indices["busy"]] != "1" or
            before_done[indices["done"]] != "0"):
        raise RtlFixtureError(
            "done pulse is not preceded by DONE/busy=1/done=0")
    _validate_logical_feeds(loaded, trace)
    return done_cycle, drained_cycle


def write_manifest(
        path, loaded, fixture_path, trace_path, ready_period,
        ready_high_cycles, simulator_name, simulator_version, command,
        done_cycle, drained_cycle):
    manifest = {
        "schema_version": 1,
        "fixture": str(Path(fixture_path).resolve()),
        "resolved_config": asdict(loaded.config),
        "resolved_config_sha256": loaded.resolved_config_sha256,
        "dut_sha256": _sha256(DUT_PATH),
        "testbench_sha256": _sha256(TESTBENCH_PATH),
        "fixed_hardware": FIXED_HARDWARE,
        "trace_schema_version": 1,
        "trace": str(Path(trace_path).resolve()),
        "ready_period": ready_period,
        "ready_high_cycles": ready_high_cycles,
        "simulator": {
            "name": simulator_name,
            "version": simulator_version,
            "command": list(command),
        },
        "rtl_done_cycle": done_cycle,
        "drained_cycle": drained_cycle,
        "post_done_drain_cycles": drained_cycle - done_cycle,
    }
    manifest_path = Path(path)
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def positive_int(value):
    parsed = int(value, 0)
    if parsed < 1:
        raise argparse.ArgumentTypeError("value must be positive")
    return parsed


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", required=True, type=Path)
    parser.add_argument("--trace", required=True, type=Path)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--ready-period", type=positive_int, default=1)
    parser.add_argument("--ready-high-cycles", type=positive_int, default=1)
    parser.add_argument("--sim-executable", required=True, type=Path)
    parser.add_argument("--sim-arg", action="append", default=[])
    parser.add_argument("--simulator-name", required=True)
    parser.add_argument("--simulator-version", required=True)
    parser.add_argument("--timeout", type=positive_int)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)

    try:
        loaded = load_fixture(args.fixture)
        validate_ready(args.ready_period, args.ready_high_cycles)
    except (FixtureError, RtlFixtureError) as error:
        parser.error(str(error))
    for warning in loaded.warnings:
        print(f"warning: {warning}", file=sys.stderr)
    print(
        f"expected_vectors={loaded.derived.expected_vectors}",
        file=sys.stderr,
    )
    print(f"resolved_config_sha256={loaded.resolved_config_sha256}")

    command = (
        str(args.sim_executable.resolve()), *args.sim_arg,
        *build_plusargs(
            loaded, args.trace, args.ready_period, args.ready_high_cycles),
    )
    print(shlex.join(command))
    if args.dry_run:
        return 0

    args.trace.parent.mkdir(parents=True, exist_ok=True)
    try:
        result = subprocess.run(
            command,
            cwd=REPOSITORY_ROOT,
            check=False,
            capture_output=True,
            text=True,
            timeout=args.timeout,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        print(f"RTL simulator failed to run: {error}", file=sys.stderr)
        return 1
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    if result.returncode != 0:
        print(
            f"RTL simulator exited with status {result.returncode}",
            file=sys.stderr,
        )
        return 1

    try:
        done_cycle, drained_cycle = validate_rtl_result(
            loaded, args.trace, result.stdout + result.stderr)
        manifest_path = args.manifest or args.trace.with_suffix(
            ".manifest.json")
        write_manifest(
            manifest_path, loaded, args.fixture, args.trace,
            args.ready_period, args.ready_high_cycles,
            args.simulator_name, args.simulator_version, command,
            done_cycle, drained_cycle,
        )
    except (OSError, RtlFixtureError, ValueError) as error:
        print(f"RTL fixture validation failed: {error}", file=sys.stderr)
        return 1
    print(
        "RTL fixture validation passed: "
        f"{len(load_trace(args.trace).rows)} cycles"
    )
    print(f"RTL manifest written: {manifest_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
