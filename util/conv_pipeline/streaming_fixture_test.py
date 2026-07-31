#!/usr/bin/env python3

import unittest

from util.conv_pipeline.pipeline_fixture import (
    FixtureError,
    resolve_fixture,
    resolve_fixture_fields,
)
from util.conv_pipeline.streaming_fixture import (
    StreamingFixtureError,
    resolve_streaming_fixture,
)


def w6_stride2_fixture():
    return {
        "schema_version": 1,
        "name": "streaming_w6_stride2",
        "im2col": {
            "schema_version": 1,
            "name": "streaming_w6_stride2_im2col",
            "n": 1,
            "c": 2,
            "h": 4,
            "w": 6,
            "kernel_h": 3,
            "kernel_w": 3,
            "stride_h": 2,
            "stride_w": 2,
            "dilation_h": 1,
            "dilation_w": 1,
            "pad_top": 1,
            "pad_left": 1,
            "spad_base": 0,
            "input_generator": "tb_act_value_v1",
        },
        "out_channels": 3,
        "cutbit": 8,
        "weight_generator": "tb_weight_value_v1",
        "bias_generator": "tb_bias_value_v1",
    }


class StreamingFixtureTest(unittest.TestCase):
    def test_accepts_scattered_raw_mapping_without_loosening_old_loader(self):
        fixture = w6_stride2_fixture()
        loaded = resolve_streaming_fixture(fixture)
        self.assertEqual(1, loaded.derived.expected_tiles)
        self.assertEqual((2, 3), (
            loaded.config.im2col.out_h, loaded.config.im2col.out_w))
        with self.assertRaisesRegex(FixtureError, "not a canonical prefix"):
            resolve_fixture(fixture)

    def test_rejects_streaming_only_range_violations(self):
        fixture = w6_stride2_fixture()
        fixture["im2col"]["stride_h"] = 3
        fixture["im2col"]["stride_w"] = 3
        with self.assertRaisesRegex(StreamingFixtureError, "stride"):
            resolve_streaming_fixture(fixture)

        fixture = w6_stride2_fixture()
        fixture["im2col"]["dilation_h"] = 2
        fixture["im2col"]["dilation_w"] = 2
        with self.assertRaisesRegex(StreamingFixtureError, "dilation"):
            resolve_streaming_fixture(fixture)

    def test_resolves_default_shared_spad_without_changing_common_hash(self):
        fixture = w6_stride2_fixture()
        common = resolve_fixture_fields(fixture)
        loaded = resolve_streaming_fixture(fixture)
        shared = loaded.config.shared_spad

        self.assertEqual(common.resolved_config_sha256,
                         resolve_fixture_fields(fixture).resolved_config_sha256)
        self.assertNotEqual(
            common.resolved_config_sha256, loaded.resolved_config_sha256)
        self.assertEqual(shared.a_base, fixture["im2col"]["spad_base"])
        self.assertEqual(shared.b_base, shared.a_base + shared.a_rows)
        self.assertEqual(shared.c_base, shared.b_base + shared.b_rows)
        self.assertEqual(shared.d_base, shared.c_base + shared.c_rows)
        self.assertEqual(shared.b_buffer_depth, loaded.derived.k)
        self.assertEqual(shared.d_pending_rows, 1)
        self.assertTrue(shared.weight_reuse)
        self.assertEqual(shared.arbitration, "a_d_b")

    def test_accepts_explicit_layout_and_rejects_overlap_and_unknowns(self):
        fixture = w6_stride2_fixture()
        fixture["shared_spad"] = {
            "a_base": 0,
            "a_rows": 8,
            "b_base": 32,
            "b_rows": 18,
            "c_base": 64,
            "c_rows": 2,
            "d_base": 80,
            "d_rows": 6,
            "b_buffer_depth": 4,
            "d_pending_rows": 2,
            "weight_reuse": False,
            "arbitration": "a_d_b",
        }
        loaded = resolve_streaming_fixture(fixture)
        self.assertEqual(32, loaded.config.shared_spad.b_base)
        self.assertEqual(4, loaded.config.shared_spad.b_buffer_depth)
        self.assertFalse(loaded.config.shared_spad.weight_reuse)

        fixture["shared_spad"]["b_base"] = 4
        with self.assertRaisesRegex(StreamingFixtureError, "overlap"):
            resolve_streaming_fixture(fixture)

        fixture = w6_stride2_fixture()
        fixture["shared_spad"] = {"extra_port": True}
        with self.assertRaisesRegex(StreamingFixtureError, "unknown fields"):
            resolve_streaming_fixture(fixture)

        fixture = w6_stride2_fixture()
        fixture["shared_spad"] = None
        with self.assertRaisesRegex(StreamingFixtureError, "JSON object"):
            resolve_streaming_fixture(fixture)

    def test_rejects_small_or_out_of_bounds_regions(self):
        fixture = w6_stride2_fixture()
        fixture["shared_spad"] = {"b_rows": 17}
        with self.assertRaisesRegex(StreamingFixtureError, "b_rows"):
            resolve_streaming_fixture(fixture)

        fixture = w6_stride2_fixture()
        fixture["shared_spad"] = {"d_base": 4095}
        with self.assertRaisesRegex(StreamingFixtureError, "exceeds"):
            resolve_streaming_fixture(fixture)


if __name__ == "__main__":
    unittest.main()
