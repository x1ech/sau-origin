#!/usr/bin/env python3
"""Independent functional and commit-count checks for sau_array_16x16."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path


SIZE = 16
ACC_MIN = -(1 << 23)
ACC_MAX = (1 << 23) - 1

CASES = {
    "tail_r1_c1_k9": (1, 1, 9, 0, False),
    "tail_r15_c15_k9": (15, 15, 9, 5, False),
    "full_r16_c16_k9": (16, 16, 9, 5, False),
    "backpressure_r3_c3_k9": (3, 3, 9, 2, True),
    "sat_pos_r1_c1_k567": (1, 1, 567, 16, False),
    "sat_neg_r1_c1_k567": (1, 1, 567, 16, False),
}

HEADER = [
    "cycle", "case_name", "state", "start", "busy", "input_valid",
    "input_ready", "input_fire", "output_valid", "output_ready",
    "activation_bus", "weight_bus", "output_fire", "output_row", "done",
    "output_bus", "mac_commit_mask", "acc_packed",
]


def signed(value: int, bits: int) -> int:
    sign = 1 << (bits - 1)
    return value - (1 << bits) if value & sign else value


def sat_add24(lhs: int, rhs: int) -> int:
    return max(ACC_MIN, min(ACC_MAX, lhs + rhs))


def clamp_int8(value: int) -> int:
    return max(-128, min(127, value))


def parse_hex(text: str, field: str, digits: int) -> int:
    if len(text) != digits or any(ch in text.lower() for ch in "xz"):
        raise ValueError(
            f"{field}: expected {digits} known hex digits, got {text!r}"
        )
    return int(text, 16)


def product(case_name: str, row: int, col: int) -> int:
    if case_name == "sat_pos_r1_c1_k567":
        return (-128) * (-128) if row == 0 and col == 0 else 0
    if case_name == "sat_neg_r1_c1_k567":
        return (-128) * 127 if row == 0 and col == 0 else 0
    return (row + 1) * (col + 1)


def bias(case_name: str, col: int) -> int:
    return 0 if case_name.startswith("sat_") else col - 8


def expected_acc(case_name: str, row: int, col: int, k_cycles: int) -> int:
    acc = 0
    for _ in range(k_cycles):
        acc = sat_add24(acc, product(case_name, row, col))
    return sat_add24(acc, bias(case_name, col))


def validate(path: Path, case_name: str) -> tuple[int, int]:
    rows_count, cols_count, k_cycles, cutbit, expect_bp = CASES[case_name]
    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames != HEADER:
            raise ValueError(f"unexpected header: {reader.fieldnames}")
        rows = list(reader)
    if not rows:
        raise ValueError("empty trace")
    if not any(row["done"] == "1" for row in rows):
        raise ValueError("trace never observed done")

    commits = [0] * (SIZE * SIZE)
    input_fires = 0
    outputs = []
    saw_stall = False
    pe00_acc = 0
    pe00_commits = 0
    first_saturation = None

    for index, row in enumerate(rows):
        if int(row["cycle"]) != index:
            raise ValueError(f"cycle sequence breaks at {index}")
        if row["case_name"] != case_name:
            raise ValueError(f"case mismatch at cycle {index}")
        input_fires += int(row["input_fire"])
        commit_mask = parse_hex(row["mac_commit_mask"], "mac_commit_mask", 64)
        acc_packed = parse_hex(row["acc_packed"], "acc_packed", 1536)
        for pe in range(SIZE * SIZE):
            if (commit_mask >> pe) & 1:
                commits[pe] += 1
        if commit_mask & 1:
            pe00_commits += 1
            addend = product(case_name, 0, 0)
            previous = pe00_acc
            pe00_acc = sat_add24(pe00_acc, addend)
            actual = signed(acc_packed & 0xFFFFFF, 24)
            if actual != pe00_acc:
                raise ValueError(
                    f"PE[0][0] commit {pe00_commits}: expected {pe00_acc}, "
                    f"got {actual}"
                )
            if first_saturation is None and (
                previous + addend > ACC_MAX or previous + addend < ACC_MIN
            ):
                first_saturation = pe00_commits
        if row["output_valid"] == "1" and row["output_ready"] == "0":
            saw_stall = True
        if row["output_fire"] == "1":
            outputs.append(row)

    if input_fires != k_cycles:
        raise ValueError(f"input fires: expected {k_cycles}, got {input_fires}")
    for r in range(SIZE):
        for c in range(SIZE):
            expected = k_cycles if r < rows_count and c < cols_count else 0
            actual = commits[r * SIZE + c]
            if actual != expected:
                raise ValueError(
                    f"PE[{r}][{c}] commits: expected {expected}, got {actual}"
                )

    if len(outputs) != rows_count:
        raise ValueError(f"output rows: expected {rows_count}, got {len(outputs)}")
    for expected_row, row in enumerate(outputs):
        if int(row["output_row"]) != expected_row:
            raise ValueError(
                f"output row sequence: expected {expected_row}, "
                f"got {row['output_row']}"
            )
        output_bus = parse_hex(row["output_bus"], "output_bus", 64)
        for c in range(cols_count):
            actual = signed((output_bus >> (16 * c)) & 0xFFFF, 16)
            expected = clamp_int8(
                expected_acc(case_name, expected_row, c, k_cycles) >> cutbit
            )
            if actual != expected:
                raise ValueError(
                    f"output[{expected_row}][{c}]: expected {expected}, "
                    f"got {actual}"
                )
        if output_bus >> (16 * cols_count):
            raise ValueError(f"output row {expected_row} has invalid payload")

    if expect_bp and not saw_stall:
        raise ValueError("backpressure case did not stall")
    if not expect_bp and saw_stall:
        raise ValueError("unexpected output stall")
    if case_name == "sat_pos_r1_c1_k567" and first_saturation != 512:
        raise ValueError(f"positive saturation expected at 512, got {first_saturation}")
    if case_name == "sat_neg_r1_c1_k567" and first_saturation != 517:
        raise ValueError(f"negative saturation expected at 517, got {first_saturation}")

    return len(rows), len(outputs)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--trace", type=Path, required=True)
    parser.add_argument("--case", choices=sorted(CASES), required=True)
    args = parser.parse_args()
    cycles, outputs = validate(args.trace, args.case)
    print(
        f"PASS sau array trace case={args.case} cycles={cycles} "
        f"output_rows={outputs}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
