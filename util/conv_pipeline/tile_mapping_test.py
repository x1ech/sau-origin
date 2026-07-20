#!/usr/bin/env python3
"""Unit tests for independent SA spatial tile mapping."""

from dataclasses import replace
import json
from pathlib import Path
import unittest

from util.conv_pipeline.pipeline_contract_test import valid_config
from util.conv_pipeline.pipeline_fixture import load_fixture
from util.conv_pipeline.tile_mapping import (
    TileMappingError,
    nchw_output_index,
    output_coordinate,
    spatial_tiles,
)


REPO_ROOT = Path(__file__).resolve().parents[2]
MATRIX = REPO_ROOT / "tests/gem5/conv_pipeline/golden_matrix.json"


def mapping_config(w, h=1, n=1):
    config = valid_config(out_channels=3, cutbit=8)
    return replace(
        config,
        im2col=replace(
            config.im2col,
            n=n,
            c=1,
            h=h,
            w=w,
            out_h=h,
            out_w=w,
            pad_top=1,
            pad_left=1,
        ),
    )


class SpatialTileMappingTest(unittest.TestCase):
    def test_w_boundaries_have_expected_prefix_tail_masks(self):
        cases = (
            (mapping_config(1, h=17), 2, 0x0001),
            (mapping_config(5, h=4), 2, 0x001f),
            (mapping_config(16), 1, 0xffff),
            (mapping_config(17), 2, 0x0001),
            (mapping_config(20), 2, 0x000f),
        )
        for config, tile_count, final_mask in cases:
            with self.subTest(w=config.im2col.w):
                tiles = spatial_tiles(config)
                self.assertEqual(tile_count, len(tiles))
                self.assertEqual(final_mask, tiles[-1].valid_mask)
                self.assertEqual(
                    bin(final_mask).count("1"), tiles[-1].valid_rows)

    def test_batch_and_tail_cover_every_coordinate_once(self):
        config = mapping_config(17, h=3, n=2)
        tiles = spatial_tiles(config)
        positions = [
            position for tile in tiles for position in tile.lanes
            if position is not None
        ]

        self.assertEqual(12, len(tiles))
        self.assertEqual(2 * 3 * 17, len(positions))
        self.assertEqual(len(positions), len(set(positions)))
        self.assertEqual((0, 0, 0),
                         (positions[0].n, positions[0].oh, positions[0].ow))
        self.assertEqual((1, 2, 16),
                         (positions[-1].n, positions[-1].oh, positions[-1].ow))

    def test_output_coordinate_and_nchw_index_use_logical_row_order(self):
        config = mapping_config(5, h=4)
        tiles = spatial_tiles(config)

        self.assertEqual((0, 2, 1, 1),
                         output_coordinate(config, tiles[0], 6, 2))
        self.assertEqual(46, nchw_output_index(config, 0, 2, 1, 1))
        self.assertEqual((0, 0, 3, 0),
                         output_coordinate(config, tiles[1], 0, 0))
        with self.assertRaises(TileMappingError):
            output_coordinate(config, tiles[1], 5, 0)

    def test_rejects_non_prefix_spatial_shape(self):
        config = valid_config()
        config = replace(
            config,
            im2col=replace(
                config.im2col,
                c=1,
                h=4,
                w=5,
                out_h=2,
                out_w=3,
                pad_top=0,
                pad_left=0,
            ),
        )
        with self.assertRaisesRegex(TileMappingError, "not a canonical prefix"):
            spatial_tiles(config)

    def test_maps_every_frozen_golden_matrix_profile(self):
        matrix = json.loads(MATRIX.read_text(encoding="utf-8"))
        for profile in matrix["profiles"]:
            with self.subTest(profile=profile["name"]):
                loaded = load_fixture(REPO_ROOT / profile["fixture"])
                tiles = spatial_tiles(loaded.config)
                self.assertEqual(loaded.derived.expected_tiles, len(tiles))
                self.assertEqual(
                    loaded.config.im2col.n * loaded.config.im2col.out_h *
                    loaded.config.im2col.out_w,
                    sum(tile.valid_rows for tile in tiles),
                )


if __name__ == "__main__":
    unittest.main()
