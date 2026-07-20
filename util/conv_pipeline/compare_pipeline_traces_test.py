#!/usr/bin/env python3
"""Tests for the strict canonical pipeline trace comparator."""

import csv
from pathlib import Path
import re
import tempfile
import unittest

from util.conv_pipeline.compare_pipeline_traces import (
    HEX_DIGITS,
    TRACE_FIELDS,
    TraceFormatError,
    TraceMismatch,
    compare_trace_files,
    load_trace,
    write_trace,
)


HASH = "a" * 64
REPO_ROOT = Path(__file__).resolve().parents[2]


def canonical_row(cycle, drained=True):
    row = {field: "0" for field in TRACE_FIELDS}
    row.update({
        "schema_version": "1",
        "resolved_config_sha256": HASH,
        "cycle": str(cycle),
        "pipeline_state": "1",
        "im2col_state": "1",
        "im2col_feed_ready": "1",
        "sa_state": "0",
        "output_grant": "1",
        "drained": str(int(drained)),
    })
    for field, digits in HEX_DIGITS.items():
        row[field] = "0x" + "0" * digits
    return row


def write_rows(path, rows, header=TRACE_FIELDS, newline="\n"):
    with open(path, "w", encoding="utf-8", newline="") as output:
        writer = csv.writer(output, lineterminator=newline)
        writer.writerow(header)
        for row in rows:
            writer.writerow([row[field] for field in header])


def set_pe(row, field, pe_index, bits, value):
    packed = int(row[field], 16)
    mask = ((1 << bits) - 1) << (pe_index * bits)
    packed = (packed & ~mask) | (value << (pe_index * bits))
    row[field] = "0x" + format(packed, f"0{HEX_DIGITS[field]}x")


class PipelineTraceComparatorTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        root = Path(self.directory.name)
        self.expected = root / "rtl.csv"
        self.actual = root / "gem5.csv"
        self.round_trip = root / "round_trip.csv"

    def tearDown(self):
        self.directory.cleanup()

    def test_valid_trace_round_trips_and_compares(self):
        rows = [canonical_row(0, False), canonical_row(1)]
        write_rows(self.expected, rows)
        loaded = load_trace(self.expected)
        write_trace(loaded, self.round_trip)
        self.assertEqual(
            self.expected.read_bytes(), self.round_trip.read_bytes())
        self.assertEqual(
            2, compare_trace_files(self.expected, self.round_trip))

    def test_cpp_writer_and_python_loader_freeze_the_same_schema(self):
        header = (
            REPO_ROOT / "src/sau_n/conv_pipeline_io.hh"
        ).read_text(encoding="utf-8")
        match = re.search(
            r"CanonicalPipelineTraceFields = \{(.*?)\n\};",
            header,
            re.DOTALL,
        )
        self.assertIsNotNone(match)
        self.assertEqual(
            TRACE_FIELDS,
            tuple(re.findall(r'"([a-z0-9_]+)"', match.group(1))),
        )

    def test_reports_first_regular_field_and_length_difference(self):
        expected = canonical_row(0)
        actual = canonical_row(0)
        expected["tile_index"] = "3"
        actual["tile_index"] = "4"
        write_rows(self.expected, [expected])
        write_rows(self.actual, [actual])
        with self.assertRaisesRegex(
                TraceMismatch, r"cycle 0, field tile_index"):
            compare_trace_files(self.expected, self.actual)

        write_rows(
            self.expected,
            [canonical_row(0, False), canonical_row(1)],
        )
        write_rows(self.actual, [canonical_row(0)])
        with self.assertRaisesRegex(TraceMismatch, "actual trace ended"):
            compare_trace_files(self.expected, self.actual)

    def test_reports_first_pe_for_every_packed_pe_field(self):
        cases = {
            "pe_valid_mask": (1, 1),
            "pe_mac_commit_mask": (1, 1),
            "pe_add_commit_mask": (1, 1),
            "pe_activations": (8, 0x81),
            "pe_weights": (8, 0x7f),
            "pe_accumulators": (24, 0xffffff),
        }
        for field, (bits, value) in cases.items():
            with self.subTest(field=field):
                expected = canonical_row(0)
                actual = canonical_row(0)
                pe_index = 2 * 16 + 3
                if field in ("pe_activations", "pe_weights"):
                    set_pe(expected, "pe_valid_mask", pe_index, 1, 1)
                    set_pe(actual, "pe_valid_mask", pe_index, 1, 1)
                if field == "pe_accumulators":
                    set_pe(expected, "pe_add_commit_mask", pe_index, 1, 1)
                    set_pe(actual, "pe_add_commit_mask", pe_index, 1, 1)
                set_pe(expected, field, pe_index, bits, value)
                write_rows(self.expected, [expected])
                write_rows(self.actual, [actual])
                with self.assertRaisesRegex(
                        TraceMismatch,
                        rf"field {field}, PE\[2\]\[3\]"):
                    compare_trace_files(self.expected, self.actual)

    def test_rejects_header_cycle_hash_and_hex_format(self):
        write_rows(self.expected, [canonical_row(0)], TRACE_FIELDS[:-1])
        with self.assertRaisesRegex(TraceFormatError, "header differs"):
            load_trace(self.expected)

        row = canonical_row(1)
        write_rows(self.expected, [row])
        with self.assertRaisesRegex(TraceFormatError, "expected 0"):
            load_trace(self.expected)

        row = canonical_row(0)
        row["resolved_config_sha256"] = "A" * 64
        write_rows(self.expected, [row])
        with self.assertRaisesRegex(TraceFormatError, "lowercase hex"):
            load_trace(self.expected)

        row = canonical_row(0)
        row["pe_valid_mask"] = "0x" + "x" * 64
        write_rows(self.expected, [row])
        with self.assertRaisesRegex(TraceFormatError, "pe_valid_mask"):
            load_trace(self.expected)

    def test_rejects_invalid_payloads_and_control_relationships(self):
        row = canonical_row(0)
        set_pe(row, "pe_activations", 0, 8, 1)
        write_rows(self.expected, [row])
        with self.assertRaisesRegex(TraceFormatError, r"PE\[0\]\[0\]"):
            load_trace(self.expected)

        row = canonical_row(0)
        row["output_slots"] = "0x" + "0" * 63 + "1"
        write_rows(self.expected, [row])
        with self.assertRaisesRegex(TraceFormatError, "output_slots"):
            load_trace(self.expected)

        row = canonical_row(0)
        row["im2col_feed_handshake"] = "1"
        write_rows(self.expected, [row])
        with self.assertRaisesRegex(TraceFormatError, "handshake"):
            load_trace(self.expected)

        launch = canonical_row(0, False)
        launch.update({
            "sa_ins_valid": "1",
            "sa_calc_cycles": "9",
            "sa_valid_rows": "2",
            "sa_valid_columns": "1",
        })
        stream = canonical_row(1)
        stream.update({
            "sa_input_valid": "1",
            "sa_row_mask": "0x0001",
            "sa_column_mask": "0x0001",
        })
        write_rows(self.expected, [launch, stream])
        with self.assertRaisesRegex(TraceFormatError, "active SA config"):
            load_trace(self.expected)

    def test_rejects_nonfinal_drained_crlf_and_missing_final_lf(self):
        write_rows(self.expected, [canonical_row(0, False)])
        with self.assertRaisesRegex(TraceFormatError, "final cycle"):
            load_trace(self.expected)

        write_rows(
            self.expected, [canonical_row(0)], newline="\r\n")
        with self.assertRaisesRegex(TraceFormatError, "LF newlines"):
            load_trace(self.expected)

        write_rows(self.actual, [canonical_row(0)])
        self.actual.write_bytes(self.actual.read_bytes().rstrip(b"\n"))
        with self.assertRaisesRegex(TraceFormatError, "end with an LF"):
            load_trace(self.actual)


if __name__ == "__main__":
    unittest.main()
