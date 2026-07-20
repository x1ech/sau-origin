#!/usr/bin/env python3

from __future__ import annotations

import csv
from pathlib import Path
import tempfile
import unittest

from util.conv_pipeline.step0.verify_step0_trace import (
    ACC_MAX,
    ACC_MIN,
    HEADER,
    saturating_add24,
    validate_trace,
)


class Step0TraceVerifierTest(unittest.TestCase):
    def test_simple_patched_tail_trace(self) -> None:
        rows = []
        for cycle in range(14):
            row = {field: "0" for field in HEADER}
            row.update(
                {
                    "cycle": str(cycle),
                    "case_name": "tail_r1_c1",
                    "expect_patched": "1",
                    "rst_n": "1",
                    "calmode": "1",
                    "calc_cycle": "9",
                    "row_num": "1",
                    "col_num": "1",
                    "finish_row": "1",
                    "finish_col": "1",
                    "activation_bus": "0",
                    "weight_bus": "0",
                    "bias_bus": "0",
                    "os_valid": "0",
                    "pe_valid": "0",
                    "output_bus": "0",
                    "pe_mac_commit_mask": "0",
                    "pe_add_commit_mask": "0",
                    "pe_acc_packed": "0",
                }
            )
            if cycle == 0:
                row["ins_valid"] = "1"
            if 1 <= cycle <= 9:
                row["en_i"] = "1"
            if cycle == 12:
                row["row_score_valid"] = "1"
                row["cal_finish"] = "1"
                row["output_bus"] = "1"
            rows.append(row)

        with tempfile.TemporaryDirectory() as directory:
            trace = Path(directory) / "trace.csv"
            with trace.open("w", encoding="utf-8", newline="") as handle:
                writer = csv.DictWriter(handle, fieldnames=HEADER, lineterminator="\n")
                writer.writeheader()
                writer.writerows(rows)
            self.assertEqual(
                validate_trace(trace, "tail_r1_c1", "integration"),
                (14, 1),
            )

    def test_saturation_ordinals(self) -> None:
        value = 0
        positive_first = None
        for ordinal in range(1, 568):
            previous = value
            value = saturating_add24(value, 16384)
            if positive_first is None and previous + 16384 > ACC_MAX:
                positive_first = ordinal
        self.assertEqual(positive_first, 512)
        self.assertEqual(value, ACC_MAX)

        value = 0
        negative_first = None
        for ordinal in range(1, 568):
            previous = value
            value = saturating_add24(value, -16256)
            if negative_first is None and previous - 16256 < ACC_MIN:
                negative_first = ordinal
        self.assertEqual(negative_first, 517)
        self.assertEqual(value, ACC_MIN)


if __name__ == "__main__":
    unittest.main()
