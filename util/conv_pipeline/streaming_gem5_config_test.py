#!/usr/bin/env python3

import unittest
from pathlib import Path

from util.conv_pipeline.streaming_fixture import load_streaming_fixture
from util.conv_pipeline.streaming_gem5_config import (
    streaming_simobject_parameters,
)


class StreamingGem5ConfigTest(unittest.TestCase):
    @staticmethod
    def loaded_fixture():
        root = Path(__file__).resolve().parents[2]
        return load_streaming_fixture(
            root / "tests/gem5/conv_pipeline/fixtures/"
            "08_n1_c16_h16_w32_oc16.json")

    def test_adds_only_streaming_trace_mode_to_shared_parameters(self):
        loaded = self.loaded_fixture()
        params = streaming_simobject_parameters(
            loaded, "/tmp/trace.csv", "/tmp/output.csv", 7, 2, True)
        self.assertTrue(params["detailed_pe_trace"])
        self.assertEqual(1, params["stride_h"])
        self.assertEqual(32, params["out_w"])
        self.assertEqual(7, params["output_ready_period"])

    def test_rejects_non_boolean_trace_mode(self):
        loaded = self.loaded_fixture()
        with self.assertRaisesRegex(ValueError, "must be a bool"):
            streaming_simobject_parameters(
                loaded, "/tmp/t", "/tmp/o", detailed_pe_trace=1)


if __name__ == "__main__":
    unittest.main()
