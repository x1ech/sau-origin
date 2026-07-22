#!/usr/bin/env python3
"""Independent Step 1 contracts for the streaming Im2Col-to-SAU path."""

from dataclasses import dataclass
from enum import IntEnum

from util.conv_pipeline.pipeline_contract import (
    MAX_CHANNELS,
    ResolvedPipelineConfig,
    SA_COLUMNS,
    SA_ROWS,
    PipelineConfigError,
    validate_and_derive,
)


STREAMING_FIFO_DEPTH = 4


class StreamingConsumerState(IntEnum):
    IDLE = 0
    LAUNCH = 1
    ACCEPT_K = 2
    WAIT_RESULT = 3
    DRAIN_OUTPUT = 4


class SauInputProtocol(IntEnum):
    STRICT_RTL_CONTINUOUS = 0
    ELASTIC_BUBBLE_ENABLED = 1


@dataclass(frozen=True)
class SpatialCoordinate:
    n: int
    oh: int
    ow: int


@dataclass(frozen=True)
class RawSpatialPayload:
    activations: tuple
    spatial_mask: int
    coordinates: tuple


@dataclass(frozen=True)
class CompactedSpatialPayload:
    activations: tuple
    spatial_mask: int
    valid_rows: int
    source_lanes: tuple
    coordinates: tuple


@dataclass(frozen=True)
class StreamingVectorTag:
    tile_index: int
    oc_group: int
    valid_columns: int
    c: int
    kh: int
    kw: int
    k_index: int
    tile_first: bool
    tile_last: bool


@dataclass(frozen=True)
class ElasticAdvanceInputs:
    s0_valid: bool
    s1_valid: bool
    s2_valid: bool
    s1_can_retire: bool
    fifo_count: int
    fifo_pop: bool
    more_vectors: bool


@dataclass(frozen=True)
class ElasticAdvanceDecision:
    fifo_push_ready: bool
    s2_ready: bool
    s1_ready: bool
    s0_ready: bool
    producer_ready: bool
    s2_to_fifo: bool
    s1_to_s2: bool
    s0_to_s1: bool
    producer_to_s0: bool


@dataclass(frozen=True)
class ElasticFifoDecision:
    push_ready: bool
    push: bool
    pop: bool
    next_count: int


@dataclass(frozen=True)
class StreamingConsumerDecision:
    begin_launch: bool
    launch: bool
    pe_ready: bool
    input_valid: bool
    input_fire: bool


@dataclass(frozen=True)
class SauInputCycleDecision:
    schedule_new_mac: bool
    commit_previously_scheduled_mac: bool
    accepted_next: int


@dataclass(frozen=True)
class StreamingConservationCounts:
    expected_vectors: int
    producer_accepted: int
    s2_pushed: int
    fifo_pushed: int
    fifo_popped: int
    pe_accepted: int
    expected_tiles: int
    tiles_generated: int
    tiles_launched: int
    tiles_completed: int


def _require_bool(value, field):
    if type(value) is not bool:
        raise PipelineConfigError(f"{field} must be a bool")


def _require_int_range(value, minimum, maximum, field):
    if type(value) is not int or value < minimum or value > maximum:
        raise PipelineConfigError(
            f"{field} must be an integer in [{minimum}, {maximum}]")


def validate_streaming_config(config):
    """Apply the exploration-only limits after the frozen base validation."""
    if not isinstance(config, ResolvedPipelineConfig):
        raise PipelineConfigError(
            "config must be a ResolvedPipelineConfig")
    derived = validate_and_derive(config)
    if (config.im2col.stride_h != config.im2col.stride_w or
            config.im2col.stride_h not in (1, 2)):
        raise PipelineConfigError(
            "streaming stride_h/stride_w must be equal and in {1, 2}")
    if (config.im2col.pad_top != config.im2col.pad_left or
            config.im2col.pad_top not in (0, 1)):
        raise PipelineConfigError(
            "streaming pad_top/pad_left must be equal and in {0, 1}")
    if (config.im2col.dilation_h != 1 or
            config.im2col.dilation_w != 1):
        raise PipelineConfigError(
            "streaming dilation_h/dilation_w must both be 1")
    return derived


