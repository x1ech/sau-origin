import json
from pathlib import Path
import tempfile
import unittest

from util.im2col.im2col_fixture import load_fixture
from util.im2col.logical_oracle import iter_logical_feed_vectors
from util.im2col.rtl_fixture_runner import (
    RtlFixtureError,
    build_plusargs,
    validate_ready,
    write_manifest,
)


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
MATRIX_PATH = REPOSITORY_ROOT / "tests/gem5/im2col/golden_matrix.json"


class RtlFixtureRunnerTest(unittest.TestCase):
    def test_builds_complete_explicit_plusargs(self):
        fixture = MATRIX_PATH.parent / "fixtures/w5_pack3_pad1.json"
        loaded = load_fixture(fixture)
        plusargs = build_plusargs(
            loaded, "/tmp/im2col trace.csv", 11, 1)
        self.assertEqual(plusargs[0], "+FIXTURE_MODE")
        self.assertIn("+CFG_N=1", plusargs)
        self.assertIn("+CFG_C=2", plusargs)
        self.assertIn("+CFG_KERNEL_PATTERN=ffff", plusargs)
        self.assertIn("+READY_PERIOD=11", plusargs)
        self.assertIn("+READY_HIGH_CYCLES=1", plusargs)
        self.assertTrue(any(
            argument.startswith("+RESOLVED_CONFIG_SHA256=")
            for argument in plusargs
        ))

    def test_rejects_invalid_ready_patterns(self):
        for period, high_cycles in ((0, 1), (1, 0), (3, 4), (True, 1)):
            with self.subTest(period=period, high_cycles=high_cycles):
                with self.assertRaises(RtlFixtureError):
                    validate_ready(period, high_cycles)

    def test_golden_matrix_resolves_and_covers_frozen_boundaries(self):
        matrix = json.loads(MATRIX_PATH.read_text(encoding="utf-8"))
        self.assertEqual(matrix["schema_version"], 1)
        loaded_runs = []
        for run in matrix["runs"]:
            validate_ready(run["ready_period"], run["ready_high_cycles"])
            fixture = MATRIX_PATH.parent / run["fixture"]
            loaded_runs.append((load_fixture(fixture), run))

        widths = {loaded.config.w for loaded, _ in loaded_runs}
        self.assertTrue({1, 5, 16, 17, 20}.issubset(widths))
        self.assertTrue(any(
            loaded.config.n > 1 and loaded.config.c > 1 and
            loaded.config.spad_base > 0
            for loaded, _ in loaded_runs
        ))
        self.assertTrue(any(
            loaded.config.dilation_h > 1 or loaded.config.dilation_w > 1
            for loaded, _ in loaded_runs
        ))
        self.assertTrue(any(
            run["ready_high_cycles"] < run["ready_period"]
            for _, run in loaded_runs
        ))

        padding = next(
            loaded for loaded, _ in loaded_runs
            if loaded.config.name == "w5_all_padding_vector"
        )
        vectors = list(iter_logical_feed_vectors(padding.config))
        self.assertTrue(vectors)
        self.assertTrue(all(
            vector.feed_mask != 0 and vector.feed_data == 0
            for vector in vectors
        ))

    def test_manifest_records_sources_runtime_and_cycles(self):
        fixture = MATRIX_PATH.parent / "fixtures/w5_pack3_pad1.json"
        loaded = load_fixture(fixture)
        with tempfile.TemporaryDirectory() as directory:
            manifest_path = Path(directory) / "manifest.json"
            write_manifest(
                manifest_path,
                loaded,
                fixture,
                Path(directory) / "trace.csv",
                7,
                2,
                "example-simulator",
                "1.2.3",
                ("simv", "+FIXTURE_MODE"),
                175,
                176,
            )
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        self.assertEqual(
            manifest["resolved_config_sha256"],
            loaded.resolved_config_sha256,
        )
        self.assertEqual(manifest["post_done_drain_cycles"], 1)
        self.assertEqual(manifest["fixed_hardware"]["sp_banks"], 16)
        self.assertRegex(manifest["dut_sha256"], r"^[0-9a-f]{64}$")
        self.assertRegex(manifest["testbench_sha256"], r"^[0-9a-f]{64}$")


if __name__ == "__main__":
    unittest.main()
