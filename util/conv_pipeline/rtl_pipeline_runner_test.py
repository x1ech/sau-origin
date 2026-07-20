#!/usr/bin/env python3
"""Unit tests for the Step 8 RTL fixture runner helpers."""

import csv
from pathlib import Path
import tempfile
import unittest

from util.conv_pipeline.convolution_oracle import generate_convolution
from util.conv_pipeline.pipeline_fixture import load_fixture
from util.conv_pipeline.rtl_pipeline_runner import (
    RtlPipelineError,
    build_plusargs,
    validate_output_file,
)


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
FIXTURE = (
    REPOSITORY_ROOT /
    "tests/gem5/conv_pipeline/fixtures/01_c1_w1_oc1_ones.json"
)


class RtlPipelineRunnerTest(unittest.TestCase):
    def setUp(self):
        self.loaded = load_fixture(FIXTURE)

    def test_plusargs_cover_resolved_config_and_runtime(self):
        plusargs = build_plusargs(
            self.loaded, "trace.csv", "output.csv", 11, 1)
        values = dict(argument[1:].split("=", 1) for argument in plusargs)
        self.assertEqual(values["CFG_N"], "1")
        self.assertEqual(values["CFG_WEIGHT_GENERATOR"], "2")
        self.assertEqual(values["EXPECTED_TILES"], "1")
        self.assertEqual(values["EXPECTED_OUTPUTS"], "3")
        self.assertEqual(values["OUTPUT_READY_PERIOD"], "11")
        self.assertEqual(values["OUTPUT_READY_HIGH_CYCLES"], "1")
        self.assertEqual(
            values["RESOLVED_CONFIG_SHA256"],
            self.loaded.resolved_config_sha256,
        )

    def test_output_validator_checks_every_nchw_value(self):
        oracle = generate_convolution(self.loaded.config)
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "output.csv"
            with output.open("w", newline="", encoding="utf-8") as stream:
                writer = csv.writer(stream, lineterminator="\n")
                writer.writerow(("n", "oc", "oh", "ow", "value"))
                for item in oracle.outputs:
                    writer.writerow((
                        item.n, item.output_channel, item.oh, item.ow,
                        item.value,
                    ))
            self.assertEqual(
                validate_output_file(self.loaded, output),
                len(oracle.outputs),
            )
            rows = output.read_text(encoding="utf-8").splitlines()
            fields = rows[-1].split(",")
            fields[-1] = str(int(fields[-1]) + 1)
            rows[-1] = ",".join(fields)
            output.write_text("\n".join(rows) + "\n", encoding="utf-8")
            with self.assertRaisesRegex(RtlPipelineError, "mismatch"):
                validate_output_file(self.loaded, output)


if __name__ == "__main__":
    unittest.main()