def _prefix_mask(count):
    _require_int_range(count, 0, SA_ROWS, "prefix mask count")
    return (1 << count) - 1


def is_canonical_prefix_mask(mask):
    _require_int_range(mask, 0, (1 << SA_ROWS) - 1, "spatial mask")
    return mask == _prefix_mask(mask.bit_count())


def _validate_coordinate(coordinate, field):
    if not isinstance(coordinate, SpatialCoordinate):
        raise PipelineConfigError(f"{field} must be a SpatialCoordinate")
    for name in ("n", "oh", "ow"):
        _require_int_range(
            getattr(coordinate, name), 0, (1 << 64) - 1,
            f"{field}.{name}")


def compact_spatial_payload(raw):
    """Stable raw-lane compaction, independently mirroring the C++ contract."""
    if not isinstance(raw, RawSpatialPayload):
        raise PipelineConfigError("raw must be a RawSpatialPayload")
    if len(raw.activations) != SA_ROWS or len(raw.coordinates) != SA_ROWS:
        raise PipelineConfigError(
            "raw activations and coordinates must each contain 16 lanes")
    _require_int_range(
        raw.spatial_mask, 1, (1 << SA_ROWS) - 1, "raw spatial mask")

    activations = [0] * SA_ROWS
    source_lanes = [0] * SA_ROWS
    coordinates = [None] * SA_ROWS
    seen_coordinates = set()
    destination = 0
    for source in range(SA_ROWS):
        activation = raw.activations[source]
        _require_int_range(activation, 0, 255, f"activation[{source}]")
        valid = bool(raw.spatial_mask & (1 << source))
        coordinate = raw.coordinates[source]
        if not valid:
            if activation != 0 or coordinate is not None:
                raise PipelineConfigError(
                    "invalid raw lanes must use canonical zero/None metadata")
            continue
        _validate_coordinate(coordinate, f"coordinate[{source}]")
        if coordinate in seen_coordinates:
            raise PipelineConfigError(
                "raw spatial coordinates must be unique")
        seen_coordinates.add(coordinate)
        activations[destination] = activation
        source_lanes[destination] = source
        coordinates[destination] = coordinate
        destination += 1

    return CompactedSpatialPayload(
        activations=tuple(activations),
        spatial_mask=_prefix_mask(destination),
        valid_rows=destination,
        source_lanes=tuple(source_lanes),
        coordinates=tuple(coordinates),
    )


def validate_vector_tag(tag, k):
    if not isinstance(tag, StreamingVectorTag):
        raise PipelineConfigError("tag must be a StreamingVectorTag")
    _require_int_range(k, 1, MAX_CHANNELS * 9, "streaming K")
    if k % 9:
        raise PipelineConfigError("streaming K must be a multiple of 9")
    for field in ("tile_index", "oc_group", "valid_columns", "c", "kh",
                  "kw", "k_index"):
        _require_int_range(
            getattr(tag, field), 0, (1 << 64) - 1, field)
    _require_bool(tag.tile_first, "tile_first")
    _require_bool(tag.tile_last, "tile_last")
    if tag.oc_group != 0:
        raise PipelineConfigError("streaming oc_group must be zero")
    if not 1 <= tag.valid_columns <= SA_COLUMNS:
        raise PipelineConfigError(
            "streaming valid_columns must be in [1, 16]")
    if tag.c >= k // 9 or tag.kh >= 3 or tag.kw >= 3 or tag.k_index >= k:
        raise PipelineConfigError("streaming vector tag exceeds K bounds")
    expected_k = tag.c * 9 + tag.kh * 3 + tag.kw
    if tag.k_index != expected_k:
        raise PipelineConfigError("streaming vector tag K order mismatch")
    if (tag.tile_first != (tag.k_index == 0) or
            tag.tile_last != (tag.k_index + 1 == k)):
        raise PipelineConfigError("streaming tile boundary tag mismatch")


