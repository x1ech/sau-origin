#!/usr/bin/env python3
"""Extract replayable CSR writes and command snapshots from a boundary CSV."""

import argparse
import csv
import json
from pathlib import Path

try:
    from .build_rtl_golden_package import CsrState
except ImportError:
    from build_rtl_golden_package import CsrState


SCOPE_SUFFIX = "SAU_1_inst."
FIXED_CSR_WRITE_ADDRESSES = tuple(range(0x200, 0x20E, 2))


def signal_name(path):
    marker = path.rfind(SCOPE_SUFFIX)
    if marker < 0:
        return path
    return path[marker + len(SCOPE_SUFFIX):]


def binary_value(text):
    value = text.strip().lower()
    if not value or any(char in value for char in "xz"):
        raise ValueError("invalid RTL binary value: {!r}".format(text))
    return int(value, 2)


def cycle_at(time_ps, zero_ps, period_ps):
    delta = time_ps - zero_ps
    if delta < 0 or delta % period_ps:
        raise ValueError(
            "FSDB time {} is not on the {} ps grid from {} ps".format(
                time_ps, period_ps, zero_ps
            )
        )
    return delta // period_ps


def read_events(path):
    events = {}
    with path.open(newline="") as source:
        reader = csv.DictReader(source)
        if reader.fieldnames != ["signal", "time", "value"]:
            raise ValueError(
                "{}: expected signal,time,value header".format(path)
            )
        for row_number, row in enumerate(reader, start=2):
            time_ps = int(row["time"])
            events.setdefault(time_ps, []).append(
                (signal_name(row["signal"]), row["value"], row_number)
            )
    return events


def extract(events, zero_ps, period_ps, infer_fixed_write_order=False):
    if infer_fixed_write_order:
        return infer_writes_from_data_pulses(events, zero_ps, period_ps)

    values = {}
    writes = []
    snapshots = []
    state = CsrState()
    command_id = 0

    for time_ps in sorted(events):
        changed = set()
        for name, value, _row_number in events[time_ps]:
            values[name] = value
            changed.add(name)

        if "csr_we" in changed and binary_value(values["csr_we"]) == 1:
            address = binary_value(values["csr_addr"])
            data = binary_value(values["csr_wdata"])
            if address >> 4 != 0x20:
                continue
            cycle = cycle_at(time_ps, zero_ps, period_ps)
            writes.append({
                "cycle": cycle,
                "csr_addr": "0x{:03x}".format(address),
                "csr_operation": 1,
                "csr_wdata": "0x{:016x}".format(data),
                "accepted": 1,
            })
            state.apply(address, data)

        if "start" in changed and binary_value(values["start"]) == 1:
            command_id += 1
            cycle = cycle_at(time_ps, zero_ps, period_ps)
            snapshots.append(
                state.snapshot(command_id, cycle, len(writes))
            )

    if not writes or not snapshots:
        raise ValueError("boundary trace contains no SAU commands")
    if len(writes) != 7 * len(snapshots):
        raise ValueError(
            "expected seven CSR writes per command, found {} writes for {} "
            "commands".format(len(writes), len(snapshots))
        )
    return writes, snapshots


def infer_writes_from_data_pulses(events, zero_ps, period_ps):
    """Recover a stripped seven-write trace without claiming sampled addr/we."""
    data_pulses = []
    start_cycles = []
    observed_modes = {}

    for time_ps in sorted(events):
        for name, value, _row_number in events[time_ps]:
            if name == "csr_wdata":
                data = binary_value(value)
                if data:
                    data_pulses.append((cycle_at(
                        time_ps, zero_ps, period_ps), data))
            elif name == "start" and binary_value(value) == 1:
                start_cycles.append(cycle_at(
                    time_ps, zero_ps, period_ps))
            elif name in {"trans_mode", "reuse_mode", "cutbit"}:
                observed_modes[name] = binary_value(value)

    if len(start_cycles) != 1:
        raise ValueError(
            "fixed-order inference requires exactly one observed start")
    if len(data_pulses) != len(FIXED_CSR_WRITE_ADDRESSES):
        raise ValueError(
            "fixed-order inference requires exactly seven nonzero "
            "csr_wdata pulses")
    if start_cycles[0] != data_pulses[-1][0] + 1:
        raise ValueError(
            "fixed-order inference requires start one cycle after the "
            "final csr_wdata pulse")

    writes = []
    state = CsrState()
    for (pulse_cycle, data), address in zip(
            data_pulses, FIXED_CSR_WRITE_ADDRESSES):
        # Value-change traces expose the CSR bus setup at the preceding
        # interval boundary. The accepted posedge is the following cycle,
        # as cross-checked against the complete ATBD csr_we trace.
        accepted_cycle = pulse_cycle + 1
        writes.append({
            "cycle": accepted_cycle,
            "csr_addr": "0x{:03x}".format(address),
            "csr_operation": 1,
            "csr_wdata": "0x{:016x}".format(data),
            "accepted": 1,
        })
        state.apply(address, data)

    snapshot = state.snapshot(1, start_cycles[0], len(writes))
    for name in ("trans_mode", "reuse_mode", "cutbit"):
        if name not in observed_modes:
            raise ValueError(
                "fixed-order inference requires observed {}".format(name))
        expected = int(snapshot[name], 0) if isinstance(
            snapshot[name], str) else snapshot[name]
        if observed_modes[name] != expected:
            raise ValueError(
                "inferred {}={} differs from observed {}".format(
                    name, expected, observed_modes[name]))
    return writes, [snapshot]


def write_csv(path, rows):
    with path.open("w", newline="") as output:
        writer = csv.DictWriter(
            output,
            fieldnames=(
                "cycle", "csr_addr", "csr_operation", "csr_wdata", "accepted"
            ),
            lineterminator="\n",
        )
        writer.writeheader()
        writer.writerows(rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--boundary", type=Path, required=True)
    parser.add_argument("--csr-writes", type=Path, required=True)
    parser.add_argument("--csr-snapshot", type=Path, required=True)
    parser.add_argument("--trace-cycle-zero-ps", type=int, default=1250)
    parser.add_argument("--clock-period-ps", type=int, default=1667)
    parser.add_argument(
        "--infer-fixed-write-order", action="store_true",
        help="infer 0x200..0x20c from seven csr_wdata pulses in a stripped "
             "single-command Step-0 trace")
    args = parser.parse_args()

    writes, snapshots = extract(
        read_events(args.boundary),
        args.trace_cycle_zero_ps,
        args.clock_period_ps,
        args.infer_fixed_write_order,
    )
    write_csv(args.csr_writes, writes)
    args.csr_snapshot.write_text(
        json.dumps(snapshots, indent=2, sort_keys=True) + "\n"
    )
    print(
        "extracted {} writes and {} commands; flows={}".format(
            len(writes),
            len(snapshots),
            [snapshot["sa_flow_mode"] for snapshot in snapshots],
        )
    )


if __name__ == "__main__":
    main()
