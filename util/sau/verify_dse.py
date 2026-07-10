#!/usr/bin/env python3
"""Validate SAU design-space exploration monotonicity from gem5 stats."""

import argparse
import re
import sys


REQUIRED_STATS = {
    "commandCycles",
    "stallArrayCapacity",
    "stallOutputBufferFull",
}
STAT_LINE = re.compile(r"^(system\.sau\.\S+)\s+(\S+)")


class StatsFormatError(ValueError):
    """Raised when a stats file lacks a required SAU statistic."""


def read_sau_stats(path):
    """Return scalar SAU statistics from a gem5 ``stats.txt`` file."""

    stats = {}
    with open(path, encoding="utf-8") as stats_file:
        for line in stats_file:
            match = STAT_LINE.match(line)
            if not match:
                continue
            name = match.group(1).removeprefix("system.sau.")
            try:
                stats[name] = float(match.group(2))
            except ValueError as exc:
                raise StatsFormatError(
                    f"{path}: statistic {name} is not numeric"
                ) from exc

    missing = REQUIRED_STATS - set(stats)
    if missing:
        raise StatsFormatError(
            f"{path}: missing required SAU statistics: "
            f"{', '.join(sorted(missing))}"
        )
    return stats


def validate_monotonicity(capacity_one, capacity_sixteen,
                          output_one, output_eight):
    """Return violations for the two Task-11 resource comparisons."""

    errors = []
    if capacity_sixteen["commandCycles"] > capacity_one["commandCycles"]:
        errors.append(
            "array_capacity=16 increased commandCycles relative to 1"
        )
    if capacity_one["stallArrayCapacity"] <= 0:
        errors.append(
            "array_capacity=1 did not record stallArrayCapacity"
        )
    if output_eight["commandCycles"] > output_one["commandCycles"]:
        errors.append(
            "output_buffer_entries=8 increased commandCycles relative to 1"
        )
    if output_one["stallOutputBufferFull"] <= 0:
        errors.append(
            "output_buffer_entries=1 did not record stallOutputBufferFull"
        )
    return errors


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Validate SAU array/output-buffer DSE monotonicity"
    )
    parser.add_argument("--array-capacity-1", required=True)
    parser.add_argument("--array-capacity-16", required=True)
    parser.add_argument("--output-buffer-1", required=True)
    parser.add_argument("--output-buffer-8", required=True)
    args = parser.parse_args(argv)

    try:
        errors = validate_monotonicity(
            read_sau_stats(args.array_capacity_1),
            read_sau_stats(args.array_capacity_16),
            read_sau_stats(args.output_buffer_1),
            read_sau_stats(args.output_buffer_8),
        )
    except (OSError, StatsFormatError) as exc:
        print(exc)
        return 1

    if errors:
        for error in errors:
            print(error)
        return 1

    print("SAU DSE monotonicity passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
