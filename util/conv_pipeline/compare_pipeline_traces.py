#!/usr/bin/env python3
"""Strict comparator for canonical Im2Col-to-SAU pipeline traces."""

import argparse
import csv
from dataclasses import dataclass
from pathlib import Path
import re
import sys


TRACE_FIELDS = (
    "schema_version", "resolved_config_sha256", "cycle",
    "pipeline_state", "tile_index", "tile_buffer_count", "collect_k",
    "stream_k", "im2col_state", "im2col_done", "im2col_fifo_count",
    "im2col_fifo_rptr", "im2col_fifo_wptr", "im2col_feed_valid",
    "im2col_feed_ready", "im2col_feed_handshake", "im2col_feed_data",
    "im2col_feed_mask", "sa_ins_valid", "sa_calc_cycles",
    "sa_valid_rows", "sa_valid_columns", "sa_cutbit", "sa_biases",
    "sa_state", "sa_datain_count", "sa_output_counter",
    "sa_input_valid", "sa_activations", "sa_weights", "sa_row_mask",
    "sa_column_mask", "pe_valid_mask", "pe_mac_commit_mask",
    "pe_add_commit_mask", "pe_activations", "pe_weights",
    "pe_accumulators", "os_valid_mask", "row_ready_mask", "pe_finish",
    "storage_ready", "output_request", "output_grant",
    "internal_output_valid", "engine_output_fire", "row_score_valid",
    "row_sequence", "output_slots", "cal_finish", "output_collected",
    "sau_last_result", "drained",
)

HEX_DIGITS = {
    "im2col_feed_data": 32,
    "im2col_feed_mask": 4,
    "sa_biases": 64,
    "sa_activations": 32,
    "sa_weights": 32,
    "sa_row_mask": 4,
    "sa_column_mask": 4,
    "pe_valid_mask": 64,
    "pe_mac_commit_mask": 64,
    "pe_add_commit_mask": 64,
    "pe_activations": 512,
    "pe_weights": 512,
    "pe_accumulators": 1536,
    "os_valid_mask": 4,
    "row_ready_mask": 4,
    "output_slots": 64,
}

BOOL_FIELDS = (
    "im2col_done", "im2col_feed_valid", "im2col_feed_ready",
    "im2col_feed_handshake", "sa_ins_valid", "sa_input_valid",
    "pe_finish", "storage_ready", "output_request", "output_grant",
    "internal_output_valid", "engine_output_fire", "row_score_valid",
    "cal_finish", "output_collected", "sau_last_result", "drained",
)

DECIMAL_FIELDS = (
    "cycle", "tile_index", "tile_buffer_count", "collect_k", "stream_k",
    "im2col_fifo_count", "im2col_fifo_rptr", "im2col_fifo_wptr",
    "sa_calc_cycles", "sa_valid_rows", "sa_valid_columns", "sa_cutbit",
    "sa_datain_count", "sa_output_counter", "row_sequence",
)

PE_PACKED_FIELDS = {
    "pe_valid_mask": 1,
    "pe_mac_commit_mask": 1,
    "pe_add_commit_mask": 1,
    "pe_activations": 8,
    "pe_weights": 8,
    "pe_accumulators": 24,
}

UINT64_MAX = (1 << 64) - 1
PE_COUNT = 256


class TraceFormatError(ValueError):
    """Raised when one trace violates the canonical pipeline schema."""


class TraceMismatch(AssertionError):
    """Raised at the first difference between two valid pipeline traces."""


@dataclass(frozen=True)
class LoadedTrace:
    path: Path
    rows: tuple


def _require(condition, path, row_number, message):
    if not condition:
        raise TraceFormatError(f"{path}: row {row_number}: {message}")


def _read_csv(path):
    trace_path = Path(path)
    try:
        raw = trace_path.read_bytes()
    except OSError as error:
        raise TraceFormatError(f"cannot read trace {trace_path}: {error}") \
            from error
    if not raw:
        raise TraceFormatError(f"{trace_path}: trace is empty")
    if b"\r" in raw:
        raise TraceFormatError(f"{trace_path}: trace must use LF newlines")
    if not raw.endswith(b"\n"):
        raise TraceFormatError(
            f"{trace_path}: trace must end with an LF newline")
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as error:
        raise TraceFormatError(f"{trace_path}: trace is not UTF-8: {error}") \
            from error
    return trace_path, list(csv.reader(text.splitlines()))


