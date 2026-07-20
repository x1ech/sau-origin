#!/usr/bin/env python3
"""Unit tests for Step 8 source and matrix tooling."""

from pathlib import Path
import tempfile
import unittest

from util.conv_pipeline.compare_pipeline_traces import TRACE_FIELDS
from util.conv_pipeline.step8.run_pipeline_matrix import (
    DEFAULT_MATRIX,
    run_matrix,
)
from util.conv_pipeline.step8.verify_step8_sources import (
    _trace_header,
    verify,
)
from util.conv_pipeline.step8.validate_step8_results import (
    Step8ResultError,
    _artifact_path,
)


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]


class Step8ToolsTest(unittest.TestCase):
    def test_source_contract_and_trace_header(self):
        verify(REPOSITORY_ROOT)
        testbench = (
            REPOSITORY_ROOT /
            "src/sau_n/rtl/tb_im2col_mikui_sau_pipeline.sv"
        ).read_text(encoding="utf-8")
        self.assertEqual(
            _trace_header(testbench),
            ",".join(TRACE_FIELDS) + "\n",
        )

    def test_seven_profile_matrix_dry_run_is_non_overwriting(self):
        with tempfile.TemporaryDirectory() as temporary:
            outdir = Path(temporary) / "matrix"
            summary = run_matrix(
                DEFAULT_MATRIX,
                Path(temporary) / "simv",
                outdir,
                "VCS",
                "VCS test",
                timeout=1,
                dry_run=True,
            )
            self.assertEqual(len(summary), 7)
            self.assertTrue((outdir / "summary.json").is_file())
            for item in summary:
                self.assertEqual(item["returncode"], 0)
                self.assertTrue((outdir / item["runner_log"]).is_file())
            with self.assertRaisesRegex(ValueError, "overwrite"):
                run_matrix(
                    DEFAULT_MATRIX,
                    Path(temporary) / "simv",
                    outdir,
                    "VCS",
                    "VCS test",
                    dry_run=True,
                )

    def test_result_artifact_paths_cannot_escape(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            self.assertEqual(_artifact_path(root, "trace.csv"),
                             root / "trace.csv")
            with self.assertRaisesRegex(Step8ResultError, "escapes"):
                _artifact_path(root, "../trace.csv")


if __name__ == "__main__":
    unittest.main()
