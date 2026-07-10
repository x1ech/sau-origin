#!/usr/bin/env python3
"""Unit tests for SAU DSE monotonicity validation."""

import unittest

from util.sau.verify_dse import validate_monotonicity


def stats(command_cycles, array_stall=0, output_stall=0):
    return {
        "commandCycles": command_cycles,
        "stallArrayCapacity": array_stall,
        "stallOutputBufferFull": output_stall,
    }


class DseMonotonicityTest(unittest.TestCase):
    def test_accepts_faster_larger_resources_with_matching_stalls(self):
        errors = validate_monotonicity(
            stats(100, array_stall=25),
            stats(60),
            stats(80, output_stall=12),
            stats(65),
        )

        self.assertEqual([], errors)

    def test_rejects_regression_and_missing_stalls(self):
        errors = validate_monotonicity(
            stats(60),
            stats(100),
            stats(65),
            stats(80),
        )

        self.assertEqual(4, len(errors))
        self.assertTrue(any("array_capacity=16" in error for error in errors))
        self.assertTrue(any("stallArrayCapacity" in error for error in errors))
        self.assertTrue(any("output_buffer_entries=8" in error for error in errors))
        self.assertTrue(any("stallOutputBufferFull" in error for error in errors))