def _validate_decimal(row, field, path, row_number):
    value = row[field]
    _require(bool(re.fullmatch(r"0|[1-9][0-9]*", value)), path,
             row_number, f"{field} must be canonical unsigned decimal")
    _require(int(value) <= UINT64_MAX, path, row_number,
             f"{field} exceeds uint64")


def _validate_normalization(row, path, row_number):
    zero = lambda field: "0x" + "0" * HEX_DIGITS[field]
    if row["im2col_feed_valid"] == "0":
        for field in ("im2col_feed_data", "im2col_feed_mask"):
            _require(row[field] == zero(field), path, row_number,
                     f"{field} must be zero when im2col_feed_valid is 0")
    if row["sa_ins_valid"] == "0":
        for field in (
                "sa_calc_cycles", "sa_valid_rows", "sa_valid_columns",
                "sa_cutbit"):
            _require(row[field] == "0", path, row_number,
                     f"{field} must be zero when sa_ins_valid is 0")
        _require(row["sa_biases"] == zero("sa_biases"), path, row_number,
                 "sa_biases must be zero when sa_ins_valid is 0")
    if row["sa_input_valid"] == "0":
        for field in (
                "sa_activations", "sa_weights", "sa_row_mask",
                "sa_column_mask"):
            _require(row[field] == zero(field), path, row_number,
                     f"{field} must be zero when sa_input_valid is 0")
    else:
        for field in ("sa_row_mask", "sa_column_mask"):
            mask = int(row[field], 16)
            _require(mask != 0 and (mask & (mask + 1)) == 0, path,
                     row_number,
                     f"{field} must be a nonempty canonical prefix mask")

    valid_mask = int(row["pe_valid_mask"], 16)
    add_mask = int(row["pe_add_commit_mask"], 16)
    activations = int(row["pe_activations"], 16)
    weights = int(row["pe_weights"], 16)
    accumulators = int(row["pe_accumulators"], 16)
    for index in range(PE_COUNT):
        if not (valid_mask & (1 << index)):
            _require((activations >> (index * 8)) & 0xff == 0, path,
                     row_number,
                     f"pe_activations PE[{index // 16}][{index % 16}] "
                     "must be zero when invalid")
            _require((weights >> (index * 8)) & 0xff == 0, path,
                     row_number,
                     f"pe_weights PE[{index // 16}][{index % 16}] "
                     "must be zero when invalid")
        if not ((valid_mask | add_mask) & (1 << index)):
            _require((accumulators >> (index * 24)) & 0xffffff == 0,
                     path, row_number,
                     f"pe_accumulators PE[{index // 16}][{index % 16}] "
                     "must be zero when invalid")
    if row["row_score_valid"] == "0":
        _require(row["row_sequence"] == "0", path, row_number,
                 "row_sequence must be zero when row_score_valid is 0")
        _require(row["output_slots"] == zero("output_slots"), path,
                 row_number,
                 "output_slots must be zero when row_score_valid is 0")


