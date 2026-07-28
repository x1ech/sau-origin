#!/usr/bin/env python3
"""Extract replayable CSR writes and command snapshots from a boundary CSV."""

import argparse
import csv
import json
from pathlib import Path

from build_rtl_golden_package import CsrState


SCOPE_SUFFIX = "SAU_1_inst."


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


def extract(events, zero_ps, period_ps):
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
    args = parser.parse_args()

    writes, snapshots = extract(
        read_events(args.boundary),
        args.trace_cycle_zero_ps,
        args.clock_period_ps,
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
