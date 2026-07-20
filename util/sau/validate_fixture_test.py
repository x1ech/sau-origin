#!/usr/bin/env python3
"""Unit tests for the PLAN2 fixture storage-contract validator."""

import unittest

from util.sau.validate_fixture import validate_storage_trace


def row(cycle, event, stream, beat):
    return {
        "cycle": str(cycle),
        "event": event,
        "command_id": "1",
        "stream": stream,
        "beat": str(beat),
    }


MANIFEST = {"elaboration_params": {"SRAM_DELAY": 3}}


class StorageContractValidatorTest(unittest.TestCase):
    def test_accepts_continuous_single_issue_ordered_reads(self):
        rows = [
            row(3, "read_accepted", "operand_a", 0),
            row(4, "read_accepted", "operand_a", 1),
            row(7, "read_response_visible", "operand_a", 0),
            row(8, "read_response_visible", "operand_a", 1),
            row(9, "read_accepted", "operand_b", 0),
            row(13, "read_response_visible", "operand_b", 0),
        ]

        self.assertEqual([], validate_storage_trace(rows, MANIFEST, "fixture"))

    def test_reports_latency_issue_order_and_preload_violations(self):
        rows = [
            row(3, "read_accepted", "operand_a", 0),
            row(3, "read_accepted", "operand_b", 0),
            row(6, "read_response_visible", "operand_b", 0),
            row(7, "read_response_visible", "operand_a", 0),
        ]

        errors = validate_storage_trace(rows, MANIFEST, "fixture")
        self.assertTrue(any("multiple requests" in error for error in errors))
        self.assertTrue(any("out of order" in error for error in errors))
        self.assertTrue(any("SRAM_DELAY + 1" in error for error in errors))
        self.assertTrue(any("before completing A" in error for error in errors))


if __name__ == "__main__":
    unittest.main()
