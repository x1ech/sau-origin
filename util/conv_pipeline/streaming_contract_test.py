#!/usr/bin/env python3

from dataclasses import replace
import unittest

from util.conv_pipeline.pipeline_contract import (
    PipelineConfigError,
    ResolvedPipelineConfig,
)
from util.conv_pipeline.streaming_contract import (
    ElasticAdvanceInputs,
    RawSpatialPayload,
    SauInputProtocol,
    SpatialCoordinate,
    StreamingConsumerState,
    StreamingConservationCounts,
    b_address,
    c_address,
    StreamingVectorTag,
    compact_spatial_payload,
    decide_elastic_advance,
    decide_elastic_fifo,
    decide_sau_input_cycle,
    decide_streaming_consumer,
    d_address,
    is_canonical_prefix_mask,
    validate_same_tile_metadata,
    validate_drained_conservation,
    validate_streaming_config,
    validate_vector_tag,
    validate_vector_sequence,
)
from util.im2col.im2col_contract import ResolvedConfig


def streaming_config():
    return ResolvedPipelineConfig(
        schema_version=1,
        name="streaming_contract",
        im2col=ResolvedConfig(
            name="streaming_contract_im2col",
            n=1, c=2, h=4, w=6, out_h=2, out_w=3,
            kernel_h=3, kernel_w=3,
            stride_h=2, stride_w=2,
            dilation_h=1, dilation_w=1,
            pad_top=1, pad_left=1,
        ),
        out_channels=3,
        cutbit=8,
        weight_generator="tb_weight_value_v1",
        bias_generator="tb_bias_value_v1",
    )


