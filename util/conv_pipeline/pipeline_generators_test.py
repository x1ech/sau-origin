#!/usr/bin/env python3
"""Unit tests for deterministic convolution pipeline data generators."""

import unittest

from util.conv_pipeline.pipeline_contract import PipelineConfigError
from util.conv_pipeline.pipeline_generators import (
    activation_raw_v1,
    activation_value_v1,
    bias_value,
    signed_int8,
    weight_value,
)


class PipelineGeneratorsTest(unittest.TestCase):
    def test_signed_activation_preserves_raw_bit_pattern(self):
        self.assertEqual(0, signed_int8(0))
        self.assertEqual(127, signed_int8(127))
        self.assertEqual(-128, signed_int8(128))
        self.assertEqual(-1, signed_int8(255))
        self.assertEqual(1, activation_raw_v1(0, 0, 0, 0))
        self.assertEqual(137, activation_raw_v1(1, 1, 1, 1))
        self.assertEqual(-119, activation_value_v1(1, 1, 1, 1))

    def test_weight_generators_match_frozen_values(self):
        self.assertEqual(-116, weight_value("tb_weight_value_v1", 0, 0, 0, 0))
        self.assertEqual(114, weight_value("tb_weight_value_v1", 15, 2, 2, 2))
        self.assertEqual(0, weight_value("zero", 9, 8, 2, 1))
        self.assertEqual(1, weight_value("ones", 9, 8, 2, 1))

    def test_bias_generators_match_frozen_values(self):
        self.assertEqual(-115, bias_value("tb_bias_value_v1", 0))
        self.assertEqual(-74, bias_value("tb_bias_value_v1", 15))
        self.assertEqual(0, bias_value("zero", 15))

    def test_rejects_invalid_values_and_generators(self):
        for raw in (-1, 256, True):
            with self.subTest(raw=raw), self.assertRaises(PipelineConfigError):
                signed_int8(raw)
        with self.assertRaises(PipelineConfigError):
            activation_raw_v1(-1, 0, 0, 0)
        with self.assertRaises(PipelineConfigError):
            weight_value("random", 0, 0, 0, 0)
        with self.assertRaises(PipelineConfigError):
            bias_value("ones", 0)


if __name__ == "__main__":
    unittest.main()
