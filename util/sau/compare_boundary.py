#!/usr/bin/env python3
"""Compare a model boundary trace against an RTL golden boundary trace.

The golden file is the NPI value-change export shipped in the
``functional_ref`` packages: ``signal,time,value`` rows with hierarchical
signal names, simulation times in picoseconds, and full-width binary
values (possibly containing x/z).  The model file uses
``signal,cycle,value`` rows with short signal names and cycles relative
to the model's command anchor; its first row per signal states the
initial value at that cycle.

Golden times are converted to cycles against the first occurrence of the
anchor signal value (default: the ``start`` rise) using the RTL clock
period; every golden change must land on an exact clock edge.  Values
are canonicalized (binary or 0x-hex, leading zeros stripped) so both
sides may use either base.  Consecutive duplicate values collapse on
both sides, matching value-change semantics.

``sequence`` mode compares each signal's ordered value changes;
``cycles`` mode additionally requires the same relative cycle for every
change.  A successful comparison prints nothing.  New modes must be
validated through this comparator against their RTL boundary golden
rather than through a second command-driver state machine.
"""

import argparse
import csv
import sys

DISPLAY_LIMIT = 48


class BoundaryTraceError(Exception):
    """Raised when a boundary trace violates its format contract."""


def normalize_value(text):
    """Canonicalize a trace value for comparison."""
    value = text.strip().lower()
    if value.startswith("0x"):
        try:
            return format(int(value, 16), "b")
        except ValueError as error:
            raise BoundaryTraceError(f"invalid hex value {text!r}") from error
    if value and all(bit in "01" for bit in value):
        return value.lstrip("0") or "0"
    if value and all(bit in "01xz" for bit in value):
        return value
    raise BoundaryTraceError(f"invalid trace value {text!r}")


def display_value(value):
    """Render a canonical value compactly for mismatch reports."""
    if all(bit in "01" for bit in value):
        return f"0x{int(value, 2):x}"
    if len(value) > DISPLAY_LIMIT:
        return value[:DISPLAY_LIMIT] + "..."
    return value


def short_name(signal, strip_prefix):
    """Strip the hierarchical prefix up to and including strip_prefix."""
    if strip_prefix and strip_prefix in signal:
        return signal.split(strip_prefix, 1)[1]
    return signal


def collapse(sequence):
    """Drop consecutive duplicate values, keeping the first occurrence."""
    collapsed = []
    for cycle, value in sequence:
        if collapsed and collapsed[-1][1] == value:
            continue
        collapsed.append((cycle, value))
    return collapsed


def read_rows(path, expected_header):
    with open(path, encoding="utf-8", newline="") as trace:
        reader = csv.reader(trace)
        header = next(reader, None)
        if header != expected_header:
            raise BoundaryTraceError(
                f"{path}: expected header {','.join(expected_header)}"
            )
        for number, row in enumerate(reader, start=2):
            if not row:
                continue
            if len(row) != 3:
                raise BoundaryTraceError(
                    f"{path} line {number}: expected 3 fields"
                )
            yield number, row


def load_golden(path, strip_prefix, clock_ps, anchor_signal, anchor_value):
    """Anchor the golden change list and convert times to cycles."""
    rows = []
    for number, (signal, time_text, value) in read_rows(
            path, ["signal", "time", "value"]):
        try:
            time = int(time_text)
        except ValueError as error:
            raise BoundaryTraceError(
                f"{path} line {number}: invalid time {time_text!r}"
            ) from error
        rows.append((short_name(signal, strip_prefix), time,
                     normalize_value(value)))

    anchor_norm = normalize_value(anchor_value)
    anchor_time = None
    for signal, time, value in rows:
        if signal == anchor_signal and value == anchor_norm:
            anchor_time = time
            break
    if anchor_time is None:
        raise BoundaryTraceError(
            f"{path}: anchor {anchor_signal}={anchor_value} not found"
        )

    sequences = {}
    for signal, time, value in rows:
        entries = sequences.setdefault(signal, [])
        if time <= anchor_time:
            # Fold every change at or before the anchor edge into the
            # cycle-0 initial state.
            entries[:] = [(0, value)]
            continue
        offset = time - anchor_time
        cycle, remainder = divmod(offset, clock_ps)
        if remainder:
            raise BoundaryTraceError(
                f"{path}: {signal} change at {time} ps is {remainder} ps "
                f"off a clock edge (clock {clock_ps} ps, "
                f"anchor {anchor_time} ps)"
            )
        entries.append((cycle, value))
    return {signal: collapse(entries)
            for signal, entries in sequences.items()}


def load_model(path):
    """Load the model trace; per-signal cycles must not decrease."""
    sequences = {}
    for number, (signal, cycle_text, value) in read_rows(
            path, ["signal", "cycle", "value"]):
        try:
            cycle = int(cycle_text)
        except ValueError as error:
            raise BoundaryTraceError(
                f"{path} line {number}: invalid cycle {cycle_text!r}"
            ) from error
        entries = sequences.setdefault(signal, [])
        if entries and cycle < entries[-1][0]:
            raise BoundaryTraceError(
                f"{path} line {number}: {signal} cycle {cycle} decreases"
            )
        entries.append((cycle, normalize_value(value)))
    return {signal: collapse(entries)
            for signal, entries in sequences.items()}


