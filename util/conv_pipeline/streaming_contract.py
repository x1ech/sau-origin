#!/usr/bin/env python3
"""Independent contracts for the streaming Im2Col-to-SAU path."""

from dataclasses import asdict, dataclass
from enum import IntEnum
import hashlib
import json

from util.conv_pipeline.pipeline_contract import (
    DerivedPipelineConfig,
    MAX_CHANNELS,
    ResolvedPipelineConfig,
    SA_COLUMNS,
    SA_ROWS,
    PipelineConfigError,
    validate_and_derive,
)


STREAMING_FIFO_DEPTH = 4
SPAD_ROWS = 4096
UINT64_MAX = (1 << 64) - 1


class BankArbitrationPolicy(IntEnum):
    A_D_B = 0


@dataclass(frozen=True)
class SharedSpadConfig:
    a_base: int
    a_rows: int
    b_base: int
    b_rows: int
    c_base: int
    c_rows: int
    d_base: int
    d_rows: int
    b_buffer_depth: int
    d_pending_rows: int
    weight_reuse: bool
    arbitration: str


@dataclass(frozen=True)
class ResolvedStreamingConfig(ResolvedPipelineConfig):
    shared_spad: SharedSpadConfig


@dataclass(frozen=True)
class StreamingDerivedConfig(DerivedPipelineConfig):
    shared_spad: SharedSpadConfig


@dataclass(frozen=True)
class ScratchpadAddress:
    bank: int
    row: int


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


def _checked_add(left, right, description):
    _require_int_range(left, 0, UINT64_MAX, f"{description} left operand")
    _require_int_range(right, 0, UINT64_MAX, f"{description} right operand")
    result = left + right
    if result > UINT64_MAX:
        raise PipelineConfigError(f"{description} overflows uint64")
    return result


def _default_shared_spad(config, derived):
    a_base = config.im2col.spad_base
    a_rows = derived.im2col.total_spatial_words
    b_base = _checked_add(a_base, a_rows, "default B base")
    b_rows = derived.k
    c_base = _checked_add(b_base, b_rows, "default C base")
    c_rows = 2
    d_base = _checked_add(c_base, c_rows, "default D base")
    d_rows = (
        config.im2col.n * config.im2col.out_h * config.im2col.out_w)
    return SharedSpadConfig(
        a_base=a_base,
        a_rows=a_rows,
        b_base=b_base,
        b_rows=b_rows,
        c_base=c_base,
        c_rows=c_rows,
        d_base=d_base,
        d_rows=d_rows,
        b_buffer_depth=derived.k,
        d_pending_rows=1,
        weight_reuse=True,
        arbitration="a_d_b",
    )


def _validate_shared_spad(config, derived, shared):
    if not isinstance(shared, SharedSpadConfig):
        raise PipelineConfigError(
            "shared_spad must be a SharedSpadConfig")
    required = {
        "a": derived.im2col.total_spatial_words,
        "b": derived.k,
        "c": 2,
        "d": (
            config.im2col.n * config.im2col.out_h *
            config.im2col.out_w),
    }
    regions = []
    for name in ("a", "b", "c", "d"):
        base = getattr(shared, f"{name}_base")
        rows = getattr(shared, f"{name}_rows")
        _require_int_range(base, 0, SPAD_ROWS - 1, f"shared_spad.{name}_base")
        _require_int_range(
            rows, required[name], SPAD_ROWS,
            f"shared_spad.{name}_rows")
        end = _checked_add(base, rows, f"shared_spad.{name} end")
        if end > SPAD_ROWS:
            raise PipelineConfigError(
                f"shared_spad.{name} region exceeds 4096 rows")
        regions.append((base, end, name))
    if shared.a_base != config.im2col.spad_base:
        raise PipelineConfigError(
            "shared_spad.a_base must equal im2col.spad_base")
    regions.sort()
    for previous, current in zip(regions, regions[1:]):
        if current[0] < previous[1]:
            raise PipelineConfigError(
                "shared_spad regions overlap: " +
                f"{previous[2]} and {current[2]}")
    _require_int_range(
        shared.b_buffer_depth, 1, derived.k,
        "shared_spad.b_buffer_depth")
    _require_int_range(
        shared.d_pending_rows, 1, UINT64_MAX,
        "shared_spad.d_pending_rows")
    if type(shared.weight_reuse) is not bool:
        raise PipelineConfigError("shared_spad.weight_reuse must be a bool")
    if shared.arbitration != "a_d_b":
        raise PipelineConfigError(
            "shared_spad.arbitration must be 'a_d_b'")
    return shared


def resolve_shared_spad(config, overrides=None):
    """Resolve optional streaming-only shared scratchpad parameters."""
    if not isinstance(config, ResolvedPipelineConfig):
        raise PipelineConfigError(
            "config must be a ResolvedPipelineConfig")
    derived = validate_and_derive(config)
    defaults = _default_shared_spad(config, derived)
    if overrides is None:
        shared = defaults
    else:
        if type(overrides) is not dict:
            raise PipelineConfigError("shared_spad must be a JSON object")
        allowed = set(asdict(defaults))
        unknown = sorted(set(overrides) - allowed)
        if unknown:
            raise PipelineConfigError(
                "shared_spad has unknown fields: " + ", ".join(unknown))
        values = asdict(defaults)
        values.update(overrides)
        shared = SharedSpadConfig(**values)
    return _validate_shared_spad(config, derived, shared)


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
    shared = (
        config.shared_spad
        if isinstance(config, ResolvedStreamingConfig)
        else resolve_shared_spad(config)
    )
    _validate_shared_spad(config, derived, shared)
    return StreamingDerivedConfig(
        im2col=derived.im2col,
        k=derived.k,
        expected_tiles=derived.expected_tiles,
        expected_outputs=derived.expected_outputs,
        expected_macs=derived.expected_macs,
        shared_spad=shared,
    )


def streaming_canonical_config_bytes(config):
    if not isinstance(config, ResolvedStreamingConfig):
        raise PipelineConfigError(
            "config must be a ResolvedStreamingConfig")
    validate_streaming_config(config)
    return json.dumps(
        asdict(config), sort_keys=True, separators=(",", ":"),
        ensure_ascii=False,
    ).encode("utf-8")


def streaming_resolved_config_sha256(config):
    return hashlib.sha256(streaming_canonical_config_bytes(config)).hexdigest()


def b_address(config, k_index, output_channel):
    derived = validate_streaming_config(config)
    _require_int_range(k_index, 0, derived.k - 1, "B k_index")
    _require_int_range(
        output_channel, 0, config.out_channels - 1, "B output_channel")
    return ScratchpadAddress(
        output_channel, derived.shared_spad.b_base + k_index)


def c_address(config, output_channel, byte_index):
    derived = validate_streaming_config(config)
    _require_int_range(
        output_channel, 0, config.out_channels - 1, "C output_channel")
    _require_int_range(byte_index, 0, 1, "C byte_index")
    return ScratchpadAddress(
        output_channel, derived.shared_spad.c_base + byte_index)


def d_address(config, n, oh, ow, output_channel):
    derived = validate_streaming_config(config)
    _require_int_range(n, 0, config.im2col.n - 1, "D n")
    _require_int_range(oh, 0, config.im2col.out_h - 1, "D oh")
    _require_int_range(ow, 0, config.im2col.out_w - 1, "D ow")
    _require_int_range(
        output_channel, 0, config.out_channels - 1, "D output_channel")
    spatial = (
        (n * config.im2col.out_h + oh) * config.im2col.out_w + ow)
    return ScratchpadAddress(
        output_channel, derived.shared_spad.d_base + spatial)


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
