#!/usr/bin/env python3
"""Unit tests for the frozen convolution pipeline Step 1 contract."""

from dataclasses import replace
import json
import unittest

from util.conv_pipeline.pipeline_contract import (
    CYCLE_ANCHORS_PROVISIONAL,
    DrainStatus,
    OutputReadyConfig,
    PipelineConfigError,
    PipelineState,
    ResolvedPipelineConfig,
    canonical_config_bytes,
    output_ready,
    pe_index,
    pipeline_drained,
    resolved_config_sha256,
    validate_and_derive,
)
from util.im2col.im2col_contract import ResolvedConfig as Im2ColConfig


def valid_config(**changes):
    config = ResolvedPipelineConfig(
        schema_version=1,
        name="conv_n1_c2_h4_w5_oc3",
        im2col=Im2ColConfig(
            name="conv_n1_c2_h4_w5_oc3_im2col",
            n=1,
            c=2,
            h=4,
            w=5,
            out_h=4,
            out_w=5,
            kernel_h=3,
            kernel_w=3,
            stride_h=1,
            stride_w=1,
            dilation_h=1,
            dilation_w=1,
            pad_top=1,
            pad_left=1,
            spad_base=0,
        ),
        out_channels=3,
        cutbit=8,
        weight_generator="tb_weight_value_v1",
        bias_generator="tb_bias_value_v1",
    )
    return replace(config, **changes)


class PipelineDerivationTest(unittest.TestCase):
    def test_derives_shared_python_cpp_anchor(self):
        derived = validate_and_derive(valid_config())

        self.assertEqual(18, derived.k)
        self.assertEqual(2, derived.expected_tiles)
        self.assertEqual(60, derived.expected_outputs)
        self.assertEqual(1080, derived.expected_macs)
        self.assertEqual(36, derived.im2col.expected_vectors)

    def test_rejects_pipeline_ranges_and_generators(self):
        invalid = (
            ("schema_version", 2),
            ("out_channels", 0),
            ("out_channels", 17),
            ("cutbit", -1),
            ("cutbit", 24),
            ("weight_generator", "random"),
            ("bias_generator", "ones"),
        )
        for field, value in invalid:
            with self.subTest(field=field, value=value), self.assertRaises(
                    PipelineConfigError):
                validate_and_derive(valid_config(**{field: value}))

    def test_rejects_non_3x3_and_channels_above_63(self):
        config = valid_config()
        with self.assertRaisesRegex(PipelineConfigError, "must both be 3"):
            validate_and_derive(replace(
                config, im2col=replace(config.im2col, kernel_h=2)))
        with self.assertRaisesRegex(PipelineConfigError, "im2col.c"):
            validate_and_derive(replace(
                config, im2col=replace(config.im2col, c=64)))

    def test_canonical_nested_json_and_hash_are_deterministic(self):
        config = valid_config(name="卷积")
        canonical = canonical_config_bytes(config)
        decoded = json.loads(canonical)

        self.assertEqual("卷积", decoded["name"])
        self.assertEqual(2, decoded["im2col"]["c"])
        self.assertNotIn(b" ", canonical)
        self.assertEqual(64, len(resolved_config_sha256(config)))
        self.assertEqual(
            resolved_config_sha256(config), resolved_config_sha256(config))


class PipelineCycleContractTest(unittest.TestCase):
    def test_freezes_state_encoding_and_validated_marker(self):
        self.assertEqual(list(range(7)), [state.value for state in PipelineState])
        self.assertFalse(CYCLE_ANCHORS_PROVISIONAL)

    def test_freezes_periodic_ready(self):
        ready = OutputReadyConfig(period=5, high_cycles=2)
        self.assertEqual(
            [True, True, False, False, False, True],
            [output_ready(cycle, ready) for cycle in range(6)],
        )
        for invalid in (
                OutputReadyConfig(0, 1), OutputReadyConfig(2, 0),
                OutputReadyConfig(2, 3)):
            with self.assertRaises(PipelineConfigError):
                output_ready(0, invalid)

    def test_freezes_logical_pe_packed_order(self):
        self.assertEqual(0, pe_index(0, 0))
        self.assertEqual(1, pe_index(0, 1))
        self.assertEqual(31, pe_index(1, 15))
        self.assertEqual(255, pe_index(15, 15))
        with self.assertRaises(PipelineConfigError):
            pe_index(16, 0)

    def test_drained_requires_every_frozen_condition(self):
        derived = validate_and_derive(valid_config())
        status = DrainStatus(
            im2col_can_feed=False,
            im2col_fifo_empty=True,
            tile_buffer_empty=True,
            sau_idle=True,
            completed_tiles=derived.expected_tiles,
            written_outputs=derived.expected_outputs,
            output_pending=False,
        )
        self.assertTrue(pipeline_drained(status, derived))
        for field, value in (
                ("im2col_can_feed", True), ("im2col_fifo_empty", False),
                ("tile_buffer_empty", False), ("sau_idle", False),
                ("completed_tiles", derived.expected_tiles - 1),
                ("written_outputs", derived.expected_outputs - 1),
                ("output_pending", True)):
            with self.subTest(field=field):
                self.assertFalse(
                    pipeline_drained(replace(status, **{field: value}), derived))


if __name__ == "__main__":
    unittest.main()
