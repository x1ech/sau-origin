#!/usr/bin/env python3
"""Unit tests for the independent Im2Col logical feed oracle."""

from dataclasses import replace
import unittest

from util.im2col.im2col_contract import ResolvedConfig, validate_and_derive
from util.im2col.logical_oracle import (
    expected_vector_count,
    iter_logical_feed_vectors,
    tb_act_value_v1,
)


def oracle_config(**changes):
    config = ResolvedConfig(
        name="oracle",
        n=1,
        c=1,
        h=1,
        w=1,
        out_h=1,
        out_w=1,
        kernel_h=1,
        kernel_w=1,
        stride_h=1,
        stride_w=1,
        dilation_h=1,
        dilation_w=1,
        pad_top=0,
        pad_left=0,
        spad_base=0,
    )
    return replace(config, **changes)


class LogicalOracleTest(unittest.TestCase):
    def test_activation_generator_uses_low_unsigned_byte(self):
        self.assertEqual(1, tb_act_value_v1(0, 0, 0, 0))
        self.assertEqual(
            (2 * 97 + 3 * 31 + 4 * 7 + 5 + 1) & 0xFF,
            tb_act_value_v1(2, 3, 4, 5),
        )

    def test_w_boundaries_have_expected_counts_and_tail_masks(self):
        cases = (
            (oracle_config(h=17, w=1, out_h=17, out_w=1), 2, 0x0001),
            (oracle_config(h=4, w=5, out_h=4, out_w=5), 2, 0x001F),
            (oracle_config(h=1, w=16, out_h=1, out_w=16), 1, 0xFFFF),
            (oracle_config(h=1, w=17, out_h=1, out_w=17), 2, 0x0001),
            (oracle_config(h=1, w=20, out_h=1, out_w=20), 2, 0x000F),
        )
        for config, count, final_mask in cases:
            with self.subTest(w=config.w):
                vectors = list(iter_logical_feed_vectors(config))
                self.assertEqual(count, len(vectors))
                self.assertEqual(count, expected_vector_count(config))
                self.assertEqual(count,
                                 validate_and_derive(config).expected_vectors)
                self.assertEqual(final_mask, vectors[-1].feed_mask)

    def test_padding_sets_mask_and_keeps_zero_data(self):
        config = oracle_config(
            h=1,
            w=5,
            out_h=1,
            out_w=5,
            kernel_h=3,
            kernel_w=3,
            pad_top=1,
            pad_left=1,
        )
        vectors = list(iter_logical_feed_vectors(config))

        self.assertEqual(9, len(vectors))
        self.assertEqual(0x001F, vectors[0].feed_mask)
        self.assertEqual((0,) * 16, vectors[0].lane_data)

        center = vectors[4]
        self.assertEqual((1, 2, 3, 4, 5), center.lane_data[:5])
        self.assertEqual((0,) * 11, center.lane_data[5:])
        self.assertEqual(0x0504030201, center.feed_data)

    def test_stride_and_dilation_map_directly_from_output_coordinates(self):
        config = oracle_config(
            h=5,
            w=5,
            out_h=2,
            out_w=2,
            kernel_h=2,
            kernel_w=2,
            stride_h=2,
            stride_w=2,
            dilation_h=2,
            dilation_w=2,
        )
        vectors = list(iter_logical_feed_vectors(config))

        self.assertEqual(4, len(vectors))
        self.assertEqual(0x0063, vectors[0].feed_mask)
        self.assertEqual((1, 3), vectors[0].lane_data[:2])
        self.assertEqual((15, 17), vectors[0].lane_data[5:7])
        self.assertEqual((17, 19), vectors[-1].lane_data[:2])
        self.assertEqual((31, 33), vectors[-1].lane_data[5:7])

    def test_sequence_advances_kw_kh_c_group_then_n(self):
        config = oracle_config(
            n=2,
            c=2,
            h=3,
            w=17,
            out_h=3,
            out_w=17,
            kernel_h=2,
            kernel_w=2,
            pad_top=1,
            pad_left=1,
        )
        vectors = list(iter_logical_feed_vectors(config))

        self.assertEqual(96, len(vectors))
        self.assertEqual((0, 0, 0, 0, 0, 0), (
            vectors[0].n, vectors[0].c, vectors[0].oh_base,
            vectors[0].ow_base, vectors[0].kh, vectors[0].kw))
        self.assertEqual((0, 0, 0, 0, 0, 1), (
            vectors[1].n, vectors[1].c, vectors[1].oh_base,
            vectors[1].ow_base, vectors[1].kh, vectors[1].kw))
        self.assertEqual((0, 1, 0, 0, 0, 0), (
            vectors[4].n, vectors[4].c, vectors[4].oh_base,
            vectors[4].ow_base, vectors[4].kh, vectors[4].kw))
        self.assertEqual((0, 0, 0, 16, 0, 0), (
            vectors[8].n, vectors[8].c, vectors[8].oh_base,
            vectors[8].ow_base, vectors[8].kh, vectors[8].kw))
        self.assertEqual((0, 0, 1, 0, 0, 0), (
            vectors[16].n, vectors[16].c, vectors[16].oh_base,
            vectors[16].ow_base, vectors[16].kh, vectors[16].kw))
        self.assertEqual((1, 0, 0, 0, 0, 0), (
            vectors[48].n, vectors[48].c, vectors[48].oh_base,
            vectors[48].ow_base, vectors[48].kh, vectors[48].kw))


if __name__ == "__main__":
    unittest.main()
