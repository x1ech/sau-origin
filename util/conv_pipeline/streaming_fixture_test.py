#!/usr/bin/env python3

import unittest

from util.conv_pipeline.pipeline_fixture import FixtureError, resolve_fixture
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


if __name__ == "__main__":
    unittest.main()
