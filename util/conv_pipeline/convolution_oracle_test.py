#!/usr/bin/env python3
"""Unit tests for the independent direct-NCHW convolution oracle."""

from dataclasses import replace
import json
from pathlib import Path
import unittest

from util.conv_pipeline.convolution_oracle import (
    ACC24_MAX,
    ACC24_MIN,
    OracleError,
    generate_convolution,
    quantize_accumulator,
    saturating_add_signed_24,
)
from util.conv_pipeline.pipeline_contract_test import valid_config
from util.conv_pipeline.pipeline_fixture import load_fixture


REPO_ROOT = Path(__file__).resolve().parents[2]
MATRIX = REPO_ROOT / "tests/gem5/conv_pipeline/golden_matrix.json"


class ConvolutionOracleTest(unittest.TestCase):
    def test_hand_computed_ones_profile(self):
        config = valid_config(
            out_channels=1,
            cutbit=0,
            weight_generator="ones",
            bias_generator="zero",
        )
        config = replace(
            config,
            im2col=replace(
                config.im2col,
                c=1,
                h=3,
                w=1,
                out_h=3,
                out_w=1,
                pad_top=1,
                pad_left=1,
            ),
        )
        result = generate_convolution(config)

        self.assertEqual((9, 24, 23), result.values)
        self.assertEqual(27, result.mac_count)
        self.assertEqual((0, 0, 0, 0), (
            result.outputs[0].n, result.outputs[0].output_channel,
            result.outputs[0].oh, result.outputs[0].ow))

    def test_stride_and_dilation_have_hand_computed_outputs(self):
        base = valid_config(
            out_channels=1,
            cutbit=0,
            weight_generator="ones",
            bias_generator="zero",
        )
        stride = replace(
            base,
            im2col=replace(
                base.im2col,
                c=1,
                h=5,
                w=5,
                out_h=2,
                out_w=2,
                stride_h=2,
                stride_w=2,
                pad_top=0,
                pad_left=0,
            ),
        )
        dilation = replace(
            stride,
            im2col=replace(
                stride.im2col,
                out_h=1,
                out_w=1,
                stride_h=1,
                stride_w=1,
                dilation_h=2,
                dilation_w=2,
            ),
        )

        stride_result = generate_convolution(stride)
        dilation_result = generate_convolution(dilation)
        self.assertEqual((81, 99, 207, 225), tuple(
            output.accumulator for output in stride_result.outputs))
        self.assertEqual((81, 99, 127, 127), stride_result.values)
        self.assertEqual((153,), tuple(
            output.accumulator for output in dilation_result.outputs))
        self.assertEqual((127,), dilation_result.values)

    def test_batch_bias_and_arithmetic_cutbit(self):
        config = valid_config(
            out_channels=1,
            cutbit=1,
            weight_generator="zero",
            bias_generator="tb_bias_value_v1",
        )
        config = replace(
            config,
            im2col=replace(
                config.im2col,
                n=2,
                c=1,
                h=1,
                w=1,
                out_h=1,
                out_w=1,
                pad_top=1,
                pad_left=1,
            ),
        )

        result = generate_convolution(config)
        self.assertEqual((-58, -58), result.values)
        self.assertEqual((-115, -115),
                         tuple(output.accumulator for output in result.outputs))

    def test_positive_saturation_first_occurs_at_mac_512(self):
        accumulator = 0
        for mac in range(1, 568):
            accumulator = saturating_add_signed_24(
                accumulator, (-128) * (-128))
            if mac == 511:
                self.assertEqual(8372224, accumulator)
            if mac >= 512:
                self.assertEqual(ACC24_MAX, accumulator)

        accumulator = saturating_add_signed_24(ACC24_MAX - 5, 10)
        self.assertEqual(ACC24_MAX, accumulator)
        self.assertEqual(
            ACC24_MAX - 10,
            saturating_add_signed_24(accumulator, -10),
        )

    def test_negative_saturation_first_occurs_at_mac_517(self):
        accumulator = 0
        for mac in range(1, 568):
            accumulator = saturating_add_signed_24(
                accumulator, (-128) * 127)
            if mac == 516:
                self.assertEqual(-8388096, accumulator)
            if mac >= 517:
                self.assertEqual(ACC24_MIN, accumulator)

    def test_bias_cutbit_and_signed_int8_saturation(self):
        self.assertEqual(-115, saturating_add_signed_24(0, -115))
        self.assertEqual(-128, quantize_accumulator(-257, 1))
        self.assertEqual(-128, quantize_accumulator(-255, 1))
        self.assertEqual(127, quantize_accumulator(256, 1))
        self.assertEqual(0xffff, (-1) & 0xffff)
        with self.assertRaises(OracleError):
            saturating_add_signed_24(ACC24_MAX + 1, 0)
        with self.assertRaises(OracleError):
            quantize_accumulator(0, 24)

    def test_generates_every_frozen_golden_matrix_profile(self):
        matrix = json.loads(MATRIX.read_text(encoding="utf-8"))
        self.assertEqual(1, matrix["schema_version"])
        self.assertEqual(7, len(matrix["profiles"]))
        for profile in matrix["profiles"]:
            with self.subTest(profile=profile["name"]):
                loaded = load_fixture(REPO_ROOT / profile["fixture"])
                result = generate_convolution(loaded.config)
                self.assertEqual(
                    loaded.derived.expected_outputs, len(result.outputs))
                self.assertEqual(
                    loaded.derived.expected_macs, result.mac_count)
                self.assertEqual(
                    list(range(len(result.outputs))),
                    [
                        (((output.n * loaded.config.out_channels +
                           output.output_channel) *
                          loaded.config.im2col.out_h + output.oh) *
                         loaded.config.im2col.out_w + output.ow)
                        for output in result.outputs
                    ],
                )


if __name__ == "__main__":
    unittest.main()
