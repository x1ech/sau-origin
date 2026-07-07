#!/usr/bin/env python3
"""Compare SAU architecture timing traces.

The public SAU trace schema is:

    cycle,event,command_id,stream,address,beat,phase

`read_trace()` validates the fixed schema, parses integer fields, and
normalizes all cycles so the single `command_accepted` row is cycle 0.
`compare_rows()` supports:

* strict: every normalized field must match exactly;
* causal: event/stream/address/beat/phase sequence must match, while absolute
  cycle differences are ignored as long as the actual trace remains
  nondecreasing.
"""

import argparse
import csv
import sys


HEADER = "cycle,event,command_id,stream,address,beat,phase"
FIELDS = HEADER.split(",")

VALID_EVENTS = {
    "phase_changed",
    "command_accepted",
    "read_accepted",
    "read_response_visible",
    "array_input_accepted",
    "result_produced",
    "write_accepted",
    "command_complete",
}
VALID_STREAMS = {"none", "operand_a", "operand_b", "output"}
VALID_PHASES = {
    "idle",
    "operand_load",
    "array_active",
    "array_drain",
    "writeback",
    "complete",
}
VALID_MODES = {"strict", "causal"}
CAUSAL_FIELDS = ("event", "stream", "address", "beat", "phase")


class TraceFormatError(ValueError):
    """Raised when a trace file violates the public SAU CSV contract."""


def _parse_int(value, field, row_number):
    try:
        return int(value)
    except ValueError as exc:
        raise TraceFormatError(
            f"row {row_number}: field '{field}' is not an integer: {value!r}"
        ) from exc


def _coerce_row(row):
    coerced = dict(row)
    coerced["cycle"] = int(coerced["cycle"])
    coerced["command_id"] = int(coerced["command_id"])
    coerced["beat"] = int(coerced["beat"])
    return coerced


def read_trace(path):
    """Read, validate, and cycle-normalize a SAU architecture trace."""

    rows = []
    command_cycles = []
    with open(path, newline="") as trace:
        reader = csv.reader(trace)
        try:
            header = next(reader)
        except StopIteration as exc:
            raise TraceFormatError(f"{path}: trace is empty") from exc

        actual_header = ",".join(header)
        if actual_header != HEADER:
            raise TraceFormatError(
                f"{path}: header mismatch: got {actual_header!r}, "
                f"expected {HEADER!r}"
            )

        for row_number, raw in enumerate(reader, start=2):
            if len(raw) != len(FIELDS):
                raise TraceFormatError(
                    f"{path}: row {row_number}: expected {len(FIELDS)} "
                    f"columns, got {len(raw)}"
                )
            row = dict(zip(FIELDS, raw))
            row["cycle"] = _parse_int(row["cycle"], "cycle", row_number)
            row["command_id"] = _parse_int(
                row["command_id"], "command_id", row_number
            )
            row["beat"] = _parse_int(row["beat"], "beat", row_number)

            if row["event"] not in VALID_EVENTS:
                raise TraceFormatError(
                    f"{path}: row {row_number}: unknown event "
                    f"{row['event']!r}"
                )
            if row["stream"] not in VALID_STREAMS:
                raise TraceFormatError(
                    f"{path}: row {row_number}: unknown stream "
                    f"{row['stream']!r}"
                )
            if row["phase"] not in VALID_PHASES:
                raise TraceFormatError(
                    f"{path}: row {row_number}: unknown phase "
                    f"{row['phase']!r}"
                )
            if row["event"] == "command_accepted":
                command_cycles.append(row["cycle"])
            rows.append(row)

    if len(command_cycles) != 1:
        raise TraceFormatError(
            f"{path}: expected exactly one command_accepted row, "
            f"got {len(command_cycles)}"
        )

    command_cycle = command_cycles[0]
    for row in rows:
        row["cycle"] -= command_cycle
    return rows


def compare_rows(expected, actual, mode="strict"):
    """Return a list of human-readable mismatches between two row sequences."""

    if mode not in VALID_MODES:
        raise ValueError(f"unknown compare mode: {mode!r}")

    expected_rows = [_coerce_row(row) for row in expected]
    actual_rows = [_coerce_row(row) for row in actual]

    errors = []
    if len(expected_rows) != len(actual_rows):
        errors.append(
            f"row count mismatch: expected {len(expected_rows)}, "
            f"actual {len(actual_rows)}"
        )

    if mode == "strict":
        _compare_strict(expected_rows, actual_rows, errors)
    else:
        _compare_causal(expected_rows, actual_rows, errors)
    return errors


def _compare_strict(expected_rows, actual_rows, errors):
    for index, (expected, actual) in enumerate(zip(expected_rows, actual_rows)):
        for field in FIELDS:
            if expected[field] != actual[field]:
                errors.append(
                    f"row {index}: {field} mismatch: expected "
                    f"{expected[field]!r}, actual {actual[field]!r}"
                )


def _compare_causal(expected_rows, actual_rows, errors):
    previous_actual_cycle = None
    for index, (expected, actual) in enumerate(zip(expected_rows, actual_rows)):
        for field in CAUSAL_FIELDS:
            if expected[field] != actual[field]:
                errors.append(
                    f"row {index}: {field} mismatch: expected "
                    f"{expected[field]!r}, actual {actual[field]!r}"
                )
        if previous_actual_cycle is not None:
            if actual["cycle"] < previous_actual_cycle:
                errors.append(
                    f"row {index}: actual cycle {actual['cycle']} violates "
                    f"nondecreasing order after {previous_actual_cycle}"
                )
        previous_actual_cycle = actual["cycle"]


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Compare two SAU architecture timing traces"
    )
    parser.add_argument(
        "--mode",
        choices=sorted(VALID_MODES),
        default="strict",
        help="comparison mode: strict compares normalized cycles; causal "
             "ignores absolute cycle differences",
    )
    parser.add_argument("expected", help="expected/reference CSV trace")
    parser.add_argument("actual", help="actual CSV trace")
    args = parser.parse_args(argv)

    try:
        expected_rows = read_trace(args.expected)
        actual_rows = read_trace(args.actual)
        errors = compare_rows(expected_rows, actual_rows, args.mode)
    except (OSError, TraceFormatError, ValueError) as exc:
        print(exc)
        return 1

    if errors:
        for error in errors:
            print(error)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