def value_at(sequence, cycle):
    """Piecewise-constant value of a change sequence at a cycle."""
    value = sequence[0][1]
    for entry_cycle, entry_value in sequence:
        if entry_cycle <= cycle:
            value = entry_value
        else:
            break
    return value


def qualify(data_sequence, valid_sequence):
    """Keep only data values accepted while the qualifier reads 1.

    Wire-style golden signals drop to zero outside their valid windows;
    the accepted-payload sequence samples the data at each window start
    and keeps changes inside the window.
    """
    accepted = []
    for index, (start, value) in enumerate(valid_sequence):
        if value != "1":
            continue
        end = valid_sequence[index + 1][0] \
            if index + 1 < len(valid_sequence) else None
        accepted.append((start, value_at(data_sequence, start)))
        for cycle, data_value in data_sequence:
            if cycle > start and (end is None or cycle < end):
                accepted.append((cycle, data_value))
    return collapse(accepted)


def compare_signal(signal, golden, model, mode, allow_actual_extra):
    """Return a mismatch description string or None."""
    for index, (want, have) in enumerate(zip(golden, model)):
        value_match = want[1] == have[1]
        cycle_match = mode != "cycles" or want[0] == have[0]
        if value_match and cycle_match:
            continue
        return (
            f"{signal}: change {index} differs: expected "
            f"{display_value(want[1])} at cycle {want[0]}, actual "
            f"{display_value(have[1])} at cycle {have[0]}"
        )
    if len(golden) != len(model):
        # A truncated golden capture window may end before the model
        # trace does; --allow-actual-extra accepts the matched prefix.
        if allow_actual_extra and len(model) > len(golden):
            return None
        return (
            f"{signal}: change count differs: expected {len(golden)}, "
            f"actual {len(model)}"
        )
    return None


def inspect(sequences):
    for signal in sorted(sequences):
        entries = sequences[signal]
        print(f"{signal}: {len(entries)} changes")
        for cycle, value in entries[:8]:
            print(f"  cycle {cycle}: {display_value(value)}")
        if len(entries) > 8:
            print(f"  ... {len(entries) - 8} more")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("golden",
                        help="RTL boundary trace (signal,time,value)")
    parser.add_argument("actual", nargs="?",
                        help="model boundary trace (signal,cycle,value)")
    parser.add_argument("--mode", choices=["sequence", "cycles"],
                        default="sequence")
    parser.add_argument("--clock-ps", type=int, default=1667,
                        help="RTL clock period in ps")
    parser.add_argument("--anchor-signal", default="start",
                        help="golden signal whose value anchors cycle 0")
    parser.add_argument("--anchor-value", default="1")
    parser.add_argument("--strip-prefix", default="SAU_1_inst.",
                        help="hierarchical prefix stripped from golden names")
    parser.add_argument("--signals", default="",
                        help="comma-separated signals; all must be present")
    parser.add_argument("--qualify", default="",
                        help="comma-separated DATA=VALID pairs: compare only "
                             "golden DATA values accepted while VALID reads 1")
    parser.add_argument("--allow-actual-extra", action="store_true",
                        help="accept a matched golden prefix when the model "
                             "trace extends past a truncated golden window")
    parser.add_argument("--inspect", action="store_true",
                        help="dump the anchored golden sequences and exit")
    args = parser.parse_args(argv)

    try:
        golden = load_golden(args.golden, args.strip_prefix, args.clock_ps,
                             args.anchor_signal, args.anchor_value)
        for pair in filter(None, args.qualify.split(",")):
            if "=" not in pair:
                raise BoundaryTraceError(
                    f"--qualify entry {pair!r} is not DATA=VALID")
            data, valid = pair.split("=", 1)
            if data not in golden or valid not in golden:
                raise BoundaryTraceError(
                    f"--qualify signals missing from the golden trace: "
                    f"{pair}")
            golden[data] = qualify(golden[data], golden[valid])
        if args.inspect:
            inspect(golden)
            return 0
        if not args.actual:
            raise BoundaryTraceError(
                "an actual model trace is required unless --inspect is used")
        model = load_model(args.actual)
    except (OSError, BoundaryTraceError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    if args.signals:
        signals = [name for name in args.signals.split(",") if name]
        missing = [name for name in signals
                   if name not in golden or name not in model]
        if missing:
            print(f"error: signals missing from a trace: "
                  f"{', '.join(missing)}", file=sys.stderr)
            return 1
    else:
        signals = sorted(set(golden) & set(model))
        if not signals:
            print("error: the traces share no signals", file=sys.stderr)
            return 1

    mismatches = []
    for signal in signals:
        mismatch = compare_signal(
            signal, golden[signal], model[signal], args.mode,
            args.allow_actual_extra)
        if mismatch:
            mismatches.append(mismatch)
    for mismatch in mismatches:
        print(mismatch)
    return 1 if mismatches else 0


if __name__ == "__main__":
    sys.exit(main())
