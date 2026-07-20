#!/usr/bin/env python3
"""Tests for ConvPipelineTiming parameter conversion."""

from pathlib import Path
import unittest

from util.conv_pipeline.gem5_config import simobject_parameters
from util.conv_pipeline.pipeline_contract import PipelineConfigError
from util.conv_pipeline.pipeline_fixture import load_fixture


REPO_ROOT = Path(__file__).resolve().parents[2]
FIXTURE = (
    REPO_ROOT / "tests/gem5/conv_pipeline/fixtures/"
    "step1_n1_c2_h4_w5_oc3.json"
)


class Gem5ConfigTest(unittest.TestCase):
    def test_maps_every_resolved_field_and_runtime_parameter(self):
        loaded = load_fixture(FIXTURE)
        params = simobject_parameters(
            loaded,
            "/tmp/trace.csv",
            "/tmp/output.csv",
            output_ready_period=11,
            output_ready_high_cycles=1,
        )

        self.assertEqual(set(params), {
            "schema_version", "fixture_name", "im2col_name", "n", "c",
            "h", "w", "out_h", "out_w", "kernel_h", "kernel_w",
            "stride_h", "stride_w", "dilation_h", "dilation_w",
            "pad_top", "pad_left", "spad_base", "cfg_dw_mode",
            "cfg_kernel_pattern", "input_generator", "out_channels",
            "cutbit", "weight_generator", "bias_generator",
            "resolved_config_sha256", "trace_file", "output_file",
            "output_ready_period", "output_ready_high_cycles",
        })
        self.assertEqual(params["c"], 2)
        self.assertEqual(params["out_channels"], 3)
        self.assertEqual(params["resolved_config_sha256"],
                         loaded.resolved_config_sha256)
        self.assertEqual(params["output_ready_period"], 11)

    def test_rejects_invalid_runtime_ready(self):
        loaded = load_fixture(FIXTURE)
        with self.assertRaises(PipelineConfigError):
            simobject_parameters(
                loaded, "/tmp/t", "/tmp/o",
                output_ready_period=2,
                output_ready_high_cycles=3,
            )

    def test_rejects_empty_output_paths(self):
        loaded = load_fixture(FIXTURE)
        with self.assertRaisesRegex(ValueError, "trace path"):
            simobject_parameters(loaded, "", "/tmp/o")
        with self.assertRaisesRegex(ValueError, "output path"):
            simobject_parameters(loaded, "/tmp/t", "")


if __name__ == "__main__":
    unittest.main()