def validate_vector_sequence(previous_tag, current_tag, k):
    validate_vector_tag(previous_tag, k)
    validate_vector_tag(current_tag, k)
    if current_tag.tile_index == previous_tag.tile_index:
        if (previous_tag.tile_last or
                current_tag.k_index != previous_tag.k_index + 1):
            raise PipelineConfigError(
                "streaming K sequence is not contiguous within a tile")
        return
    if (not previous_tag.tile_last or not current_tag.tile_first or
            current_tag.tile_index != previous_tag.tile_index + 1):
        raise PipelineConfigError(
            "streaming tile sequence is not contiguous")


def validate_same_tile_metadata(previous_tag, previous_payload,
                                current_tag, current_payload):
    if (not isinstance(previous_payload, CompactedSpatialPayload) or
            not isinstance(current_payload, CompactedSpatialPayload)):
        raise PipelineConfigError(
            "tile payloads must be CompactedSpatialPayload values")
    if (previous_tag.tile_index != current_tag.tile_index or
            previous_tag.oc_group != current_tag.oc_group or
            previous_tag.valid_columns != current_tag.valid_columns or
            previous_payload.spatial_mask != current_payload.spatial_mask or
            previous_payload.valid_rows != current_payload.valid_rows or
            previous_payload.source_lanes != current_payload.source_lanes or
            previous_payload.coordinates != current_payload.coordinates):
        raise PipelineConfigError(
            "streaming metadata changed within one tile")


def decide_elastic_advance(inputs):
    if not isinstance(inputs, ElasticAdvanceInputs):
        raise PipelineConfigError("inputs must be ElasticAdvanceInputs")
    for field in ("s0_valid", "s1_valid", "s2_valid", "s1_can_retire",
                  "fifo_pop", "more_vectors"):
        _require_bool(getattr(inputs, field), field)
    _require_int_range(
        inputs.fifo_count, 0, STREAMING_FIFO_DEPTH, "FIFO count")
    if inputs.fifo_pop and inputs.fifo_count == 0:
        raise PipelineConfigError("streaming FIFO cannot pop while empty")

    fifo_push_ready = (
        inputs.fifo_count < STREAMING_FIFO_DEPTH or inputs.fifo_pop)
    s2_ready = not inputs.s2_valid or fifo_push_ready
    s1_ready = (not inputs.s1_valid or
                (inputs.s1_can_retire and s2_ready))
    s0_ready = not inputs.s0_valid or s1_ready
    return ElasticAdvanceDecision(
        fifo_push_ready=fifo_push_ready,
        s2_ready=s2_ready,
        s1_ready=s1_ready,
        s0_ready=s0_ready,
        producer_ready=s0_ready and inputs.more_vectors,
        s2_to_fifo=inputs.s2_valid and fifo_push_ready,
        s1_to_s2=(inputs.s1_valid and inputs.s1_can_retire and s2_ready),
        s0_to_s1=inputs.s0_valid and s1_ready,
        producer_to_s0=inputs.more_vectors and s0_ready,
    )


def decide_elastic_fifo(count, push_valid, pop_request):
    _require_int_range(count, 0, STREAMING_FIFO_DEPTH, "FIFO count")
    _require_bool(push_valid, "push_valid")
    _require_bool(pop_request, "pop_request")
    if pop_request and count == 0:
        raise PipelineConfigError("streaming FIFO cannot pop while empty")
    push_ready = count < STREAMING_FIFO_DEPTH or pop_request
    push = push_valid and push_ready
    pop = pop_request
    next_count = count + int(push) - int(pop)
    if not 0 <= next_count <= STREAMING_FIFO_DEPTH:
        raise PipelineConfigError("streaming FIFO conservation failed")
    return ElasticFifoDecision(push_ready, push, pop, next_count)