def scattered_payload():
    activations = [0] * 16
    coordinates = [None] * 16
    lanes = (0, 1, 2, 6, 7, 8)
    for index, lane in enumerate(lanes):
        activations[lane] = 10 + lane
        coordinates[lane] = SpatialCoordinate(0, index // 3, index % 3)
    return RawSpatialPayload(
        tuple(activations), 0x01C7, tuple(coordinates))


def tag_for(tile, k_index, k=18):
    kernel_index = k_index % 9
    return StreamingVectorTag(
        tile_index=tile,
        oc_group=0,
        valid_columns=3,
        c=k_index // 9,
        kh=kernel_index // 3,
        kw=kernel_index % 3,
        k_index=k_index,
        tile_first=k_index == 0,
        tile_last=k_index + 1 == k,
    )


class StreamingConfigTest(unittest.TestCase):
    def test_accepts_w6_stride2_and_rejects_exploration_limits(self):
        derived = validate_streaming_config(streaming_config())
        self.assertEqual(derived.k, 18)
        self.assertEqual(derived.expected_tiles, 1)

        base = streaming_config()
        invalid_im2cols = (
            replace(base.im2col, stride_w=1),
            replace(base.im2col, stride_h=3, stride_w=3),
            replace(base.im2col, pad_left=0),
            replace(base.im2col, pad_top=2, pad_left=2),
            replace(base.im2col, dilation_w=2),
        )
        for im2col in invalid_im2cols:
            with self.subTest(im2col=im2col):
                with self.assertRaises(PipelineConfigError):
                    validate_streaming_config(replace(base, im2col=im2col))

    def test_freezes_shared_spad_address_layout(self):
        config = streaming_config()
        derived = validate_streaming_config(config)
        shared = derived.shared_spad
        self.assertEqual(shared.a_base, config.im2col.spad_base)
        self.assertEqual(
            b_address(config, 17, 2),
            type(b_address(config, 0, 0))(2, shared.b_base + 17))
        self.assertEqual(
            c_address(config, 2, 0),
            type(c_address(config, 0, 0))(2, shared.c_base))
        self.assertEqual(
            c_address(config, 2, 1),
            type(c_address(config, 0, 0))(2, shared.c_base + 1))
        self.assertEqual(
            d_address(config, 0, 1, 2, 2),
            type(d_address(config, 0, 0, 0, 0))(
                2, shared.d_base + 5))

        for call in (
                lambda: b_address(config, 18, 0),
                lambda: c_address(config, 0, 2),
                lambda: d_address(config, 1, 0, 0, 0)):
            with self.assertRaises(PipelineConfigError):
                call()


class StreamingCompactionTest(unittest.TestCase):
    def test_stably_compacts_scattered_raw_lanes(self):
        compacted = compact_spatial_payload(scattered_payload())
        self.assertEqual(compacted.valid_rows, 6)
        self.assertEqual(compacted.spatial_mask, 0x003F)
        self.assertTrue(is_canonical_prefix_mask(compacted.spatial_mask))
        self.assertEqual(compacted.source_lanes[:6], (0, 1, 2, 6, 7, 8))
        self.assertEqual(compacted.activations[:6], (10, 11, 12, 16, 17, 18))
        self.assertEqual(
            compacted.coordinates[:6],
            tuple(SpatialCoordinate(0, row // 3, row % 3)
                  for row in range(6)),
        )
        self.assertEqual(compacted.activations[6:], (0,) * 10)
        self.assertEqual(compacted.source_lanes[6:], (0,) * 10)
        self.assertEqual(compacted.coordinates[6:], (None,) * 10)

    def test_rejects_noncanonical_or_duplicate_raw_metadata(self):
        with self.assertRaises(PipelineConfigError):
            compact_spatial_payload(RawSpatialPayload(
                (0,) * 16, 0, (None,) * 16))

        raw = scattered_payload()
        activations = list(raw.activations)
        activations[3] = 1
        with self.assertRaises(PipelineConfigError):
            compact_spatial_payload(replace(
                raw, activations=tuple(activations)))

        coordinates = list(raw.coordinates)
        coordinates[6] = coordinates[0]
        with self.assertRaises(PipelineConfigError):
            compact_spatial_payload(replace(
                raw, coordinates=tuple(coordinates)))


class StreamingTagTest(unittest.TestCase):
    def test_enforces_canonical_k_and_tile_boundaries(self):
        validate_vector_tag(tag_for(7, 0), 18)
        validate_vector_tag(tag_for(7, 17), 18)
        for invalid in (
                replace(tag_for(7, 4), kw=2),
                replace(tag_for(7, 4), tile_first=True),
                replace(tag_for(7, 4), oc_group=1)):
            with self.subTest(tag=invalid):
                with self.assertRaises(PipelineConfigError):
                    validate_vector_tag(invalid, 18)

    def test_requires_stable_metadata_within_tile(self):
        payload = compact_spatial_payload(scattered_payload())
        validate_same_tile_metadata(
            tag_for(1, 0), payload, tag_for(1, 1), payload)
        changed_sources = list(payload.source_lanes)
        changed_sources[0] = 1
        with self.assertRaises(PipelineConfigError):
            validate_same_tile_metadata(
                tag_for(1, 0), payload,
                tag_for(1, 1),
                replace(payload, source_lanes=tuple(changed_sources)),
            )
        with self.assertRaises(PipelineConfigError):
            validate_same_tile_metadata(
                tag_for(1, 0), payload, tag_for(2, 1), payload)

    def test_enforces_contiguous_vector_and_tile_sequence(self):
        validate_vector_sequence(tag_for(1, 4), tag_for(1, 5), 18)
        validate_vector_sequence(tag_for(1, 17), tag_for(2, 0), 18)
        for previous, current in (
                (tag_for(1, 4), tag_for(1, 6)),
                (tag_for(1, 17), tag_for(3, 0)),
                (tag_for(1, 4), tag_for(2, 0))):
            with self.subTest(previous=previous, current=current):
                with self.assertRaises(PipelineConfigError):
                    validate_vector_sequence(previous, current, 18)


class StreamingElasticTest(unittest.TestCase):
    def test_all_four_transfers_can_fire_in_one_cycle(self):
        decision = decide_elastic_advance(ElasticAdvanceInputs(
            True, True, True, True, 3, True, True))
        self.assertTrue(all((
            decision.fifo_push_ready,
            decision.s2_ready,
            decision.s1_ready,
            decision.s0_ready,
            decision.producer_ready,
            decision.s2_to_fifo,
            decision.s1_to_s2,
            decision.s0_to_s1,
            decision.producer_to_s0,
        )))

    def test_conflict_and_backpressure_hold_upstream(self):
        conflict = decide_elastic_advance(ElasticAdvanceInputs(
            True, True, False, False, 0, False, True))
        self.assertFalse(conflict.s1_ready)
        self.assertFalse(conflict.s0_ready)
        self.assertFalse(conflict.producer_ready)

        full = decide_elastic_advance(ElasticAdvanceInputs(
            True, True, True, True, 4, False, True))
        self.assertFalse(full.fifo_push_ready)
        self.assertFalse(full.s2_ready)
        self.assertFalse(full.s1_ready)
        self.assertFalse(full.s0_ready)

    def test_fifo_full_pop_push_and_conservation(self):
        exchange = decide_elastic_fifo(4, True, True)
        self.assertTrue(exchange.push_ready)
        self.assertTrue(exchange.push)
        self.assertTrue(exchange.pop)
        self.assertEqual(exchange.next_count, 4)

        blocked = decide_elastic_fifo(4, True, False)
        self.assertFalse(blocked.push_ready)
        self.assertFalse(blocked.push)
        self.assertEqual(blocked.next_count, 4)
        with self.assertRaises(PipelineConfigError):
            decide_elastic_fifo(0, False, True)


class StreamingConsumerTest(unittest.TestCase):
    def test_launches_then_accepts_only_matching_tile_and_k(self):
        first = tag_for(3, 0)
        idle = decide_streaming_consumer(
            StreamingConsumerState.IDLE, True, first, 0, 0, 18)
        self.assertTrue(idle.begin_launch)
        self.assertFalse(idle.launch)
        self.assertFalse(idle.input_fire)
        launch = decide_streaming_consumer(
            StreamingConsumerState.LAUNCH, True, first, 3, 0, 18)
        self.assertTrue(launch.launch)

        accept = decide_streaming_consumer(
            StreamingConsumerState.ACCEPT_K,
            True, tag_for(3, 5), 3, 5, 18)
        self.assertTrue(accept.pe_ready)
        self.assertTrue(accept.input_valid)
        self.assertTrue(accept.input_fire)
        for bad_head in (tag_for(4, 5), tag_for(3, 6)):
            with self.subTest(head=bad_head):
                with self.assertRaises(PipelineConfigError):
                    decide_streaming_consumer(
                        StreamingConsumerState.ACCEPT_K,
                        True, bad_head, 3, 5, 18)

    def test_does_not_consume_next_tile_while_busy(self):
        for state in (StreamingConsumerState.WAIT_RESULT,
                      StreamingConsumerState.DRAIN_OUTPUT):
            decision = decide_streaming_consumer(
                state, True, tag_for(4, 0), 3, 18, 18)
            self.assertFalse(decision.pe_ready)
            self.assertFalse(decision.input_valid)
            self.assertFalse(decision.input_fire)


class StreamingSauInputTest(unittest.TestCase):
    def test_strict_and_elastic_bubble_protocols_are_isolated(self):
        with self.assertRaises(PipelineConfigError):
            decide_sau_input_cycle(
                SauInputProtocol.STRICT_RTL_CONTINUOUS,
                True, 5, 18, False, True)

        bubble = decide_sau_input_cycle(
            SauInputProtocol.ELASTIC_BUBBLE_ENABLED,
            True, 5, 18, False, True)
        self.assertFalse(bubble.schedule_new_mac)
        self.assertTrue(bubble.commit_previously_scheduled_mac)
        self.assertEqual(bubble.accepted_next, 5)

        fire = decide_sau_input_cycle(
            SauInputProtocol.ELASTIC_BUBBLE_ENABLED,
            True, 5, 18, True, False)
        self.assertTrue(fire.schedule_new_mac)
        self.assertEqual(fire.accepted_next, 6)


class StreamingConservationTest(unittest.TestCase):
    def test_requires_every_drained_boundary_to_match(self):
        complete = StreamingConservationCounts(
            expected_vectors=36,
            producer_accepted=36,
            s2_pushed=36,
            fifo_pushed=36,
            fifo_popped=36,
            pe_accepted=36,
            expected_tiles=2,
            tiles_generated=2,
            tiles_launched=2,
            tiles_completed=2,
        )
        validate_drained_conservation(complete)
        with self.assertRaises(PipelineConfigError):
            validate_drained_conservation(replace(complete, fifo_popped=35))
        with self.assertRaises(PipelineConfigError):
            validate_drained_conservation(replace(complete, tiles_completed=1))


if __name__ == "__main__":
    unittest.main()
