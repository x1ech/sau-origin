#!/usr/bin/env python3
"""Frozen Step 1 contracts for the Im2Col-to-SAU convolution pipeline."""

from dataclasses import asdict, dataclass
from enum import IntEnum
import hashlib
import json

from util.im2col.im2col_contract import (
    ConfigError,
    DerivedConfig as Im2ColDerivedConfig,
    ResolvedConfig as Im2ColResolvedConfig,
    checked_multiply,
    validate_and_derive as validate_and_derive_im2col,
)


SCHEMA_VERSION = 1
SA_ROWS = 16
SA_COLUMNS = 16
KERNEL_HEIGHT = 3
KERNEL_WIDTH = 3
MIN_CHANNELS = 1
MAX_CHANNELS = 63
MIN_OUT_CHANNELS = 1
MAX_OUT_CHANNELS = 16
MIN_CUTBIT = 0
MAX_CUTBIT = 23
CYCLE_ANCHORS_PROVISIONAL = True

WEIGHT_GENERATORS = ("tb_weight_value_v1", "zero", "ones")
BIAS_GENERATORS = ("tb_bias_value_v1", "zero")


class PipelineConfigError(ConfigError):
    """Raised when a resolved pipeline configuration is invalid."""


class PipelineState(IntEnum):
    IDLE = 0
    COLLECT_TILE = 1
    LAUNCH_SA = 2
    STREAM_K = 3
    WAIT_RESULT = 4
    DRAIN_OUTPUT = 5
    DONE = 6


@dataclass(frozen=True)
class ResolvedPipelineConfig:
    schema_version: int
    name: str
    im2col: Im2ColResolvedConfig
    out_channels: int
    cutbit: int
    weight_generator: str
    bias_generator: str


@dataclass(frozen=True)
class DerivedPipelineConfig:
    im2col: Im2ColDerivedConfig
    k: int
    expected_tiles: int
    expected_outputs: int
    expected_macs: int


@dataclass(frozen=True)
class OutputReadyConfig:
    period: int = 1
    high_cycles: int = 1


@dataclass(frozen=True)
class DrainStatus:
    im2col_can_feed: bool
    im2col_fifo_empty: bool
    tile_buffer_empty: bool
    sau_idle: bool
    completed_tiles: int
    written_outputs: int
    output_pending: bool


def _require_integer(value, field):
    if type(value) is not int:
        raise PipelineConfigError(f"{field} must be an integer")


def _require_range(value, minimum, maximum, field):
    _require_integer(value, field)
    if value < minimum or value > maximum:
        raise PipelineConfigError(
            f"{field} must be in [{minimum}, {maximum}]")


def _checked_product(values, description):
    result = 1
    for value in values:
        result = checked_multiply(result, value, description)
    return result


def validate_and_derive(config):
    """Validate a resolved pipeline config and derive all Step 1 counts."""
    if not isinstance(config, ResolvedPipelineConfig):
        raise PipelineConfigError(
            "config must be a ResolvedPipelineConfig")
    _require_range(
        config.schema_version, SCHEMA_VERSION, SCHEMA_VERSION,
        "schema_version")
    if type(config.name) is not str or not config.name:
        raise PipelineConfigError("name must be a non-empty string")

    try:
        im2col = validate_and_derive_im2col(config.im2col)
    except ConfigError as error:
        raise PipelineConfigError(f"im2col.{error}") from error
    if (config.im2col.kernel_h != KERNEL_HEIGHT or
            config.im2col.kernel_w != KERNEL_WIDTH):
        raise PipelineConfigError("im2col kernel_h and kernel_w must both be 3")
    _require_range(
        config.im2col.c, MIN_CHANNELS, MAX_CHANNELS, "im2col.c")
    _require_range(
        config.out_channels, MIN_OUT_CHANNELS, MAX_OUT_CHANNELS,
        "out_channels")
    _require_range(config.cutbit, MIN_CUTBIT, MAX_CUTBIT, "cutbit")
    if config.weight_generator not in WEIGHT_GENERATORS:
        raise PipelineConfigError(
            "weight_generator must be one of: " +
            ", ".join(WEIGHT_GENERATORS))
    if config.bias_generator not in BIAS_GENERATORS:
        raise PipelineConfigError(
            "bias_generator must be one of: " +
            ", ".join(BIAS_GENERATORS))

    k = checked_multiply(
        config.im2col.c, KERNEL_HEIGHT * KERNEL_WIDTH, "pipeline K")
    expected_tiles = _checked_product(
        (config.im2col.n, im2col.h_groups, im2col.w_groups),
        "expected tile count")
    expected_vectors = checked_multiply(
        expected_tiles, k, "expected vector count")
    if expected_vectors != im2col.expected_vectors:
        raise PipelineConfigError(
            "pipeline tile derivation disagrees with Im2Col expected_vectors")
    expected_outputs = _checked_product(
        (config.im2col.n, config.im2col.out_h, config.im2col.out_w,
         config.out_channels),
        "expected output count")
    expected_macs = checked_multiply(
        expected_outputs, k, "expected useful MAC count")
    return DerivedPipelineConfig(
        im2col=im2col,
        k=k,
        expected_tiles=expected_tiles,
        expected_outputs=expected_outputs,
        expected_macs=expected_macs,
    )


def canonical_config_bytes(config):
    validate_and_derive(config)
    text = json.dumps(
        asdict(config), sort_keys=True, separators=(",", ":"),
        ensure_ascii=False,
    )
    return text.encode("utf-8")


def resolved_config_sha256(config):
    return hashlib.sha256(canonical_config_bytes(config)).hexdigest()


def validate_output_ready(config):
    if not isinstance(config, OutputReadyConfig):
        raise PipelineConfigError("ready config must be an OutputReadyConfig")
    _require_range(config.period, 1, (1 << 64) - 1, "output ready period")
    _require_range(
        config.high_cycles, 1, config.period, "output ready high_cycles")


def output_ready(cycle, config=OutputReadyConfig()):
    _require_range(cycle, 0, (1 << 64) - 1, "cycle")
    validate_output_ready(config)
    return cycle % config.period < config.high_cycles


def pe_index(row, column):
    _require_range(row, 0, SA_ROWS - 1, "PE row")
    _require_range(column, 0, SA_COLUMNS - 1, "PE column")
    return row * SA_COLUMNS + column


def pipeline_drained(status, derived):
    if not isinstance(status, DrainStatus):
        raise PipelineConfigError("status must be a DrainStatus")
    if not isinstance(derived, DerivedPipelineConfig):
        raise PipelineConfigError("derived must be a DerivedPipelineConfig")
    for field in ("completed_tiles", "written_outputs"):
        _require_range(getattr(status, field), 0, (1 << 64) - 1, field)
    return (
        not status.im2col_can_feed and
        status.im2col_fifo_empty and
        status.tile_buffer_empty and
        status.sau_idle and
        status.completed_tiles == derived.expected_tiles and
        status.written_outputs == derived.expected_outputs and
        not status.output_pending
    )
