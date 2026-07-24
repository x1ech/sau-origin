#!/usr/bin/env python3
"""Unit tests for the SAU timing trace comparator."""

import io
import os
import tempfile
import unittest
from contextlib import redirect_stdout

from util.sau.compare_trace import (  # noqa: E402
    TraceFormatError,
    compare_rows,
    main,
    read_trace,
)


HEADER = "cycle,event,command_id,stream,address,beat,phase"


def make_trace(rows):
    return HEADER + "\n" + "\n".join(rows) + "\n"


ROWS = [
    {
        "cycle": 0,
        "event": "phase_changed",
        "command_id": 1,
        "stream": "none",
        "address": "0x00000000",
        "beat": 0,
        "phase": "operand_load",
    },
    {
        "cycle": 0,
        "event": "command_accepted",
        "command_id": 1,
        "stream": "none",
        "address": "0x00000000",
        "beat": 0,
        "phase": "operand_load",
    },
    {
        "cycle": 3,
        "event": "read_accepted",
        "command_id": 1,
        "stream": "operand_a",
        "address": "0x29120000",
        "beat": 0,
        "phase": "operand_load",
    },
    {
        "cycle": 7,
        "event": "read_response_visible",
        "command_id": 1,
        "stream": "operand_a",
        "address": "0x00000000",
        "beat": 0,
        "phase": "operand_load",
    },
    {
        "cycle": 77,
        "event": "phase_changed",
        "command_id": 1,
        "stream": "none",
        "address": "0x00000000",
        "beat": 0,
        "phase": "array_active",
    },
    {
        "cycle": 77,
        "event": "array_input_accepted",
        "command_id": 1,
        "stream": "operand_b",
        "address": "0x00000000",
        "beat": 0,
        "phase": "array_active",
    },
    {
        "cycle": 324,
        "event": "result_produced",
        "command_id": 1,
        "stream": "output",
        "address": "0x00000000",
        "beat": 0,
        "phase": "array_active",
    },
    {
        "cycle": 1777,
        "event": "write_accepted",
        "command_id": 1,
        "stream": "output",
        "address": "0x29125940",
        "beat": 0,
        "phase": "writeback",
    },
    {
        "cycle": 2036,
        "event": "command_complete",
        "command_id": 1,
        "stream": "none",
        "address": "0x00000000",
        "beat": 0,
        "phase": "complete",
    },
]