def load_trace(path):
    """Load and strictly validate one canonical pipeline cycle trace."""
    trace_path, records = _read_csv(path)
    if tuple(records[0]) != TRACE_FIELDS:
        raise TraceFormatError(
            f"{trace_path}: header differs from the frozen 53-field schema")
    if len(records) == 1:
        raise TraceFormatError(f"{trace_path}: trace has no cycle rows")

    sha_pattern = re.compile(r"[0-9a-f]{64}\Z")
    hex_patterns = {
        field: re.compile(rf"0x[0-9a-f]{{{digits}}}\Z")
        for field, digits in HEX_DIGITS.items()
    }
    rows = []
    expected_hash = None
    active_rows = None
    active_columns = None
    for cycle, record in enumerate(records[1:]):
        row_number = cycle + 2
        _require(len(record) == len(TRACE_FIELDS), trace_path, row_number,
                 f"expected {len(TRACE_FIELDS)} fields, found {len(record)}")
        row = dict(zip(TRACE_FIELDS, record))
        _require(row["schema_version"] == "1", trace_path, row_number,
                 "schema_version must be 1")
        _require(bool(sha_pattern.fullmatch(row["resolved_config_sha256"])),
                 trace_path, row_number,
                 "resolved_config_sha256 must be 64 lowercase hex digits")
        if expected_hash is None:
            expected_hash = row["resolved_config_sha256"]
        _require(row["resolved_config_sha256"] == expected_hash,
                 trace_path, row_number,
                 "resolved_config_sha256 changes within the trace")
        for field in DECIMAL_FIELDS:
            _validate_decimal(row, field, trace_path, row_number)
        _require(row["cycle"] == str(cycle), trace_path, row_number,
                 f"cycle must be consecutive; expected {cycle}")
        _require(row["pipeline_state"] in {str(value) for value in range(7)},
                 trace_path, row_number, "pipeline_state must be in [0, 6]")
        _require(row["im2col_state"] in {str(value) for value in range(6)},
                 trace_path, row_number, "im2col_state must be in [0, 5]")
        _require(row["sa_state"] in {str(value) for value in range(5)},
                 trace_path, row_number, "sa_state must be in [0, 4]")
        _require(int(row["im2col_fifo_count"]) <= 4, trace_path,
                 row_number, "im2col_fifo_count must be in [0, 4]")
        for field in ("im2col_fifo_rptr", "im2col_fifo_wptr"):
            _require(int(row[field]) < 4, trace_path, row_number,
                     f"{field} must be in [0, 3]")
        _require(int(row["sa_valid_rows"]) <= 16, trace_path, row_number,
                 "sa_valid_rows must be in [0, 16]")
        _require(int(row["sa_valid_columns"]) <= 16, trace_path, row_number,
                 "sa_valid_columns must be in [0, 16]")
        _require(int(row["sa_cutbit"]) <= 23, trace_path, row_number,
                 "sa_cutbit must be in [0, 23]")
        _require(int(row["row_sequence"]) < 16, trace_path, row_number,
                 "row_sequence must be in [0, 15]")
        for field in BOOL_FIELDS:
            _require(row[field] in {"0", "1"}, trace_path, row_number,
                     f"{field} must be 0 or 1")
        for field, pattern in hex_patterns.items():
            _require(bool(pattern.fullmatch(row[field])), trace_path,
                     row_number, f"{field} has noncanonical hex width")
        _require(row["im2col_feed_handshake"] == str(
                     int(row["im2col_feed_valid"] == "1" and
                         row["im2col_feed_ready"] == "1")),
                 trace_path, row_number,
                 "im2col_feed_handshake disagrees with valid/ready")
        _require(row["internal_output_valid"] ==
                 row["engine_output_fire"], trace_path, row_number,
                 "engine_output_fire disagrees with internal_output_valid")
        _require(row["output_collected"] == row["row_score_valid"],
                 trace_path, row_number,
                 "output_collected disagrees with row_score_valid")
        _require(row["sau_last_result"] == "0" or
                 row["output_collected"] == "1", trace_path, row_number,
                 "sau_last_result requires output_collected")
        if row["sa_ins_valid"] == "1":
            _require(1 <= int(row["sa_calc_cycles"]) <= 567, trace_path,
                     row_number, "sa_calc_cycles must be in [1, 567]")
            _require(1 <= int(row["sa_valid_rows"]) <= 16, trace_path,
                     row_number, "sa_valid_rows must be in [1, 16]")
            _require(1 <= int(row["sa_valid_columns"]) <= 16, trace_path,
                     row_number, "sa_valid_columns must be in [1, 16]")
            active_rows = int(row["sa_valid_rows"])
            active_columns = int(row["sa_valid_columns"])
        if row["sa_input_valid"] == "1":
            _require(active_rows is not None, trace_path, row_number,
                     "sa_input_valid requires an earlier SA launch")
            expected_row_mask = (1 << active_rows) - 1
            expected_column_mask = (1 << active_columns) - 1
            _require(int(row["sa_row_mask"], 16) == expected_row_mask,
                     trace_path, row_number,
                     "sa_row_mask disagrees with the active SA config")
            _require(int(row["sa_column_mask"], 16) ==
                     expected_column_mask, trace_path, row_number,
                     "sa_column_mask disagrees with the active SA config")
        _validate_normalization(row, trace_path, row_number)
        rows.append(tuple(record))

    for cycle, record in enumerate(rows):
        drained = record[TRACE_FIELDS.index("drained")]
        expected = "1" if cycle + 1 == len(rows) else "0"
        _require(drained == expected, trace_path, cycle + 2,
                 "drained must be 1 only on the final cycle")
    return LoadedTrace(path=trace_path, rows=tuple(rows))


