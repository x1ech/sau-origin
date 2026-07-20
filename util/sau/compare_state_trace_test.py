#!/usr/bin/env python3
"""Unit tests for SAU semantic-state trace normalization and comparison."""

import csv
import os
import tempfile
import unittest

from util.sau.compare_state_trace import (
    HEADER,
    compare_state_rows,
    normalize_rtl_diagnostic,
    read_state_trace,
)


class StateTraceComparatorTest(unittest.TestCase):
    def write_file(self, contents):
        handle = tempfile.NamedTemporaryFile("w", delete=False)
        self.addCleanup(os.unlink, handle.name)
        handle.write(contents)
        handle.close()
        return handle.name

    def test_normalizes_rtl_start_and_complete(self):
        path = self.write_file(
            "cycle,command_id,start,core_state,input_switch_f,command_done\n"
            "10,1,1,IDLE,2'b00,0\n"
            "11,1,0,REGISTER_LOAD,2'b00,0\n"
            "12,1,0,TRANSPOSE_LOAD,2'b00,0\n"
            "13,1,0,REUSE_LOAD,2'b01,0\n"
            "14,1,0,IDLE,2'b00,1\n"
        )
        rows = normalize_rtl_diagnostic(path)
        self.assertEqual(
            ["resident_load", "transpose_setup", "flow_execute", "complete"],
            [row["schedule_state"] for row in rows],
        )
        self.assertEqual([0, 2, 3, 4], [row["cycle"] for row in rows])

    def test_reports_transition_cause_mismatch(self):
        rows = [
            {
                "cycle": 0,
                "command_id": 1,
                "schedule_state": "resident_load",
                "rtl_state": "REGISTER_LOAD",
                "input_switch": "00",
                "transition_cause": "command_accepted",
            }
        ]
        actual = [dict(rows[0], transition_cause="unexpected")]
        errors = compare_state_rows(rows, actual)
        self.assertEqual(1, len(errors))
        self.assertIn("transition_cause", errors[0])

    def test_marks_a_switch_only_change_as_pipeline_visibility(self):
        path = self.write_file(
            "cycle,command_id,start,core_state,input_switch_f,command_done\n"
            "10,1,1,IDLE,2'b00,0\n"
            "11,1,0,REUSE_LOAD,2'b00,0\n"
            "12,1,0,REUSE_LOAD,2'b01,0\n"
        )
        rows = normalize_rtl_diagnostic(path)
        self.assertEqual("transpose_complete", rows[1]["transition_cause"])
        self.assertEqual("input_switch_visible", rows[2]["transition_cause"])

    def test_reads_independent_state_schema(self):
        path = self.write_file(
            HEADER + "\n"
            "20,1,resident_load,REGISTER_LOAD,00,command_accepted\n"
        )
        rows = read_state_trace(path)
        self.assertEqual(0, rows[0]["cycle"])


if __name__ == "__main__":
    unittest.main()