class TraceComparatorTest(unittest.TestCase):
    def test_strict_accepts_identical_rows(self):
        self.assertEqual([], compare_rows(ROWS, ROWS, mode="strict"))

    def test_strict_reports_one_cycle_offset(self):
        actual = [dict(row) for row in ROWS]
        actual[2]["cycle"] = actual[2]["cycle"] + 1

        errors = compare_rows(ROWS, actual, mode="strict")

        self.assertEqual(1, len(errors))
        self.assertIn("cycle", errors[0])
        self.assertIn("row 2", errors[0])

    def test_causal_allows_latency_but_not_dependency_reordering(self):
        delayed = [dict(row, cycle=row["cycle"] + 7) for row in ROWS]

        self.assertEqual([], compare_rows(ROWS, delayed, mode="causal"))
        self.assertNotEqual(
            [], compare_rows(ROWS, list(reversed(delayed)), mode="causal")
        )

    def test_causal_allows_independent_read_response_interleaving(self):
        expected = [
            dict(ROWS[0]),
            dict(ROWS[1]),
            dict(ROWS[2]),
            {
                "cycle": 4,
                "event": "read_accepted",
                "command_id": 1,
                "stream": "operand_a",
                "address": "0x29120020",
                "beat": 1,
                "phase": "operand_load",
            },
            dict(ROWS[3]),
            {
                "cycle": 8,
                "event": "read_response_visible",
                "command_id": 1,
                "stream": "operand_a",
                "address": "0x00000000",
                "beat": 1,
                "phase": "operand_load",
            },
        ] + [dict(row) for row in ROWS[4:]]
        actual = [
            dict(expected[0]),
            dict(expected[1]),
            dict(expected[2]),
            dict(expected[4], cycle=6),
            dict(expected[3], cycle=7),
            dict(expected[5], cycle=8),
        ] + [dict(row) for row in expected[6:]]

        self.assertEqual([], compare_rows(expected, actual, mode="causal"))

    def test_causal_allows_data_event_phase_snapshot_to_move(self):
        actual = [dict(row) for row in ROWS]
        actual[6]["phase"] = "array_drain"

        self.assertEqual([], compare_rows(ROWS, actual, mode="causal"))
        self.assertNotEqual([], compare_rows(ROWS, actual, mode="strict"))

    def test_causal_rejects_phase_transition_sequence_change(self):
        actual = [dict(row) for row in ROWS]
        actual[4]["phase"] = "array_drain"

        errors = compare_rows(ROWS, actual, mode="causal")

        self.assertTrue(any("phase mismatch" in error for error in errors))

    def test_causal_rejects_response_before_its_request(self):
        actual = [dict(row) for row in ROWS]
        actual[2], actual[3] = actual[3], actual[2]

        errors = compare_rows(ROWS, actual, mode="causal")

        self.assertTrue(any("response precedes request" in error
                            for error in errors))

    def test_causal_rejects_actual_cycle_regression(self):
        actual = [dict(row) for row in ROWS]
        actual[4]["cycle"] = actual[3]["cycle"] - 1

        errors = compare_rows(ROWS, actual, mode="causal")

        self.assertTrue(any("nondecreasing" in error for error in errors))

    def test_read_trace_normalizes_to_command_accepted(self):
        rows = [
            "10,phase_changed,1,none,0x00000000,0,operand_load",
            "10,command_accepted,1,none,0x00000000,0,operand_load",
            "13,read_accepted,1,operand_a,0x29120000,0,operand_load",
        ]
        with tempfile.NamedTemporaryFile("w", delete=False) as trace:
            trace.write(make_trace(rows))
            trace_path = trace.name
        self.addCleanup(os.unlink, trace_path)

        parsed = read_trace(trace_path)

        self.assertEqual([0, 0, 3], [row["cycle"] for row in parsed])

    def test_read_trace_accepts_multiple_commands(self):
        rows = [
            "10,phase_changed,1,none,0x00000000,0,operand_load",
            "10,command_accepted,1,none,0x00000000,0,operand_load",
            "13,command_complete,1,none,0x00000000,0,complete",
            "20,phase_changed,2,none,0x00000000,0,operand_load",
            "20,command_accepted,2,none,0x00000000,0,operand_load",
            "23,command_complete,2,none,0x00000000,0,complete",
        ]
        with tempfile.NamedTemporaryFile("w", delete=False) as trace:
            trace.write(make_trace(rows))
            trace_path = trace.name
        self.addCleanup(os.unlink, trace_path)

        parsed = read_trace(trace_path)

        self.assertEqual([0, 0, 3, 10, 10, 13],
                         [row["cycle"] for row in parsed])
        self.assertEqual([1, 1, 1, 2, 2, 2],
                         [row["command_id"] for row in parsed])

    def test_read_trace_rejects_unknown_event(self):
        rows = [
            "0,phase_changed,1,none,0x00000000,0,operand_load",
            "0,command_accepted,1,none,0x00000000,0,operand_load",
            "1,not_real,1,none,0x00000000,0,operand_load",
        ]
        with tempfile.NamedTemporaryFile("w", delete=False) as trace:
            trace.write(make_trace(rows))
            trace_path = trace.name
        self.addCleanup(os.unlink, trace_path)

        with self.assertRaisesRegex(TraceFormatError, "unknown event"):
            read_trace(trace_path)

    def test_cli_returns_one_and_prints_all_mismatches(self):
        with tempfile.NamedTemporaryFile("w", delete=False) as expected:
            expected.write(make_trace([
                "0,phase_changed,1,none,0x00000000,0,operand_load",
                "0,command_accepted,1,none,0x00000000,0,operand_load",
                "3,read_accepted,1,operand_a,0x29120000,0,operand_load",
            ]))
            expected_path = expected.name
        self.addCleanup(os.unlink, expected_path)
        with tempfile.NamedTemporaryFile("w", delete=False) as actual:
            actual.write(make_trace([
                "0,phase_changed,1,none,0x00000000,0,operand_load",
                "0,command_accepted,1,none,0x00000000,0,operand_load",
                "4,read_accepted,1,operand_b,0x29120020,0,operand_load",
            ]))
            actual_path = actual.name
        self.addCleanup(os.unlink, actual_path)

        out = io.StringIO()
        with redirect_stdout(out):
            rc = main(["--mode", "strict", expected_path, actual_path])

        self.assertEqual(1, rc)
        self.assertIn("cycle", out.getvalue())
        self.assertIn("stream", out.getvalue())
        self.assertIn("address", out.getvalue())


if __name__ == "__main__":
    unittest.main()
