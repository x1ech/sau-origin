#!/usr/bin/env python3
"""Unit tests for the strict nested pipeline fixture loader."""

import json
from pathlib import Path
import tempfile
import unittest

from util.conv_pipeline.pipeline_fixture import (
    FixtureError,
    load_fixture,
    resolve_fixture,
)


def im2col_fixture(**changes):
    fixture = {
        "schema_version": 1,
        "name": "conv_n1_c2_h4_w5_oc3_im2col",
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


def pipeline_fixture(**changes):
    fixture = {
        "schema_version": 1,
        "name": "conv_n1_c2_h4_w5_oc3",
        "im2col": im2col_fixture(),
        "out_channels": 3,
        "cutbit": 8,
        "weight_generator": "tb_weight_value_v1",
        "bias_generator": "tb_bias_value_v1",
    }
    fixture.update(changes)
    return fixture


class PipelineFixtureTest(unittest.TestCase):
    def test_resolves_nested_fixture_and_counts(self):
        loaded = resolve_fixture(pipeline_fixture())

        self.assertEqual((4, 5),
                         (loaded.config.im2col.out_h, loaded.config.im2col.out_w))
        self.assertEqual(18, loaded.derived.k)
        self.assertEqual(2, loaded.derived.expected_tiles)
        self.assertEqual(60, loaded.derived.expected_outputs)
        self.assertEqual(1080, loaded.derived.expected_macs)
        self.assertEqual(64, len(loaded.resolved_config_sha256))

    def test_rejects_outer_unknown_missing_and_runtime_fields(self):
        missing = pipeline_fixture()
        del missing["cutbit"]
        with self.assertRaisesRegex(FixtureError, "missing required fields"):
            resolve_fixture(missing)
        with self.assertRaisesRegex(FixtureError, "unknown fields"):
            resolve_fixture(pipeline_fixture(extra=1))
        with self.assertRaisesRegex(FixtureError, "output_ready_period"):
            resolve_fixture(pipeline_fixture(output_ready_period=2))

    def test_reuses_strict_im2col_loader(self):
        with self.assertRaisesRegex(FixtureError, "im2col.*unknown fields"):
            resolve_fixture(pipeline_fixture(
                im2col=im2col_fixture(extra=1)))
        with self.assertRaisesRegex(FixtureError, "im2col.*kernel_h"):
            resolve_fixture(pipeline_fixture(
                im2col=im2col_fixture(kernel_h=True)))

    def test_rejects_non_prefix_sau_spatial_mask_before_startup(self):
        with self.assertRaisesRegex(FixtureError, "not a canonical prefix"):
            resolve_fixture(pipeline_fixture(im2col=im2col_fixture(
                h=4,
                w=5,
                pad_top=0,
                pad_left=0,
            )))

    def test_load_rejects_duplicate_outer_and_nested_fields(self):
        documents = (
            '{"schema_version":1,"schema_version":1}',
            '{"schema_version":1,"name":"x","im2col":'
            '{"schema_version":1,"schema_version":1}}',
        )
        for index, text in enumerate(documents):
            with self.subTest(index=index), tempfile.TemporaryDirectory() as tmp:
                path = Path(tmp) / "fixture.json"
                path.write_text(text, encoding="utf-8")
                with self.assertRaisesRegex(FixtureError, "duplicate JSON field"):
                    load_fixture(path)

    def test_loads_utf8_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "fixture.json"
            path.write_text(
                json.dumps(pipeline_fixture(name="卷积"), ensure_ascii=False),
                encoding="utf-8")
            loaded = load_fixture(path)
        self.assertEqual("卷积", loaded.config.name)


if __name__ == "__main__":
    unittest.main()
