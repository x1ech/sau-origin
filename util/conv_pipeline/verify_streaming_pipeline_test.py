#!/usr/bin/env python3
"""Tests for the independent streaming artifact verifier."""

from pathlib import Path
from dataclasses import replace
import unittest

from util.conv_pipeline.streaming_fixture import load_streaming_fixture
from util.conv_pipeline.verify_streaming_pipeline import (
    TRACE_FIELDS,
    _BBufferReplay,
    expected_d_pending_decision,
    expected_shared_grants,
    expected_tile_source_lanes,
)


REPO_ROOT = Path(__file__).resolve().parents[2]
FIXTURES = REPO_ROOT / "tests/gem5/streaming_conv_pipeline/fixtures"


class StreamingArtifactVerifierTest(unittest.TestCase):
    def sources(self, name):
        loaded = load_streaming_fixture(FIXTURES / name)
        return loaded, expected_tile_source_lanes(loaded.config)

    def test_shared_spad_trace_schema_is_frozen_at_87_fields(self):
        self.assertEqual(87, len(TRACE_FIELDS))
        self.assertEqual(len(TRACE_FIELDS), len(set(TRACE_FIELDS)))
        self.assertEqual("drained", TRACE_FIELDS[53])
        self.assertEqual("a_request_mask", TRACE_FIELDS[54])
        self.assertEqual("b_ready_entries", TRACE_FIELDS[-1])

    def test_shared_bank_arbiter_is_a_then_d_then_b(self):
        self.assertEqual(
            (0x0003, 0x0008, 0, 0x0004),
            expected_shared_grants(
                0x0003, 0x000f, 0, 0x0007))
        self.assertEqual(
            (0, 0, 0x0007, 0),
            expected_shared_grants(0, 0, 0x0007, 0))

    def test_depth_one_d_queue_turnover_and_partial_backpressure(self):
        self.assertEqual(
            (True, True, True),
            expected_d_pending_decision(
                1, 1, 0x0007, 0x0007, True))
        self.assertEqual(
            (False, False, False),
            expected_d_pending_decision(
                1, 1, 0x0007, 0x0003, True))
        self.assertEqual(
            (True, True, False),
            expected_d_pending_decision(
                1, 1, 0x0007, 0x0007, False))

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

    def test_small_b_buffers_force_refill_without_weight_reuse(self):
        loaded, tiles = self.sources(
            "n2_w6_stride2_depth2_no_reuse.json")
        self.assertEqual(2, loaded.derived.expected_tiles)
        self.assertEqual(loaded.derived.expected_tiles, len(tiles))
        self.assertEqual(2, loaded.config.shared_spad.b_buffer_depth)
        self.assertFalse(loaded.config.shared_spad.weight_reuse)
        self.assertGreater(loaded.derived.k, 4)

    def test_b_replay_refills_overwritten_chunk_after_switch(self):
        loaded, _ = self.sources(
            "n2_w6_stride2_depth2_no_reuse.json")
        replay = _BBufferReplay(loaded)
        self.assertEqual((0x0007, (0, 0, 0)),
                         replay.expected_request())
        replay.apply_response(0x0007, (0, 0, 0))
        replay.apply_response(0x0007, (0, 1, 1))
        replay.apply_response(0x0007, (1, 0, 2))
        replay.apply_response(0x0007, (1, 1, 3))
        replay.consume(0, 0)
        replay.consume(1, 0)
        self.assertEqual(1, replay.active)
        self.assertEqual(1, replay.switches)
        self.assertEqual((0x0007, (0, 0, 4)),
                         replay.expected_request())

    def test_b_replay_keeps_full_k_resident_across_tiles(self):
        loaded, _ = self.sources("n2_w6_stride2_full_reuse.json")
        replay = _BBufferReplay(loaded)
        for k_index in range(loaded.derived.k):
            replay.apply_response(0x0007, (0, k_index, k_index))
        self.assertTrue(replay.launch_ready())
        for tile in range(loaded.derived.expected_tiles):
            for k_index in range(loaded.derived.k):
                replay.consume(k_index, tile)
        self.assertEqual(loaded.derived.k, replay.fills)
        self.assertEqual(
            loaded.derived.im2col.expected_vectors,
            replay.consumed)
        self.assertEqual(loaded.derived.k, replay.reuse_hits)
        self.assertEqual(0, replay.switches)

    def test_b_replay_supports_half_k_split_full_residency(self):
        loaded, _ = self.sources("n2_w6_stride2_full_reuse.json")
        shared = replace(
            loaded.config.shared_spad,
            b_buffer_depth=loaded.derived.k // 2)
        loaded = replace(
            loaded,
            config=replace(loaded.config, shared_spad=shared))
        replay = _BBufferReplay(loaded)
        self.assertTrue(replay.full_resident)
        for buffer in range(2):
            for slot in range(replay.depth):
                global_k = buffer * replay.depth + slot
                replay.apply_response(
                    0x0007, (buffer, slot, global_k))
        self.assertTrue(replay.launch_ready())
        for k_index in range(loaded.derived.k):
            replay.consume(k_index, 0)
        self.assertEqual(1, replay.switches)
        self.assertEqual(0, replay.active)
        self.assertEqual(0, replay.next_k)
        for k_index in range(loaded.derived.k):
            replay.consume(k_index, 1)
        self.assertEqual(2, replay.switches)
        self.assertEqual(loaded.derived.k, replay.reuse_hits)

    def test_performance_profiles_share_one_workload(self):
        depth_one, _ = self.sources(
            "n2_w6_stride2_depth1_no_reuse.json")
        chunked, _ = self.sources(
            "n2_w6_stride2_depth2_no_reuse.json")
        resident, _ = self.sources(
            "n2_w6_stride2_full_reuse.json")

        for loaded in (chunked, resident):
            self.assertEqual(
                depth_one.derived.im2col.expected_vectors,
                loaded.derived.im2col.expected_vectors)
            self.assertEqual(
                depth_one.derived.expected_tiles,
                loaded.derived.expected_tiles)
            self.assertEqual(
                depth_one.derived.expected_outputs,
                loaded.derived.expected_outputs)
        self.assertEqual(
            1, depth_one.config.shared_spad.b_buffer_depth)
        self.assertFalse(
            depth_one.config.shared_spad.weight_reuse)
        self.assertEqual(
            2, chunked.config.shared_spad.b_buffer_depth)
        self.assertFalse(chunked.config.shared_spad.weight_reuse)
        self.assertEqual(
            resident.derived.k,
            resident.config.shared_spad.b_buffer_depth)
        self.assertTrue(resident.config.shared_spad.weight_reuse)

    def test_full_bank_ab_conflict_profile_is_depth_one(self):
        loaded, tiles = self.sources(
            "n2_w16_stride1_depth1_ab_conflict.json")
        self.assertEqual(16, loaded.config.out_channels)
        self.assertEqual(
            1, loaded.config.shared_spad.b_buffer_depth)
        self.assertFalse(loaded.config.shared_spad.weight_reuse)
        self.assertGreater(len(tiles), 1)


if __name__ == "__main__":
    unittest.main()