def write_trace(trace, path):
    """Write a previously validated trace in canonical CSV form."""
    if not isinstance(trace, LoadedTrace):
        raise TypeError("trace must be a LoadedTrace")
    output_path = Path(path)
    try:
        with output_path.open("w", encoding="utf-8", newline="") as output:
            writer = csv.writer(output, lineterminator="\n")
            writer.writerow(TRACE_FIELDS)
            writer.writerows(trace.rows)
    except OSError as error:
        raise TraceFormatError(f"cannot write trace {output_path}: {error}") \
            from error


def _pe_difference(field, expected, actual):
    bits = PE_PACKED_FIELDS[field]
    expected_packed = int(expected, 16)
    actual_packed = int(actual, 16)
    mask = (1 << bits) - 1
    for index in range(PE_COUNT):
        expected_value = (expected_packed >> (index * bits)) & mask
        actual_value = (actual_packed >> (index * bits)) & mask
        if expected_value != actual_value:
            if bits == 1:
                expected_text = str(expected_value)
                actual_text = str(actual_value)
            else:
                digits = bits // 4
                expected_text = f"0x{expected_value:0{digits}x}"
                actual_text = f"0x{actual_value:0{digits}x}"
            return index // 16, index % 16, expected_text, actual_text
    raise AssertionError("packed PE values differ without an element delta")


def compare_trace_files(expected_path, actual_path):
    """Compare valid traces and report the first cycle/field/PE delta."""
    expected = load_trace(expected_path)
    actual = load_trace(actual_path)
    shared_rows = min(len(expected.rows), len(actual.rows))
    for cycle in range(shared_rows):
        for index, field in enumerate(TRACE_FIELDS):
            expected_value = expected.rows[cycle][index]
            actual_value = actual.rows[cycle][index]
            if expected_value == actual_value:
                continue
            if (field == "drained" and
                    len(expected.rows) != len(actual.rows) and
                    cycle + 1 == shared_rows):
                continue
            if field in PE_PACKED_FIELDS:
                row, column, expected_pe, actual_pe = _pe_difference(
                    field, expected_value, actual_value)
                raise TraceMismatch(
                    f"cycle {cycle}, field {field}, PE[{row}][{column}]: "
                    f"expected {expected_pe}, actual {actual_pe}")
            raise TraceMismatch(
                f"cycle {cycle}, field {field}: expected {expected_value}, "
                f"actual {actual_value}")
    if len(expected.rows) != len(actual.rows):
        cycle = shared_rows
        if len(expected.rows) > shared_rows:
            raise TraceMismatch(
                f"cycle {cycle}: actual trace ended before expected trace")
        raise TraceMismatch(
            f"cycle {cycle}: actual trace contains an unexpected cycle")
    return len(expected.rows)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("expected", type=Path, help="Expected/RTL CSV trace")
    parser.add_argument("actual", type=Path, help="Actual/gem5 CSV trace")
    args = parser.parse_args(argv)
    try:
        cycles = compare_trace_files(args.expected, args.actual)
    except (TraceFormatError, TraceMismatch) as error:
        print(error, file=sys.stderr)
        return 1
    print(f"pipeline trace comparison passed: {cycles} cycles")
    return 0


if __name__ == "__main__":
    sys.exit(main())
