#!/usr/bin/env python3
"""Unit tests for the strict Im2Col per-cycle trace comparator."""

import csv
from pathlib import Path
import tempfile
import unittest

from util.im2col.compare_traces import (
    TraceFormatError,
    TraceMismatch,
    compare_trace_files,
    load_trace,
)
from util.im2col.im2col_contract import TRACE_FIELDS


HASH = "a" * 64


def canonical_row(cycle):
    row = {field: "0" for field in TRACE_FIELDS}
    row.update({
        "schema_version": "1",
        "resolved_config_sha256": HASH,
        "cycle": str(cycle),
        "state": "1",
        "busy": "1",
        "done": "0",
        "fifo_count": "0",
        "fifo_rptr": "0",
        "fifo_wptr": "0",
        "req_valid": "0x0000",
        "resp_valid": "0x0000",
        "feed_valid": "0",
        "feed_ready": "1",
        "feed_data": "0x" + "0" * 32,
        "feed_mask": "0x0000",
    })
    for bank in range(16):
        row[f"req_addr_b{bank:02d}"] = "0x000"
        row[f"resp_data_b{bank:02d}"] = "0x00"
    return row


def write_trace(path, rows, header=TRACE_FIELDS, newline="\n"):
    with open(path, "w", encoding="utf-8", newline="") as output:
        writer = csv.writer(output, lineterminator=newline)
        writer.writerow(header)
        for row in rows:
            writer.writerow([row[field] for field in header])


class TraceComparatorTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.expected = Path(self.directory.name) / "rtl.csv"
        self.actual = Path(self.directory.name) / "gem5.csv"

    def tearDown(self):
        self.directory.cleanup()

    def test_identical_traces_pass_and_report_cycle_count(self):
        rows = [canonical_row(0), canonical_row(1)]
        write_trace(self.expected, rows)
        write_trace(self.actual, rows)
        self.assertEqual(2, compare_trace_files(self.expected, self.actual))

    def test_first_field_mismatch_reports_cycle_and_bank_field(self):
        expected = [canonical_row(0), canonical_row(1)]
        actual = [canonical_row(0), canonical_row(1)]
        expected[1]["req_valid"] = "0x0008"
        expected[1]["req_addr_b03"] = "0x123"
        actual[1]["req_valid"] = "0x0008"
        actual[1]["req_addr_b03"] = "0x124"
        write_trace(self.expected, expected)
        write_trace(self.actual, actual)

        with self.assertRaisesRegex(
                TraceMismatch, r"cycle 1, field req_addr_b03"):
            compare_trace_files(self.expected, self.actual)

    def test_length_mismatch_reports_first_missing_cycle(self):
        write_trace(
            self.expected, [canonical_row(0), canonical_row(1)])
        write_trace(self.actual, [canonical_row(0)])
        with self.assertRaisesRegex(TraceMismatch, r"cycle 1: actual trace"):
            compare_trace_files(self.expected, self.actual)

    def test_rejects_header_and_cycle_gap(self):
        write_trace(self.expected, [canonical_row(0)], TRACE_FIELDS[:-1])
        with self.assertRaisesRegex(TraceFormatError, "header differs"):
            load_trace(self.expected)

        write_trace(self.actual, [canonical_row(1)])
        with self.assertRaisesRegex(TraceFormatError, "expected 0"):
            load_trace(self.actual)

    def test_rejects_noncanonical_hex_and_invalid_payload(self):
        row = canonical_row(0)
        row["req_valid"] = "0x1"
        write_trace(self.expected, [row])
        with self.assertRaisesRegex(TraceFormatError, "req_valid"):
            load_trace(self.expected)

        row = canonical_row(0)
        row["req_addr_b03"] = "0x123"
        write_trace(self.actual, [row])
        with self.assertRaisesRegex(TraceFormatError, "must be zero"):
            load_trace(self.actual)

        row = canonical_row(0)
        row["feed_data"] = "0x" + "0" * 31 + "1"
        write_trace(self.actual, [row])
        with self.assertRaisesRegex(TraceFormatError, "feed_data must be zero"):
            load_trace(self.actual)

    def test_rejects_crlf_and_missing_final_lf(self):
        write_trace(self.expected, [canonical_row(0)], newline="\r\n")
        with self.assertRaisesRegex(TraceFormatError, "LF newlines"):
            load_trace(self.expected)

        write_trace(self.actual, [canonical_row(0)])
        self.actual.write_bytes(self.actual.read_bytes().rstrip(b"\n"))
        with self.assertRaisesRegex(TraceFormatError, "end with an LF"):
            load_trace(self.actual)


if __name__ == "__main__":
    unittest.main()