def decide_streaming_consumer(state, fifo_valid, head_tag, active_tile,
                              accepted_k, k):
    if not isinstance(state, StreamingConsumerState):
        raise PipelineConfigError("state must be a StreamingConsumerState")
    _require_bool(fifo_valid, "fifo_valid")
    _require_int_range(active_tile, 0, (1 << 64) - 1, "active_tile")
    _require_int_range(accepted_k, 0, k, "accepted_k")
    begin_launch = launch = pe_ready = input_valid = False
    if state == StreamingConsumerState.IDLE and fifo_valid:
        validate_vector_tag(head_tag, k)
        if not head_tag.tile_first or head_tag.k_index != 0:
            raise PipelineConfigError(
                "idle consumer requires a tile-first FIFO head")
        begin_launch = True
    elif state == StreamingConsumerState.LAUNCH:
        if (not fifo_valid or head_tag.tile_index != active_tile or
                not head_tag.tile_first or head_tag.k_index != 0):
            raise PipelineConfigError(
                "launch consumer must retain the tile-first FIFO head")
        validate_vector_tag(head_tag, k)
        launch = True
    elif state == StreamingConsumerState.ACCEPT_K:
        pe_ready = accepted_k < k
        if fifo_valid:
            validate_vector_tag(head_tag, k)
            if (head_tag.tile_index != active_tile or
                    head_tag.k_index != accepted_k):
                raise PipelineConfigError(
                    "FIFO head does not match active tile/K")
            input_valid = True
    return StreamingConsumerDecision(
        begin_launch=begin_launch,
        launch=launch,
        pe_ready=pe_ready,
        input_valid=input_valid,
        input_fire=input_valid and pe_ready,
    )


def decide_sau_input_cycle(protocol, stream_active, accepted_inputs,
                           calc_cycles, input_fire,
                           previously_scheduled_mac_due):
    if not isinstance(protocol, SauInputProtocol):
        raise PipelineConfigError("protocol must be a SauInputProtocol")
    for value, field in ((stream_active, "stream_active"),
                         (input_fire, "input_fire"),
                         (previously_scheduled_mac_due,
                          "previously_scheduled_mac_due")):
        _require_bool(value, field)
    _require_int_range(calc_cycles, 1, MAX_CHANNELS * 9, "calc_cycles")
    _require_int_range(accepted_inputs, 0, calc_cycles, "accepted_inputs")
    if input_fire and accepted_inputs == calc_cycles:
        raise PipelineConfigError("SA input exceeds configured K")
    if (protocol == SauInputProtocol.STRICT_RTL_CONTINUOUS and
            stream_active and 0 < accepted_inputs < calc_cycles and
            not input_fire):
        raise PipelineConfigError(
            "strict SA input stream cannot contain bubbles")
    return SauInputCycleDecision(
        schedule_new_mac=input_fire,
        commit_previously_scheduled_mac=previously_scheduled_mac_due,
        accepted_next=accepted_inputs + int(input_fire),
    )


def validate_drained_conservation(counts):
    if not isinstance(counts, StreamingConservationCounts):
        raise PipelineConfigError(
            "counts must be StreamingConservationCounts")
    for field in counts.__dataclass_fields__:
        _require_int_range(
            getattr(counts, field), 0, (1 << 64) - 1, field)
    if counts.expected_vectors == 0 or counts.expected_tiles == 0:
        raise PipelineConfigError(
            "streaming expected vector/tile counts must be nonzero")
    if any(value != counts.expected_vectors for value in (
            counts.producer_accepted,
            counts.s2_pushed,
            counts.fifo_pushed,
            counts.fifo_popped,
            counts.pe_accepted)):
        raise PipelineConfigError(
            "streaming vector conservation failed at drained")
    if any(value != counts.expected_tiles for value in (
            counts.tiles_generated,
            counts.tiles_launched,
            counts.tiles_completed)):
        raise PipelineConfigError(
            "streaming tile conservation failed at drained")
