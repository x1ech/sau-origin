#!/usr/bin/env python3
"""Compare SAU architecture timing traces.

The public SAU trace schema is:

    cycle,event,command_id,stream,address,beat,phase

`read_trace()` validates the fixed schema, parses integer fields, and
normalizes all cycles so the first `command_accepted` row is cycle 0.
`compare_rows()` supports:

* strict: every normalized field must match exactly;
* causal: command-local dataflow lanes, phase-transition sequence, and
  dependencies must match, while independent events may interleave and data
  events may observe a different phase snapshot under backpressure.
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
CAUSAL_SEQUENCE_FIELDS = ("address", "beat")
CAUSAL_PHASE_ANCHOR_EVENTS = {
    "phase_changed",
    "command_accepted",
    "command_complete",
}


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

    if not command_cycles:
        raise TraceFormatError(
            f"{path}: expected at least one command_accepted row, got 0"
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
    _check_nondecreasing_cycles(actual_rows, errors)

    expected_lanes = _group_causal_lanes(expected_rows)
    actual_lanes = _group_causal_lanes(actual_rows)
    expected_keys = set(expected_lanes)
    actual_keys = set(actual_lanes)
    for key in sorted(expected_keys - actual_keys):
        errors.append(f"causal lane missing from actual trace: {key}")
    for key in sorted(actual_keys - expected_keys):
        errors.append(f"unexpected causal lane in actual trace: {key}")

    for key in sorted(expected_keys & actual_keys):
        expected_lane = expected_lanes[key]
        actual_lane = actual_lanes[key]
        if len(expected_lane) != len(actual_lane):
            errors.append(
                f"causal lane {key} count mismatch: expected "
                f"{len(expected_lane)}, actual {len(actual_lane)}"
            )
        for index, (expected, actual) in enumerate(
                zip(expected_lane, actual_lane)):
            fields = CAUSAL_SEQUENCE_FIELDS
            if key[1] in CAUSAL_PHASE_ANCHOR_EVENTS:
                fields += ("phase",)
            for field in fields:
                if expected[field] != actual[field]:
                    errors.append(
                        f"causal lane {key} item {index}: {field} mismatch: "
                        f"expected {expected[field]!r}, actual "
                        f"{actual[field]!r}"
                    )

    _check_command_boundaries(actual_rows, errors)
    _check_read_dependencies(actual_rows, errors)
    _check_b_input_dependencies(actual_rows, errors)
    _check_result_write_dependency(actual_rows, errors)


def _check_nondecreasing_cycles(rows, errors):
    previous_actual_cycle = None
    for index, actual in enumerate(rows):
        if previous_actual_cycle is not None:
            if actual["cycle"] < previous_actual_cycle:
                errors.append(
                    f"row {index}: actual cycle {actual['cycle']} violates "
                    f"nondecreasing order after {previous_actual_cycle}"
                )
        previous_actual_cycle = actual["cycle"]


def _group_causal_lanes(rows):
    lanes = {}
    for row in rows:
        key = (row["command_id"], row["event"], row["stream"])
        lanes.setdefault(key, []).append(row)
    return lanes


def _positions(rows, event, stream, command_id):
    return [
        index for index, row in enumerate(rows)
        if row["command_id"] == command_id and row["event"] == event and
        row["stream"] == stream
    ]


def _check_command_boundaries(rows, errors):
    command_ids = sorted({row["command_id"] for row in rows})
    for command_id in command_ids:
        command_positions = [
            index for index, row in enumerate(rows)
            if row["command_id"] == command_id
        ]
        accepted = _positions(rows, "command_accepted", "none", command_id)
        complete = _positions(rows, "command_complete", "none", command_id)
        if len(accepted) != 1:
            errors.append(
                f"command {command_id}: expected one command_accepted, "
                f"got {len(accepted)}"
            )
        if len(complete) != 1:
            errors.append(
                f"command {command_id}: expected one command_complete, "
                f"got {len(complete)}"
            )
        if accepted and complete:
            data_positions = [
                index for index in command_positions
                if rows[index]["event"] not in
                {"phase_changed", "command_accepted", "command_complete"}
            ]
            if data_positions and accepted[0] > min(data_positions):
                errors.append(
                    f"command {command_id}: data event precedes acceptance"
                )
            if complete[0] != max(command_positions):
                errors.append(
                    f"command {command_id}: completion is not the final event"
                )


def _check_read_dependencies(rows, errors):
    command_ids = {row["command_id"] for row in rows}
    for command_id in command_ids:
        for stream in ("operand_a", "operand_b"):
            accepted = _positions(rows, "read_accepted", stream, command_id)
            responses = _positions(
                rows, "read_response_visible", stream, command_id
            )
            for index, (request, response) in enumerate(zip(accepted, responses)):
                if response <= request:
                    errors.append(
                        f"command {command_id} {stream} beat {index}: "
                        "response precedes request acceptance"
                    )


def _check_b_input_dependencies(rows, errors):
    command_ids = {row["command_id"] for row in rows}
    for command_id in command_ids:
        responses = _positions(
            rows, "read_response_visible", "operand_b", command_id
        )
        inputs = _positions(
            rows, "array_input_accepted", "operand_b", command_id
        )
        for index, (response, array_input) in enumerate(zip(responses, inputs)):
            if array_input <= response:
                errors.append(
                    f"command {command_id} operand_b beat {index}: "
                    "array input precedes response visibility"
                )


def _check_result_write_dependency(rows, errors):
    command_ids = {row["command_id"] for row in rows}
    for command_id in command_ids:
        results = _positions(rows, "result_produced", "output", command_id)
        writes = _positions(rows, "write_accepted", "output", command_id)
        if results and writes and min(writes) <= max(results):
            errors.append(
                f"command {command_id}: writeback begins before result "
                "production completes"
            )


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Compare two SAU architecture timing traces"
    )
    parser.add_argument(
        "--mode",
        choices=sorted(VALID_MODES),
        default="strict",
        help="comparison mode: strict compares normalized cycles; causal "
             "checks dataflow dependencies while allowing memory interleaving",
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
