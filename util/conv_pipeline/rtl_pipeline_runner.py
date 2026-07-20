#!/usr/bin/env python3
"""Run one pipeline fixture through a prebuilt RTL simulation image."""

import argparse
import csv
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

from util.conv_pipeline.compare_pipeline_traces import (
    TRACE_FIELDS,
    load_trace,
)
from util.conv_pipeline.convolution_oracle import generate_convolution
from util.conv_pipeline.pipeline_contract import (
    OutputReadyConfig,
    validate_output_ready,
)
from util.conv_pipeline.pipeline_fixture import FixtureError, load_fixture


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
RTL_SOURCES = (
    "src/sau_n/rtl/mikui/filelists/pipeline.f",
    "src/sau_n/rtl/gemmini_im2col_chw_gather_readable.sv",
    "src/sau_n/rtl/sau_array_16x16.sv",
    "src/sau_n/rtl/sau_array_16x16.f",
    "src/sau_n/rtl/sau_array_provenance.json",
    "src/sau_n/rtl/tb_sau_array_16x16.sv",
    "src/sau_n/rtl/im2col_mikui_sau_pipeline.sv",
    "src/sau_n/rtl/tb_im2col_mikui_sau_pipeline.sv",
    "util/conv_pipeline/array/verify_sau_array_trace.py",
)
WEIGHT_GENERATOR_CODES = {
    "tb_weight_value_v1": 0,
    "zero": 1,
    "ones": 2,
}


class RtlPipelineError(RuntimeError):
    """Raised when a pipeline RTL run or artifact is invalid."""


def sha256_file(path):
    digest = hashlib.sha256()
    try:
        with Path(path).open("rb") as source:
            for chunk in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as error:
        raise RtlPipelineError(f"cannot hash {path}: {error}") from error
    return digest.hexdigest()


def build_plusargs(
        loaded, trace_path, output_path, output_ready_period=1,
        output_ready_high_cycles=1):
    ready = OutputReadyConfig(
        period=output_ready_period,
        high_cycles=output_ready_high_cycles,
    )
    validate_output_ready(ready)
    config = loaded.config
    im2col = config.im2col
    values = (
        ("FIXTURE_NAME", config.name),
        ("TRACE_FILE", str(Path(trace_path).resolve())),
        ("OUTPUT_FILE", str(Path(output_path).resolve())),
        ("RESOLVED_CONFIG_SHA256", loaded.resolved_config_sha256),
        ("CFG_N", im2col.n),
        ("CFG_C", im2col.c),
        ("CFG_H", im2col.h),
        ("CFG_W", im2col.w),
        ("CFG_OUT_H", im2col.out_h),
        ("CFG_OUT_W", im2col.out_w),
        ("CFG_KERNEL_H", im2col.kernel_h),
        ("CFG_KERNEL_W", im2col.kernel_w),
        ("CFG_STRIDE_H", im2col.stride_h),
        ("CFG_STRIDE_W", im2col.stride_w),
        ("CFG_DILATION_H", im2col.dilation_h),
        ("CFG_DILATION_W", im2col.dilation_w),
        ("CFG_PAD_TOP", im2col.pad_top),
        ("CFG_PAD_LEFT", im2col.pad_left),
        ("CFG_SPAD_BASE", im2col.spad_base),
        ("CFG_OUT_CHANNELS", config.out_channels),
        ("CFG_CUTBIT", config.cutbit),
        ("CFG_WEIGHT_GENERATOR",
         WEIGHT_GENERATOR_CODES[config.weight_generator]),
        ("CFG_BIAS_ZERO", int(config.bias_generator == "zero")),
        ("EXPECTED_TILES", loaded.derived.expected_tiles),
        ("EXPECTED_OUTPUTS", loaded.derived.expected_outputs),
        ("OUTPUT_READY_PERIOD", ready.period),
        ("OUTPUT_READY_HIGH_CYCLES", ready.high_cycles),
    )
    return tuple(f"+{key}={value}" for key, value in values)


def _reported_cycle(output, field):
    matches = re.findall(rf"(?m)^{field}=(-?[0-9]+)$", output)
    if len(matches) != 1:
        raise RtlPipelineError(
            f"RTL output must contain exactly one {field}=<cycle> line")
    value = int(matches[0])
    if value < 0:
        raise RtlPipelineError(f"RTL did not observe {field}")
    return value


