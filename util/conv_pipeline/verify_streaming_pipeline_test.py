#!/usr/bin/env python3
"""Tests for the independent streaming artifact verifier."""

from pathlib import Path
import unittest

from util.conv_pipeline.streaming_fixture import load_streaming_fixture
from util.conv_pipeline.verify_streaming_pipeline import (
    TRACE_FIELDS,
    expected_tile_source_lanes,
)


REPO_ROOT = Path(__file__).resolve().parents[2]
FIXTURES = REPO_ROOT / "tests/gem5/streaming_conv_pipeline/fixtures"


class StreamingArtifactVerifierTest(unittest.TestCase):
    def sources(self, name):
        loaded = load_streaming_fixture(FIXTURES / name)
        return loaded, expected_tile_source_lanes(loaded.config)

    def test_compact_trace_schema_is_frozen_at_54_fields(self):
        self.assertEqual(54, len(TRACE_FIELDS))
        self.assertEqual(len(TRACE_FIELDS), len(set(TRACE_FIELDS)))

    def test_w6_stride2_mapping_is_derived_without_compaction_helper(self):
        loaded, tiles = self.sources("w6_stride2_scattered.json")
        self.assertEqual(1, loaded.derived.expected_tiles)
        self.assertEqual((0, 1, 2, 6, 7, 8), tiles[0][0])
        self.assertEqual(
            ((0, 0, 0), (0, 0, 1), (0, 0, 2),
             (0, 1, 0), (0, 1, 1), (0, 1, 2)),
            tiles[0][1],
        )

    def test_w5_pad0_and_w17_tail_have_independent_source_maps(self):
        w5, w5_tiles = self.sources("w5_stride1_pad0.json")
        self.assertEqual(w5.derived.expected_tiles, len(w5_tiles))
        self.assertEqual((0, 1, 2, 5, 6, 7, 10, 11, 12), w5_tiles[0][0])

        w17, w17_tiles = self.sources("w17_stride1_tail_oc7.json")
        self.assertEqual(w17.derived.expected_tiles, len(w17_tiles))
        self.assertEqual(tuple(range(16)), w17_tiles[0][0])
        self.assertEqual((0,), w17_tiles[1][0])


if __name__ == "__main__":
    unittest.main()
