#!/usr/bin/env python3
"""Strict per-cycle comparator for Im2Col RTL and gem5 CSV traces."""

import argparse
import csv
from dataclasses import dataclass
from pathlib import Path
import re
import sys

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from util.im2col.im2col_contract import (
    FIFO_DEPTH,
    REQ_ADDR_HEX_DIGITS,
    REQ_VALID_HEX_DIGITS,
    RESP_DATA_HEX_DIGITS,
    RESP_VALID_HEX_DIGITS,
    FEED_DATA_HEX_DIGITS,
    FEED_MASK_HEX_DIGITS,
    SP_BANKS,
    TRACE_FIELDS,
)


class TraceFormatError(ValueError):
    """Raised when one trace violates the frozen CSV schema."""


class TraceMismatch(AssertionError):
    """Raised at the first per-cycle difference between two valid traces."""


@dataclass(frozen=True)
class LoadedTrace:
    path: Path
    rows: tuple


def _hex_pattern(digits):
    return re.compile(rf"0x[0-9a-f]{{{digits}}}\Z")


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


def load_trace(path):
    """Load and validate one canonical Im2Col cycle trace."""
    trace_path, records = _read_csv(path)
    header = tuple(records[0])
    if header != TRACE_FIELDS:
        raise TraceFormatError(
            f"{trace_path}: header differs from the frozen 47-field schema")
    if len(records) == 1:
        raise TraceFormatError(f"{trace_path}: trace has no cycle rows")

    patterns = {
        "req_valid": _hex_pattern(REQ_VALID_HEX_DIGITS),
        "resp_valid": _hex_pattern(RESP_VALID_HEX_DIGITS),
        "feed_data": _hex_pattern(FEED_DATA_HEX_DIGITS),
        "feed_mask": _hex_pattern(FEED_MASK_HEX_DIGITS),
    }
    request_address_pattern = _hex_pattern(REQ_ADDR_HEX_DIGITS)
    response_data_pattern = _hex_pattern(RESP_DATA_HEX_DIGITS)
    sha_pattern = re.compile(r"[0-9a-f]{64}\Z")
    rows = []
    expected_hash = None

    for cycle, record in enumerate(records[1:]):
        row_number = cycle + 2
        _require(
            len(record) == len(TRACE_FIELDS), trace_path, row_number,
            f"expected {len(TRACE_FIELDS)} fields, found {len(record)}",
        )
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
        _require(row["cycle"] == str(cycle), trace_path, row_number,
                 f"cycle must be consecutive; expected {cycle}")
        _require(row["state"] in {str(value) for value in range(6)},
                 trace_path, row_number, "state must be in [0, 5]")
        for field in ("busy", "done", "feed_valid", "feed_ready"):
            _require(row[field] in {"0", "1"}, trace_path, row_number,
                     f"{field} must be 0 or 1")
        _require(row["fifo_count"] in {str(value)
                                      for value in range(FIFO_DEPTH + 1)},
                 trace_path, row_number,
                 f"fifo_count must be in [0, {FIFO_DEPTH}]")
        for field in ("fifo_rptr", "fifo_wptr"):
            _require(row[field] in {str(value)
                                    for value in range(FIFO_DEPTH)},
                     trace_path, row_number,
                     f"{field} must be in [0, {FIFO_DEPTH - 1}]")
        for field, pattern in patterns.items():
            _require(bool(pattern.fullmatch(row[field])), trace_path,
                     row_number, f"{field} has noncanonical hex width")

        request_valid = int(row["req_valid"], 16)
        response_valid = int(row["resp_valid"], 16)
        _require(request_valid < (1 << SP_BANKS), trace_path, row_number,
                 "req_valid sets bits outside the fixed bank count")
        _require(response_valid < (1 << SP_BANKS), trace_path, row_number,
                 "resp_valid sets bits outside the fixed bank count")
        for bank in range(SP_BANKS):
            address_field = f"req_addr_b{bank:02d}"
            data_field = f"resp_data_b{bank:02d}"
            _require(bool(request_address_pattern.fullmatch(
                         row[address_field])), trace_path, row_number,
                     f"{address_field} has noncanonical hex width")
            _require(bool(response_data_pattern.fullmatch(row[data_field])),
                     trace_path, row_number,
                     f"{data_field} has noncanonical hex width")
            if not (request_valid & (1 << bank)):
                _require(row[address_field] == "0x000", trace_path,
                         row_number,
                         f"{address_field} must be zero when invalid")
            if not (response_valid & (1 << bank)):
                _require(row[data_field] == "0x00", trace_path,
                         row_number,
                         f"{data_field} must be zero when invalid")
        if row["feed_valid"] == "0":
            _require(row["feed_data"] == "0x" + "0" * FEED_DATA_HEX_DIGITS,
                     trace_path, row_number,
                     "feed_data must be zero when feed_valid is 0")
            _require(row["feed_mask"] == "0x" + "0" * FEED_MASK_HEX_DIGITS,
                     trace_path, row_number,
                     "feed_mask must be zero when feed_valid is 0")
        rows.append(tuple(record))

    return LoadedTrace(path=trace_path, rows=tuple(rows))


def compare_trace_files(expected_path, actual_path):
    """Compare two valid traces and fail at their first cycle/field delta."""
    expected = load_trace(expected_path)
    actual = load_trace(actual_path)
    shared_rows = min(len(expected.rows), len(actual.rows))
    for cycle in range(shared_rows):
        for field_index, field in enumerate(TRACE_FIELDS):
            expected_value = expected.rows[cycle][field_index]
            actual_value = actual.rows[cycle][field_index]
            if expected_value != actual_value:
                raise TraceMismatch(
                    f"cycle {cycle}, field {field}: expected "
                    f"{expected_value}, actual {actual_value}")
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
    print(f"trace comparison passed: {cycles} cycles")
    return 0


if __name__ == "__main__":
    sys.exit(main())
