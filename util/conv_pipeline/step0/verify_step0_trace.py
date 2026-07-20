#!/usr/bin/env python3
"""Validate the directed Mikui SA_ENGINE Step 0 trace."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path


HEADER = [
    "cycle",
    "case_name",
    "expect_patched",
    "rst_n",
    "en_i",
    "flag_o",
    "flag_o_ready",
    "ins_valid",
    "calmode",
    "flowmode",
    "register_mode",
    "shift_mode",
    "shift_ctl",
    "calc_cycle",
    "row_num",
    "col_num",
    "activation_bus",
    "weight_bus",
    "bias_bus",
    "sa_state",
    "finish_row",
    "finish_col",
    "datain_cnt",
    "os_valid",
    "pe_valid",
    "storage_ready",
    "internal_valid",
    "cnt_o",
    "row_score_valid",
    "row_seq",
    "cal_finish",
    "output_bus",
    "pe_mac_commit_mask",
    "pe_add_commit_mask",
    "pe_acc_packed",
]

CASES = {
    "tail_r1_c1": (1, 1, 9, 0, False),
    "tail_r15_c15": (15, 15, 9, 5, False),
    "tail_r16_c16": (16, 16, 9, 5, False),
    "mapping_k9": (16, 16, 9, 5, False),
    "control_k9_bp": (3, 3, 9, 2, True),
    "sat_pos_k567": (1, 1, 567, 16, False),
    "sat_neg_k567": (1, 1, 567, 16, False),
}

ACC_MIN = -(1 << 23)
ACC_MAX = (1 << 23) - 1


def parse_decimal(value: str, field: str) -> int:
    if not value or any(ch in value.lower() for ch in "xz"):
        raise ValueError(f"{field} is not a known decimal value: {value!r}")
    return int(value, 10)


def parse_hex(value: str, field: str) -> int:
    if not value or any(ch in value.lower() for ch in "xz"):
        raise ValueError(f"{field} is not a known hexadecimal value: {value!r}")
    return int(value, 16)


def signed(value: int, bits: int) -> int:
    sign = 1 << (bits - 1)
    return value - (1 << bits) if value & sign else value


def clamp_int8(value: int) -> int:
    return max(-128, min(127, value))


def saturating_add24(lhs: int, rhs: int) -> int:
    return max(ACC_MIN, min(ACC_MAX, lhs + rhs))


def expected_output(case_name: str, row: int, col: int, cutbit: int) -> int:
    if case_name == "sat_pos_k567":
        acc = ACC_MAX
    elif case_name == "sat_neg_k567":
        acc = ACC_MIN
    else:
        acc = 9 * (row + 1) * (col + 1) + (col - 8)
    return clamp_int8(acc >> cutbit)


def validate_trace(
    path: Path, case_name: str, variant: str
) -> tuple[int, int]:
    requested_rows, requested_cols, k_cycles, cutbit, backpressure = (
        CASES[case_name]
    )
    patched = variant == "integration"
    expected_finish_rows = requested_rows if patched else 16
    expected_finish_cols = requested_cols if patched else 16

    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames != HEADER:
            raise ValueError(
                f"unexpected header: expected {HEADER}, got {reader.fieldnames}"
            )
        rows = list(reader)

    if not rows:
        raise ValueError("trace has no data rows")

    for cycle, row in enumerate(rows):
        if parse_decimal(row["cycle"], "cycle") != cycle:
            raise ValueError(f"cycle sequence breaks at row {cycle}")
        if row["case_name"] != case_name:
            raise ValueError(f"case_name mismatch at cycle {cycle}")
        if parse_decimal(row["expect_patched"], "expect_patched") != int(
            patched
        ):
            raise ValueError(f"variant marker mismatch at cycle {cycle}")
        if parse_decimal(row["finish_row"], "finish_row") != expected_finish_rows:
            raise ValueError(
                f"finish_row mismatch at cycle {cycle}: expected "
                f"{expected_finish_rows}, got {row['finish_row']}"
            )
        if parse_decimal(row["finish_col"], "finish_col") != expected_finish_cols:
            raise ValueError(
                f"finish_col mismatch at cycle {cycle}: expected "
                f"{expected_finish_cols}, got {row['finish_col']}"
            )

    en_cycles = sum(parse_decimal(row["en_i"], "en_i") for row in rows)
    if en_cycles != k_cycles:
        raise ValueError(f"EN_i cycles: expected {k_cycles}, got {en_cycles}")
    ins_cycles = sum(
        parse_decimal(row["ins_valid"], "ins_valid") for row in rows
    )
    if ins_cycles != 1:
        raise ValueError(f"ins_valid cycles: expected 1, got {ins_cycles}")

    output_rows = [row for row in rows if row["row_score_valid"] == "1"]
    if len(output_rows) != expected_finish_rows:
        raise ValueError(
            f"output rows: expected {expected_finish_rows}, "
            f"got {len(output_rows)}"
        )
    for expected_row, row in enumerate(output_rows):
        row_seq = parse_decimal(row["row_seq"], "row_seq")
        if row_seq != expected_row:
            raise ValueError(
                f"row sequence mismatch: expected {expected_row}, got {row_seq}"
            )
        output_bus = parse_hex(row["output_bus"], "output_bus")
        for col in range(expected_finish_cols):
            actual = signed((output_bus >> (16 * col)) & 0xFFFF, 16)
            expected = expected_output(case_name, expected_row, col, cutbit)
            if actual != expected:
                raise ValueError(
                    f"output[{expected_row}][{col}]: expected {expected}, "
                    f"got {actual}"
                )

    finish_rows = [row for row in rows if row["cal_finish"] == "1"]
    if len(finish_rows) != 1:
        raise ValueError(
            f"cal_finish pulses: expected 1, got {len(finish_rows)}"
        )
    if finish_rows[0] is not output_rows[-1]:
        raise ValueError("cal_finish is not aligned with the last registered output")

    if backpressure:
        stalled = [
            row
            for row in rows
            if row["flag_o_ready"] == "0"
            and (row["flag_o"] == "1" or row["cnt_o"] not in ("0", "x"))
        ]
        if not stalled:
            raise ValueError("backpressure case did not stall the output sequence")

    if case_name.startswith("sat_"):
        product = 16384 if case_name == "sat_pos_k567" else -16256
        expected_first_saturation = (
            512 if case_name == "sat_pos_k567" else 517
        )
        acc = 0
        commits = 0
        first_saturation = None
        for index, row in enumerate(rows[:-1]):
            mask_text = row["pe_mac_commit_mask"]
            if any(ch in mask_text.lower() for ch in "xz"):
                continue
            if parse_hex(mask_text, "pe_mac_commit_mask") & 1:
                commits += 1
                previous = acc
                acc = saturating_add24(acc, product)
                if first_saturation is None and acc in (ACC_MIN, ACC_MAX) and (
                    previous + product < ACC_MIN
                    or previous + product > ACC_MAX
                ):
                    first_saturation = commits
                packed = parse_hex(
                    rows[index + 1]["pe_acc_packed"], "pe_acc_packed"
                )
                actual_acc = signed(packed & 0xFFFFFF, 24)
                if actual_acc != acc:
                    raise ValueError(
                        f"PE[0][0] MAC commit {commits}: expected acc {acc}, "
                        f"got {actual_acc}"
                    )
        if commits != k_cycles:
            raise ValueError(
                f"PE[0][0] MAC commits: expected {k_cycles}, got {commits}"
            )
        if first_saturation != expected_first_saturation:
            raise ValueError(
                f"first saturation: expected MAC {expected_first_saturation}, "
                f"got {first_saturation}"
            )

    return len(rows), len(output_rows)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--trace", type=Path, required=True)
    parser.add_argument("--case", choices=sorted(CASES), required=True)
    parser.add_argument(
        "--variant", choices=("original", "integration"), required=True
    )
    args = parser.parse_args()
    cycles, outputs = validate_trace(args.trace, args.case, args.variant)
    print(
        f"PASS step0 trace variant={args.variant} case={args.case} "
        f"cycles={cycles} output_rows={outputs}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
