#!/usr/bin/env python3
"""Unit tests for the Im2Col JSON fixture loader."""

from dataclasses import asdict
import json
from pathlib import Path
import tempfile
import unittest

from util.im2col.im2col_fixture import (
    FixtureError,
    load_fixture,
    resolve_fixture,
)


def base_fixture(**changes):
    fixture = {
        "schema_version": 1,
        "name": "w5_pack3_pad1_stride1",
        "n": 1,
        "c": 2,
        "h": 4,
        "w": 5,
        "kernel_h": 3,
        "kernel_w": 3,
        "stride_h": 1,
        "stride_w": 1,
        "dilation_h": 1,
        "dilation_w": 1,
        "pad_top": 1,
        "pad_left": 1,
        "spad_base": 0,
        "input_generator": "tb_act_value_v1",
    }
    fixture.update(changes)
    return fixture


class FixtureResolutionTest(unittest.TestCase):
    def test_computes_omitted_output_pair(self):
        loaded = resolve_fixture(base_fixture())

        self.assertEqual((4, 5), (loaded.config.out_h, loaded.config.out_w))
        self.assertEqual(36, loaded.derived.expected_vectors)
        self.assertEqual((), loaded.warnings)
        self.assertEqual(64, len(loaded.resolved_config_sha256))

    def test_preserves_matching_explicit_output_pair(self):
        loaded = resolve_fixture(base_fixture(out_h=4, out_w=5))

        self.assertEqual((4, 5), (loaded.config.out_h, loaded.config.out_w))
        self.assertEqual((), loaded.warnings)

    def test_auto_output_accounts_for_stride_and_dilation(self):
        loaded = resolve_fixture(base_fixture(
            c=1,
            h=5,
            w=5,
            kernel_h=2,
            kernel_w=2,
            stride_h=2,
            stride_w=2,
            dilation_h=2,
            dilation_w=2,
            pad_top=0,
            pad_left=0,
        ))

        self.assertEqual((2, 2), (loaded.config.out_h, loaded.config.out_w))
        self.assertEqual(4, loaded.derived.expected_vectors)

    def test_allows_mismatched_explicit_output_with_warning(self):
        loaded = resolve_fixture(base_fixture(out_h=2, out_w=3))

        self.assertEqual((2, 3), (loaded.config.out_h, loaded.config.out_w))
        self.assertEqual(1, len(loaded.warnings))
        self.assertIn("differs from automatic output 4x5", loaded.warnings[0])

    def test_explicit_output_does_not_require_nonnegative_auto_formula(self):
        loaded = resolve_fixture(base_fixture(
            h=1,
            w=5,
            kernel_h=3,
            kernel_w=3,
            pad_top=0,
            pad_left=0,
            out_h=1,
            out_w=1,
        ))

        self.assertEqual((1, 1), (loaded.config.out_h, loaded.config.out_w))
        self.assertEqual((), loaded.warnings)

    def test_rejects_automatic_output_with_negative_numerator(self):
        with self.assertRaisesRegex(FixtureError, "negative numerator"):
            resolve_fixture(base_fixture(
                h=1,
                kernel_h=3,
                pad_top=0,
            ))

    def test_rejects_only_one_explicit_output_dimension(self):
        for field in ("out_h", "out_w"):
            with self.subTest(field=field), self.assertRaisesRegex(
                    FixtureError, "must be provided together"):
                resolve_fixture(base_fixture(**{field: 4}))

    def test_rejects_missing_unknown_and_runtime_fields(self):
        missing = base_fixture()
        del missing["stride_h"]
        with self.assertRaisesRegex(FixtureError, "missing required fields"):
            resolve_fixture(missing)

        with self.assertRaisesRegex(FixtureError, "unknown fields"):
            resolve_fixture(base_fixture(extra=1))
        with self.assertRaisesRegex(FixtureError, "ready_period"):
            resolve_fixture(base_fixture(ready_period=2))
        with self.assertRaisesRegex(FixtureError, "field names"):
            resolve_fixture({1: "not a JSON object field"})

    def test_rejects_empty_name_and_fixed_mode_violations(self):
        with self.assertRaisesRegex(FixtureError, "non-empty string"):
            resolve_fixture(base_fixture(name=""))
        with self.assertRaisesRegex(FixtureError, "cfg_dw_mode"):
            resolve_fixture(base_fixture(cfg_dw_mode=1))
        with self.assertRaisesRegex(FixtureError, "cfg_kernel_pattern"):
            resolve_fixture(base_fixture(cfg_kernel_pattern=0xFFFE))

    def test_resolved_config_contains_fixed_default_mode_fields(self):
        document = asdict(resolve_fixture(base_fixture()).config)

        self.assertEqual(0, document["cfg_dw_mode"])
        self.assertEqual(0xFFFF, document["cfg_kernel_pattern"])
        self.assertEqual(4, document["out_h"])
        self.assertEqual(5, document["out_w"])


class FixtureFileTest(unittest.TestCase):
    def test_loads_utf8_json_file(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fixture.json"
            document = base_fixture(name="宽度5")
            path.write_text(
                json.dumps(document, ensure_ascii=False), encoding="utf-8")

            loaded = load_fixture(path)

        self.assertEqual("宽度5", loaded.config.name)

    def test_rejects_duplicate_json_field(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fixture.json"
            path.write_text('{"schema_version":1,"schema_version":1}',
                            encoding="utf-8")

            with self.assertRaisesRegex(FixtureError, "duplicate JSON field"):
                load_fixture(path)

    def test_rejects_non_object_and_malformed_json(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fixture.json"
            path.write_text("[]", encoding="utf-8")
            with self.assertRaisesRegex(FixtureError, "JSON object"):
                load_fixture(path)

            path.write_text("{", encoding="utf-8")
            with self.assertRaisesRegex(FixtureError, "invalid JSON"):
                load_fixture(path)


if __name__ == "__main__":
    unittest.main()