def validate_output_file(loaded, output_path):
    """Validate strict NCHW CSV order and every value against the oracle."""
    path = Path(output_path)
    try:
        raw = path.read_bytes()
    except OSError as error:
        raise RtlPipelineError(f"cannot read RTL output {path}: {error}") \
            from error
    if not raw.endswith(b"\n") or b"\r" in raw:
        raise RtlPipelineError("RTL output must use LF and end with LF")
    try:
        records = list(csv.reader(raw.decode("utf-8").splitlines()))
    except (UnicodeDecodeError, csv.Error) as error:
        raise RtlPipelineError(f"invalid RTL output CSV: {error}") from error
    if not records or records[0] != ["n", "oc", "oh", "ow", "value"]:
        raise RtlPipelineError("RTL output has an unexpected CSV header")
    oracle = generate_convolution(loaded.config)
    if len(records) - 1 != len(oracle.outputs):
        raise RtlPipelineError(
            f"RTL output has {len(records) - 1} elements; expected "
            f"{len(oracle.outputs)}")
    for index, (record, expected) in enumerate(
            zip(records[1:], oracle.outputs)):
        if len(record) != 5:
            raise RtlPipelineError(
                f"RTL output row {index + 2} must have five fields")
        try:
            actual = tuple(int(value, 10) for value in record)
        except ValueError as error:
            raise RtlPipelineError(
                f"RTL output row {index + 2} is not decimal") from error
        expected_row = (
            expected.n, expected.output_channel, expected.oh, expected.ow,
            expected.value,
        )
        if actual != expected_row:
            raise RtlPipelineError(
                f"RTL output mismatch at flat index {index}: expected "
                f"{expected_row}, actual {actual}")
    return len(oracle.outputs)


def validate_commit_contract(loaded, trace):
    """Check every fused tile has exactly K MACs per valid physical PE."""
    indices = {field: index for index, field in enumerate(TRACE_FIELDS)}
    active = None
    completed_tiles = 0
    observed_mac_commits = 0
    for cycle, record in enumerate(trace.rows):
        row = {field: record[index] for field, index in indices.items()}
        if row["pe_valid_mask"] != row["pe_mac_commit_mask"]:
            raise RtlPipelineError(
                f"cycle {cycle}: pe_valid_mask differs from MAC commits")
        if row["sa_ins_valid"] == "1":
            if active is not None:
                raise RtlPipelineError(
                    f"cycle {cycle}: SA launch overlaps an active tile")
            active = {
                "k": int(row["sa_calc_cycles"]),
                "rows": int(row["sa_valid_rows"]),
                "cols": int(row["sa_valid_columns"]),
                "inputs": 0,
                "outputs": 0,
                "macs": [0] * 256,
                "adds": [0] * 256,
            }
        mac_mask = int(row["pe_mac_commit_mask"], 16)
        add_mask = int(row["pe_add_commit_mask"], 16)
        if (mac_mask or add_mask or row["sa_input_valid"] == "1" or
                row["output_collected"] == "1") and active is None:
            raise RtlPipelineError(
                f"cycle {cycle}: array activity occurs outside a tile")
        if active is not None:
            active["inputs"] += int(row["sa_input_valid"])
            active["outputs"] += int(row["output_collected"])
            for pe in range(256):
                active["macs"][pe] += (mac_mask >> pe) & 1
                active["adds"][pe] += (add_mask >> pe) & 1
        if row["cal_finish"] == "1":
            if active is None:
                raise RtlPipelineError(
                    f"cycle {cycle}: cal_finish occurs without an active tile")
            if active["inputs"] != active["k"]:
                raise RtlPipelineError(
                    f"tile {completed_tiles}: expected {active['k']} input "
                    f"cycles, got {active['inputs']}")
            if active["outputs"] != active["rows"]:
                raise RtlPipelineError(
                    f"tile {completed_tiles}: expected {active['rows']} "
                    f"output rows, got {active['outputs']}")
            for pe in range(256):
                pe_row, pe_col = divmod(pe, 16)
                valid = pe_row < active["rows"] and pe_col < active["cols"]
                expected_macs = active["k"] if valid else 0
                expected_adds = 1 if valid else 0
                if active["macs"][pe] != expected_macs:
                    raise RtlPipelineError(
                        f"tile {completed_tiles} PE[{pe_row}][{pe_col}] "
                        f"MAC commits: expected {expected_macs}, got "
                        f"{active['macs'][pe]}")
                if active["adds"][pe] != expected_adds:
                    raise RtlPipelineError(
                        f"tile {completed_tiles} PE[{pe_row}][{pe_col}] "
                        f"bias commits: expected {expected_adds}, got "
                        f"{active['adds'][pe]}")
                observed_mac_commits += active["macs"][pe]
            completed_tiles += 1
            active = None
    if active is not None:
        raise RtlPipelineError("final fused tile never completed")
    if completed_tiles != loaded.derived.expected_tiles:
        raise RtlPipelineError(
            f"completed tiles: expected {loaded.derived.expected_tiles}, "
            f"got {completed_tiles}")
    if observed_mac_commits != loaded.derived.expected_macs:
        raise RtlPipelineError(
            f"MAC commits: expected {loaded.derived.expected_macs}, got "
            f"{observed_mac_commits}")
    return completed_tiles, observed_mac_commits


