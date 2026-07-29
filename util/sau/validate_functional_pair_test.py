#!/usr/bin/env python3
"""Unit tests for the SAU paired-functional-fixture validator."""

import json
import tempfile
import unittest
from pathlib import Path

from util.sau.validate_functional_pair import validate_pair


class FunctionalPairTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)

    def fixture(self, name, trans_mode, initial=b"same", output=b"result"):
        directory = self.root / name
        directory.mkdir()
        manifest = {
            "csr_modes": {
                "trans_mode": trans_mode,
                "reuse_mode": 1,
                "sa_flow_mode": 0,
                "cutbit": 8,
            },
            "matrix": {"m": 32, "k": 32, "n": 32},
            "elaboration_params": {
                "SA_SIZE": 32,
                "SRAM_DELAY": 3,
            },
            "memory_contract": {
                "external_beat_bytes": 32,
                "output_base": "0x29120c00",
                "output_end_exclusive": "0x29121000",
                "output_size_bytes": 1024,
                "boundary_trace": "boundary.csv",
            },
        }
        (directory / "manifest.json").write_text(
            json.dumps(manifest), encoding="utf-8"
        )
        (directory / "initial_memory.hex").write_bytes(initial)
        (directory / "final_output_memory.hex").write_bytes(output)
        return directory

    def test_accepts_pair_differing_only_in_trans_mode_and_output(self):
        baseline = self.fixture("atbd", 1, output=b"atbd")
        candidate = self.fixture("abtd", 2, output=b"abtd")
        self.assertEqual(validate_pair(baseline, candidate), [])

    def test_rejects_input_memory_difference(self):
        baseline = self.fixture("atbd", 1, initial=b"first", output=b"atbd")
        candidate = self.fixture(
            "abtd", 2, initial=b"second", output=b"abtd"
        )
        errors = validate_pair(baseline, candidate)
        self.assertIn("initial memory images differ", errors)

    def test_rejects_non_trans_csr_difference(self):
        baseline = self.fixture("atbd", 1, output=b"atbd")
        candidate = self.fixture("abtd", 2, output=b"abtd")
        manifest_path = candidate / "manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["csr_modes"]["cutbit"] = 1
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        errors = validate_pair(baseline, candidate)
        self.assertIn("CSR modes other than trans_mode differ", errors)

    def test_rejects_identical_rtl_outputs(self):
        baseline = self.fixture("atbd", 1, output=b"same")
        candidate = self.fixture("abtd", 2, output=b"same")
        errors = validate_pair(baseline, candidate)
        self.assertIn("RTL final-memory oracles are identical", errors)

    def test_rejects_unexpected_trans_modes(self):
        baseline = self.fixture("atbd", 0, output=b"atbd")
        candidate = self.fixture("abtd", 3, output=b"abtd")
        errors = validate_pair(baseline, candidate)
        self.assertTrue(any("expected 1" in error for error in errors))
        self.assertTrue(any("expected 2" in error for error in errors))


if __name__ == "__main__":
    unittest.main()
