#!/usr/bin/env python3
"""Normalize RTL SAU diagnostics and compare semantic state traces."""

import argparse
import csv
import sys


HEADER = (
    "cycle,command_id,schedule_state,rtl_state,input_switch,"
    "transition_cause"
)
FIELDS = HEADER.split(",")

RTL_TO_SEMANTIC = {
    "REGISTER_LOAD": ("resident_load", "REGISTER_LOAD", "register_load"),
    "TRANSPOSE_LOAD": (
        "transpose_setup", "TRANSPOSE_LOAD", "register_load_done"
    ),
    "REUSE_LOAD": ("flow_execute", "REUSE_LOAD", "transpose_complete"),
    "TRANSPOSE_CLIP": (
        "flow_boundary", "TRANSPOSE_CLIP", "flow_boundary"
    ),
    "FIRST_LOAD": (
        "drain_and_writeback", "FIRST_LOAD|D_OUT|REGISTER_UNLOAD",
        "final_flow",
    ),
    "D_OUT": (
        "drain_and_writeback", "FIRST_LOAD|D_OUT|REGISTER_UNLOAD",
        "drain_results",
    ),
    "REGISTER_UNLOAD": (
        "drain_and_writeback", "FIRST_LOAD|D_OUT|REGISTER_UNLOAD",
        "writeback_drained",
    ),
}


class StateTraceFormatError(ValueError):
    """Raised when a state trace or RTL diagnostic violates its schema."""


def _integer(value, field, path, row_number):
    try:
        return int(value)
    except ValueError as exc:
        raise StateTraceFormatError(
            f"{path}: row {row_number}: {field} is not an integer: {value!r}"
        ) from exc


def _normalize_cycles(rows, path):
    if not rows:
        raise StateTraceFormatError(f"{path}: no active SAU state rows")
    origin = rows[0]["cycle"]
    for row in rows:
        row["cycle"] -= origin
    return rows


def read_state_trace(path):
    """Read a gem5 semantic-state trace with its independent public schema."""
    with open(path, newline="") as trace:
        reader = csv.DictReader(trace)
        if reader.fieldnames != FIELDS:
            raise StateTraceFormatError(
                f"{path}: header mismatch: expected {HEADER!r}"
            )
        rows = []
        for row_number, row in enumerate(reader, start=2):
            row["cycle"] = _integer(row["cycle"], "cycle", path, row_number)
            row["command_id"] = _integer(
                row["command_id"], "command_id", path, row_number
            )
            if row["input_switch"] not in {"00", "01", "10", "11"}:
                raise StateTraceFormatError(
                    f"{path}: row {row_number}: invalid input_switch "
                    f"{row['input_switch']!r}"
                )
            rows.append(row)
    return _normalize_cycles(rows, path)


def normalize_rtl_diagnostic(path):
    """Project diagnostic.csv onto the supported semantic ATB/reuse-A states."""
    required = {
        "cycle", "command_id", "start", "core_state", "input_switch_f",
        "command_done",
    }
    rows = []
    previous = None
    previous_row = None
    with open(path, newline="") as diagnostic:
        reader = csv.DictReader(diagnostic)
        if not reader.fieldnames or not required.issubset(reader.fieldnames):
            raise StateTraceFormatError(
                f"{path}: diagnostic.csv lacks required state fields"
            )
        for row_number, raw in enumerate(reader, start=2):
            command_id = _integer(
                raw["command_id"], "command_id", path, row_number
            )
            if command_id == 0:
                continue
            cycle = _integer(raw["cycle"], "cycle", path, row_number)
            state = raw["core_state"]
            if state == "IDLE" and raw["start"] != "0":
                semantic, rtl_state, cause = (
                    "resident_load", "REGISTER_LOAD", "command_accepted"
                )
            elif state == "IDLE" and raw["command_done"] != "0":
                semantic, rtl_state, cause = (
                    "complete", "IDLE", "command_complete"
                )
            elif state in RTL_TO_SEMANTIC:
                semantic, rtl_state, cause = RTL_TO_SEMANTIC[state]
            else:
                continue
            input_switch = raw["input_switch_f"].replace("2'b", "")
            row = {
                "cycle": cycle,
                "command_id": command_id,
                "schedule_state": semantic,
                "rtl_state": rtl_state,
                "input_switch": input_switch,
                "transition_cause": cause,
            }
            key = (
                row["command_id"], row["schedule_state"],
                row["input_switch"],
            )
            if key != previous:
                if (previous_row and
                        row["command_id"] == previous_row["command_id"] and
                        row["schedule_state"] == previous_row["schedule_state"] and
                        row["input_switch"] != previous_row["input_switch"]):
                    row["transition_cause"] = "input_switch_visible"
                rows.append(row)
                previous = key
                previous_row = row
    return _normalize_cycles(rows, path)


def compare_state_rows(expected, actual):
    """Compare normalized state transitions and report localized mismatches."""
    errors = []
    for index, (left, right) in enumerate(zip(expected, actual)):
        for field in FIELDS:
            if left[field] != right[field]:
                errors.append(
                    f"state row {index} (expected command "
                    f"{left['command_id']} cycle {left['cycle']}, actual "
                    f"command {right['command_id']} cycle {right['cycle']}): "
                    f"{field} mismatch: expected {left[field]!r}, actual "
                    f"{right[field]!r}"
                )
    if len(expected) != len(actual):
        errors.append(
            f"state row count mismatch: expected {len(expected)}, "
            f"actual {len(actual)}"
        )
    return errors


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Compare an RTL diagnostic or semantic state trace"
    )
    parser.add_argument(
        "--rtl-diagnostic", action="store_true",
        help="normalize the expected input from RTL diagnostic.csv",
    )
    parser.add_argument("expected")
    parser.add_argument("actual")
    parser.add_argument(
        "--max-errors", type=int, default=1,
        help="number of earliest mismatches to print (default: 1)",
    )
    args = parser.parse_args(argv)
    try:
        expected = (
            normalize_rtl_diagnostic(args.expected)
            if args.rtl_diagnostic else read_state_trace(args.expected)
        )
        actual = read_state_trace(args.actual)
        errors = compare_state_rows(expected, actual)
    except (OSError, StateTraceFormatError, ValueError) as error:
        print(error)
        return 1
    for error in errors[:args.max_errors]:
        print(error)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