def validate_rtl_result(loaded, trace_path, output_path, simulator_output):
    pass_line = f"PASS pipeline fixture {loaded.config.name} outputs="
    if pass_line not in simulator_output:
        raise RtlPipelineError(f"RTL output is missing: {pass_line}")
    trace = load_trace(trace_path)
    completed_tiles, observed_mac_commits = validate_commit_contract(
        loaded, trace)
    indices = {field: index for index, field in enumerate(TRACE_FIELDS)}
    for cycle, row in enumerate(trace.rows):
        if row[indices["resolved_config_sha256"]] != (
                loaded.resolved_config_sha256):
            raise RtlPipelineError(
                f"RTL trace config hash differs at cycle {cycle}")
    im2col_done = _reported_cycle(simulator_output, "im2col_done_cycle")
    last_result = _reported_cycle(
        simulator_output, "sau_last_result_cycle")
    drained = _reported_cycle(
        simulator_output, "pipeline_drained_cycle")
    if drained != len(trace.rows) - 1:
        raise RtlPipelineError(
            "reported drained cycle does not match final trace row")
    if trace.rows[im2col_done][indices["im2col_done"]] != "1":
        raise RtlPipelineError("reported Im2Col done cycle has no done pulse")
    if trace.rows[last_result][indices["sau_last_result"]] != "1":
        raise RtlPipelineError(
            "reported SA last-result cycle has no last-result pulse")
    if trace.rows[drained][indices["drained"]] != "1":
        raise RtlPipelineError("reported drained cycle is not drained")
    output_elements = validate_output_file(loaded, output_path)
    return {
        "im2col_done_cycle": im2col_done,
        "sau_last_result_cycle": last_result,
        "pipeline_drained_cycle": drained,
        "post_im2col_drain_cycles": drained - im2col_done,
        "output_elements": output_elements,
        "completed_tiles": completed_tiles,
        "observed_mac_commits": observed_mac_commits,
        "trace_cycles": len(trace.rows),
    }


def write_manifest(
        path, loaded, fixture_path, trace_path, output_path,
        output_ready_period, output_ready_high_cycles, simulator_name,
        simulator_version, command, anchors):
    sources = {
        relative: sha256_file(REPOSITORY_ROOT / relative)
        for relative in RTL_SOURCES
    }
    manifest = {
        "schema_version": 1,
        "status": "rtl_pipeline_fixture_passed",
        "fixture": str(Path(fixture_path).resolve()),
        "resolved_config": asdict(loaded.config),
        "resolved_config_sha256": loaded.resolved_config_sha256,
        "derived": asdict(loaded.derived),
        "runtime": {
            "output_ready_period": output_ready_period,
            "output_ready_high_cycles": output_ready_high_cycles,
        },
        "sources": sources,
        "trace_schema_version": 1,
        "trace": str(Path(trace_path).resolve()),
        "trace_sha256": sha256_file(trace_path),
        "output": str(Path(output_path).resolve()),
        "output_sha256": sha256_file(output_path),
        "anchors": dict(anchors),
        "simulator": {
            "name": simulator_name,
            "version": simulator_version,
            "command": list(command),
        },
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
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--output-ready-period", type=positive_int, default=1)
    parser.add_argument(
        "--output-ready-high-cycles", type=positive_int, default=1)
    parser.add_argument("--sim-executable", required=True, type=Path)
    parser.add_argument("--sim-arg", action="append", default=[])
    parser.add_argument("--simulator-name", required=True)
    parser.add_argument("--simulator-version", required=True)
    parser.add_argument("--timeout", type=positive_int)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)

    try:
        loaded = load_fixture(args.fixture)
        plusargs = build_plusargs(
            loaded, args.trace, args.output, args.output_ready_period,
            args.output_ready_high_cycles)
    except (FixtureError, ValueError) as error:
        parser.error(str(error))
    command = (
        str(args.sim_executable.resolve()), *args.sim_arg, *plusargs)
    print(f"resolved_config_sha256={loaded.resolved_config_sha256}")
    print(shlex.join(command))
    if args.dry_run:
        return 0

    args.trace.parent.mkdir(parents=True, exist_ok=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
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
        anchors = validate_rtl_result(
            loaded, args.trace, args.output,
            result.stdout + result.stderr)
        write_manifest(
            args.manifest, loaded, args.fixture, args.trace, args.output,
            args.output_ready_period, args.output_ready_high_cycles,
            args.simulator_name, args.simulator_version, command, anchors)
    except (OSError, RtlPipelineError, ValueError) as error:
        print(f"RTL pipeline validation failed: {error}", file=sys.stderr)
        return 1
    print(f"RTL pipeline manifest written: {args.manifest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
